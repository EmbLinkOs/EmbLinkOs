#!/usr/bin/env python3
"""sbsign.py -- put an Authenticode signature on an EFI application.

    python3 tools/sbsign.py in.efi out.efi key.pem cert.pem

A machine with Secure Boot switched on -- which is how retail hardware leaves
the factory -- will not launch an unsigned EFI application at all. It does not
warn: the firmware says "Access Denied" and moves to the next boot option, and
a person who has just written our image to a USB stick sees a machine that
ignores it.

WHAT A SIGNED PE IS. The signature is a PKCS#7 blob appended to the end of the
file, pointed at by the Certificate Table entry in the optional header's data
directory. What it signs is not the file: it is an "Authenticode hash", which
deliberately SKIPS the three things that change when a file is signed --

    the optional header's CheckSum      (recomputed by whoever writes the file)
    the Certificate Table directory     (points AT the signature)
    the appended signature itself

-- so that a signed file hashes to the same value as the unsigned one it was
made from. Getting any of those three wrong produces a file that verifies
perfectly against a hash nobody else computes, and the only symptom is the same
"Access Denied" as no signature at all.

The PKCS#7 itself is built by openssl, which is on both hosts this tree is
built on. What openssl cannot do is the PE hash or the container, and those are
the parts here.
"""
import hashlib
import os
import struct
import subprocess
import sys
import tempfile

CERT_TABLE_DIR = 4          # data directory index of the Certificate Table
WIN_CERT_TYPE_PKCS_SIGNED_DATA = 0x0002
WIN_CERT_REVISION_2_0 = 0x0200

# Microsoft's OIDs for the Authenticode content. The signature is over a
# SpcIndirectDataContent, not over the digest directly.
SPC_INDIRECT_DATA_OBJID = "1.3.6.1.4.1.311.2.1.4"
SPC_PE_IMAGE_DATA_OBJID = "1.3.6.1.4.1.311.2.1.15"


# ---- a very small DER writer ---------------------------------------------
def der_len(n):
    if n < 0x80:
        return bytes([n])
    b = n.to_bytes((n.bit_length() + 7) // 8, "big")
    return bytes([0x80 | len(b)]) + b


def der(tag, body):
    return bytes([tag]) + der_len(len(body)) + body


def der_oid(dotted):
    parts = [int(x) for x in dotted.split(".")]
    out = bytearray([40 * parts[0] + parts[1]])
    for p in parts[2:]:
        if p == 0:
            out.append(0)
            continue
        chunks = []
        while p:
            chunks.append(p & 0x7F)
            p >>= 7
        for i, c in enumerate(reversed(chunks)):
            out.append(c | (0x80 if i < len(chunks) - 1 else 0))
    return der(0x06, bytes(out))


def spc_indirect_data(digest):
    """SpcIndirectDataContent: what Authenticode actually signs.

    The `file` field is the string "<<<Obsolete>>>" in UCS-2 -- not a
    placeholder this script invented, but what every signer puts there. The
    field once named the file and has been meaningless for twenty years."""
    obsolete = "<<<Obsolete>>>".encode("utf-16-be")
    # [0] IMPLICIT BMPString -- PRIMITIVE (0x80), not constructed. Tagging it
    # 0xA0 produces a structure that every parser walks INTO and chokes on,
    # and the firmware's is no more forgiving than openssl's.
    spc_string = der(0x80, obsolete)                 # SpcString [0] unicode
    spc_link = der(0xA2, spc_string)                 # SpcLink [2] file
    spc_pe_image_data = der(0x30, der(0x03, b"\x00") + spc_link)
    attr = der(0x30, der_oid(SPC_PE_IMAGE_DATA_OBJID) + spc_pe_image_data)

    alg = der(0x30, der_oid("2.16.840.1.101.3.4.2.1") + der(0x05, b""))  # sha256
    digest_info = der(0x30, alg + der(0x04, digest))
    return der(0x30, attr + digest_info)


# ---- the PE side ----------------------------------------------------------
def pe_offsets(data):
    if data[0:2] != b"MZ":
        raise SystemExit("sbsign: not a PE file (no MZ)")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise SystemExit("sbsign: no PE signature at e_lfanew")
    opt = pe + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    if magic != 0x20B:
        raise SystemExit("sbsign: expected PE32+ (0x20b), got 0x%x" % magic)
    nsections = struct.unpack_from("<H", data, pe + 6)[0]
    checksum_off = opt + 64
    size_of_headers = struct.unpack_from("<I", data, opt + 60)[0]
    nrva = struct.unpack_from("<I", data, opt + 108)[0]
    if nrva <= CERT_TABLE_DIR:
        raise SystemExit("sbsign: image has no Certificate Table directory slot")
    dirs = opt + 112
    cert_dir_off = dirs + CERT_TABLE_DIR * 8
    sect_table = opt + struct.unpack_from("<H", data, pe + 20)[0]
    return {
        "pe": pe, "opt": opt, "nsections": nsections,
        "checksum": checksum_off, "size_of_headers": size_of_headers,
        "cert_dir": cert_dir_off, "sections": sect_table,
    }


def authenticode_hash(data, o):
    """The PE hash, with the three signing-related regions skipped."""
    h = hashlib.sha256()
    h.update(data[0:o["checksum"]])
    h.update(data[o["checksum"] + 4:o["cert_dir"]])
    h.update(data[o["cert_dir"] + 8:o["size_of_headers"]])

    # Sections in FILE order, which is not always table order.
    sects = []
    for i in range(o["nsections"]):
        e = o["sections"] + i * 40
        raw_size, raw_ptr = struct.unpack_from("<II", data, e + 16)
        if raw_size:
            sects.append((raw_ptr, raw_size))
    sects.sort()
    hashed = o["size_of_headers"]
    for ptr, size in sects:
        h.update(data[ptr:ptr + size])
        hashed += size

    # ANYTHING AFTER THE LAST SECTION is hashed too -- except an existing
    # certificate table, which is what we are about to replace.
    cert_rva, cert_size = struct.unpack_from("<II", data, o["cert_dir"])
    if len(data) > hashed:
        h.update(data[hashed:len(data) - cert_size])
    return h.digest()


# ---- a very small DER reader, for pulling two fields out of the certificate -
def der_read(buf, pos):
    """Return (tag, body_start, body_end, next) for the element at `pos`."""
    tag = buf[pos]
    n = buf[pos + 1]
    if n < 0x80:
        start = pos + 2
        length = n
    else:
        k = n & 0x7F
        length = int.from_bytes(buf[pos + 2:pos + 2 + k], "big")
        start = pos + 2 + k
    return tag, start, start + length, start + length


def cert_issuer_and_serial(der_cert):
    """Certificate ::= SEQUENCE { tbsCertificate, sigAlg, sigValue }
       tbsCertificate ::= SEQUENCE { [0] version OPTIONAL, serialNumber,
                                     signature, issuer, ... }

    Read rather than shelled out for, because `openssl x509 -issuer` prints a
    HUMAN-READABLE name and what PKCS#7 needs is the exact DER bytes -- a name
    re-encoded from its text form is a different sequence of bytes and matches
    nothing."""
    _, s0, _, _ = der_read(der_cert, 0)            # Certificate
    tag, s1, e1, _ = der_read(der_cert, s0)        # tbsCertificate
    p = s1
    tag, bs, be, nxt = der_read(der_cert, p)
    if tag == 0xA0:                                 # [0] version
        p = nxt
        tag, bs, be, nxt = der_read(der_cert, p)
    serial = der_cert[p:nxt]                        # INTEGER, with its header
    p = nxt
    _, _, _, nxt = der_read(der_cert, p)            # signature AlgorithmIdentifier
    p = nxt
    _, _, _, nxt = der_read(der_cert, p)            # issuer Name
    issuer = der_cert[p:nxt]
    return issuer, serial


def pkcs7_sign(content_der, key, cert_pem):
    """Build the Authenticode PKCS#7 by hand.

    OPENSSL'S CMS OUTPUT IS NOT THIS, and the difference is why the first
    attempt was refused with the same Access Denied as no signature at all:

      * CMS numbers SignedData version 3 when the content type is not id-data.
        Authenticode is PKCS#7 1.5 and wants version 1.
      * CMS always wraps eContent in an OCTET STRING. Authenticode embeds the
        SpcIndirectDataContent SEQUENCE DIRECTLY -- and the firmware reads the
        digest by walking that SEQUENCE's own header, so an extra layer puts
        every offset out.

    openssl still does the one thing worth delegating: the RSA signature."""
    cert_der = subprocess.run(
        ["openssl", "x509", "-in", cert_pem, "-outform", "DER"],
        capture_output=True, check=True).stdout
    issuer, serial = cert_issuer_and_serial(cert_der)

    sha256_alg = der(0x30, der_oid("2.16.840.1.101.3.4.2.1") + der(0x05, b""))
    rsa_alg    = der(0x30, der_oid("1.2.840.113549.1.1.1") + der(0x05, b""))

    # AUTHENTICATED ATTRIBUTES. The signature is over these, not over the
    # content -- the content is bound in by the messageDigest attribute. Every
    # real signer emits them and the firmware's verifier expects the shape.
    attr_ct = der(0x30, der_oid("1.2.840.113549.1.9.3") +
                  der(0x31, der_oid(SPC_INDIRECT_DATA_OBJID)))
    # THE CONTENT THE SIGNATURE COVERS IS THE SEQUENCE'S VALUE, NOT ITS TLV.
    #
    # The firmware walks past the SpcIndirectDataContent's own tag and length
    # and hands the bytes INSIDE it to the PKCS#7 verifier -- so the
    # messageDigest attribute has to be the digest of exactly those bytes.
    # Hashing the whole element instead produces a signature that is perfectly
    # valid over something nobody computes, and OpenSSL (which is what the
    # firmware verifies with) reports it as "digest failure" while the
    # firmware reports it as Access Denied and says nothing else.
    _, cstart, cend, _ = der_read(content_der, 0)
    content_value = content_der[cstart:cend]
    attr_md = der(0x30, der_oid("1.2.840.113549.1.9.4") +
                  der(0x31, der(0x04, hashlib.sha256(content_value).digest())))
    # Sorted by encoding, as DER requires of a SET OF.
    attrs = sorted([attr_ct, attr_md])
    signed_attrs_for_signing = der(0x31, b"".join(attrs))     # SET, for the hash
    signed_attrs_in_message  = der(0xA0, b"".join(attrs))     # [0] IMPLICIT

    with tempfile.TemporaryDirectory() as td:
        tosign = os.path.join(td, "attrs.der")
        open(tosign, "wb").write(signed_attrs_for_signing)
        r = subprocess.run(["openssl", "dgst", "-sha256", "-sign", key,
                            "-out", os.path.join(td, "sig.bin"), tosign],
                           capture_output=True)
        if r.returncode != 0:
            raise SystemExit("sbsign: openssl dgst failed:\n" +
                             r.stderr.decode("utf-8", "replace"))
        signature = open(os.path.join(td, "sig.bin"), "rb").read()

    signer_info = der(0x30,
                      der(0x02, b"\x01") +                     # version 1
                      der(0x30, issuer + serial) +
                      sha256_alg +
                      signed_attrs_in_message +
                      rsa_alg +
                      der(0x04, signature))

    content_info = der(0x30,
                       der_oid(SPC_INDIRECT_DATA_OBJID) +
                       der(0xA0, content_der))                 # DIRECTLY, no OCTET STRING

    signed_data = der(0x30,
                      der(0x02, b"\x01") +                     # version 1
                      der(0x31, sha256_alg) +                  # digestAlgorithms
                      content_info +
                      der(0xA0, cert_der) +                    # [0] certificates
                      der(0x31, signer_info))                  # signerInfos

    return der(0x30, der_oid("1.2.840.113549.1.7.2") + der(0xA0, signed_data))


def sign(src, dst, key, cert):
    data = bytearray(open(src, "rb").read())
    o = pe_offsets(data)

    # Drop any existing signature first, so re-signing is idempotent rather
    # than each run appending another blob nobody looks at.
    cert_rva, cert_size = struct.unpack_from("<II", data, o["cert_dir"])
    if cert_size and cert_rva and cert_rva + cert_size <= len(data):
        del data[cert_rva:cert_rva + cert_size]
        struct.pack_into("<II", data, o["cert_dir"], 0, 0)

    # PAD TO 8 BEFORE HASHING, not after. The certificate table has to start on
    # an 8-byte boundary, so a file whose length is not a multiple of 8 gains
    # padding bytes -- and those bytes land in the trailing region that the
    # Authenticode hash COVERS. Padding afterwards produces a signature over a
    # file that no longer exists, and the firmware reports it exactly the same
    # way as no signature at all: Access Denied, nothing else.
    data += b"\x00" * ((-len(data)) % 8)

    digest = authenticode_hash(data, o)
    p7 = pkcs7_sign(spc_indirect_data(digest), key, cert)

    # WIN_CERTIFICATE, 8-byte aligned. dwLength COUNTS THE HEADER, and the
    # padding is inside the length -- a signature whose length excludes it is
    # rejected with the same Access Denied as no signature at all.
    body = p7 + b"\x00" * ((-len(p7)) % 8)
    hdr = struct.pack("<IHH", 8 + len(body), WIN_CERT_REVISION_2_0,
                      WIN_CERT_TYPE_PKCS_SIGNED_DATA)
    wincert = hdr + body

    cert_off = len(data)          # already 8-aligned, and already hashed
    data += wincert
    struct.pack_into("<II", data, o["cert_dir"], cert_off, len(wincert))

    open(dst, "wb").write(bytes(data))
    print("sbsign: %s -> %s" % (os.path.basename(src), os.path.basename(dst)))
    print("sbsign:   authenticode sha256 %s" % digest.hex())
    print("sbsign:   signature %d bytes at +0x%x" % (len(wincert), cert_off))


def main(argv):
    if len(argv) != 4:
        print(__doc__)
        return 2
    sign(argv[0], argv[1], argv[2], argv[3])
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
