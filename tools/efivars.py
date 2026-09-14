#!/usr/bin/env python3
"""efivars.py -- enroll Secure Boot keys into a UEFI variable store.

    python3 tools/efivars.py OVMF_VARS.fd out.fd PK.der [KEK.der [db.der ...]]

WHY THIS EXISTS. A firmware with Secure Boot compiled in does not enforce
anything until a Platform Key is enrolled: with no PK it is in SETUP MODE, and
it will launch an unsigned image quite happily. That is the state OVMF's stock
variable store ships in, and it is why our unsigned loader booted straight to
the desktop under the "secure" firmware -- a result that looks like Secure Boot
passing and is actually Secure Boot switched off.

So a test of Secure Boot has to enroll a key first, and the tools that normally
do that (virt-fw-vars, EnrollDefaultKeys, the firmware's own setup menu) are
either absent on this host or need a human at a screen. The variable store is a
documented binary format, so this writes it -- the same choice tools/mkfat32.py
and tools/mkuefidisk.py make about FAT32 and GPT.

WHAT A VARIABLE STORE IS: a firmware volume header, then a variable-store
header, then packed variable records, each a 60-byte header followed by a
UCS-2 name and the data, 4-byte aligned.

WHY WRITING PK DIRECTLY IS LEGITIMATE and not a way around the signature
checks: the firmware authenticates an authenticated variable when somebody
calls SetVariable at runtime. The store itself is trusted storage -- on a real
machine it is in flash behind SMM, and whoever can rewrite it has already won.
Handing QEMU a different flash image is the emulator's equivalent of standing
in front of the machine with a programmer, which is exactly the position the
owner of a computer is supposed to be in.
"""
import struct
import sys
import uuid

# The variable store this understands. The OTHER one (gEfiVariableGuid) is the
# non-authenticated format, whose records are 32 bytes rather than 60 and which
# cannot hold PK at all -- a firmware built without Secure Boot uses it, and
# writing keys into it would produce a file that loads and enforces nothing.
AUTH_VARSTORE_GUID = uuid.UUID("aaf32c78-947b-439a-a180-2e144ec37792")
FV_NVDATA_GUID     = uuid.UUID("fff12b8d-7696-4c8b-a985-2747075b4f50")

EFI_GLOBAL_VARIABLE   = uuid.UUID("8be4df61-93ca-11d2-aa0d-00e098032b8c")
EFI_IMAGE_SECURITY_DB = uuid.UUID("d719b2cb-3d3a-4596-a3bc-dad00e67656f")
EFI_CERT_X509_GUID    = uuid.UUID("a5c059a1-94e4-4aa7-87b5-ab155c2bf072")

VAR_ADDED = 0x3F
# NON_VOLATILE | BOOTSERVICE_ACCESS | RUNTIME_ACCESS | TIME_BASED_AUTH_WRITE
ATTRS_AUTH = 0x01 | 0x02 | 0x04 | 0x20


def signature_list(der, owner=None):
    """Wrap a DER certificate as an EFI_SIGNATURE_LIST holding one X509 entry."""
    owner = owner or uuid.UUID(int=0)
    sig_size = 16 + len(der)
    total = 16 + 4 + 4 + 4 + sig_size
    return (EFI_CERT_X509_GUID.bytes_le
            + struct.pack("<III", total, 0, sig_size)
            + owner.bytes_le + der)


def var_record(name, guid, data, attrs=ATTRS_AUTH):
    """One AUTHENTICATED_VARIABLE_HEADER plus its name and data.

    The timestamp is left ZERO on purpose. It is the replay-protection stamp
    for authenticated writes, and a record placed into the store directly was
    never written through SetVariable, so there is no earlier write for it to
    be newer than."""
    nm = name.encode("utf-16-le") + b"\x00\x00"
    hdr = struct.pack("<HBBIQ", 0x55AA, VAR_ADDED, 0, attrs, 0)
    hdr += b"\x00" * 16                       # EFI_TIME TimeStamp
    hdr += struct.pack("<III", 0, len(nm), len(data))
    hdr += guid.bytes_le
    assert len(hdr) == 60, len(hdr)
    rec = hdr + nm + data
    return rec + b"\xff" * ((-len(rec)) % 4)   # pad to 4, with erased flash


def store_offsets(fd):
    """Find the variable store inside the firmware volume, and where its
    records start and end."""
    if fd[40:44] != b"_FVH":
        raise SystemExit("efivars: not a firmware volume (no _FVH signature)")
    guid = uuid.UUID(bytes_le=bytes(fd[16:32]))
    if guid != FV_NVDATA_GUID:
        raise SystemExit("efivars: firmware volume is %s, not the NV data one" % guid)
    hdrlen = struct.unpack_from("<H", fd, 48)[0]

    vs = hdrlen
    sig = uuid.UUID(bytes_le=bytes(fd[vs:vs + 16]))
    if sig != AUTH_VARSTORE_GUID:
        raise SystemExit(
            "efivars: this store is %s -- not the AUTHENTICATED variable store.\n"
            "         A firmware built without Secure Boot uses the other format "
            "and cannot hold a PK." % sig)
    size, fmt, state = struct.unpack_from("<IBB", fd, vs + 16)
    if fmt != 0x5A or state != 0xFE:
        raise SystemExit("efivars: variable store is not formatted+healthy "
                         "(format=0x%02x state=0x%02x)" % (fmt, state))
    return vs + 28, vs + size


def existing_end(fd, first, limit):
    """Walk the records already present and return where to append."""
    p = first
    n = 0
    while p + 60 <= limit:
        start, state = struct.unpack_from("<HB", fd, p)
        if start != 0x55AA:
            break
        namesz, datasz = struct.unpack_from("<II", fd, p + 36)
        p += 60 + namesz + datasz
        p = (p + 3) & ~3
        n += 1
    return p, n


def enroll(src, dst, pk=None, kek=None, dbs=()):
    fd = bytearray(open(src, "rb").read())
    first, limit = store_offsets(fd)
    p, existing = existing_end(fd, first, limit)
    print("efivars: store holds %d variable(s); appending at +0x%x" % (existing, p))

    to_add = []
    if pk:
        to_add.append(("PK", EFI_GLOBAL_VARIABLE, signature_list(pk)))
    if kek:
        to_add.append(("KEK", EFI_GLOBAL_VARIABLE, signature_list(kek)))
    if dbs:
        # ONE list holding every allowed certificate, not one variable each:
        # db is a single variable and a second write would replace the first.
        payload = b"".join(signature_list(d) for d in dbs)
        to_add.append(("db", EFI_IMAGE_SECURITY_DB, payload))

    for name, guid, data in to_add:
        rec = var_record(name, guid, data)
        if p + len(rec) > limit:
            raise SystemExit("efivars: no room left in the variable store")
        fd[p:p + len(rec)] = rec
        print("efivars:   %-4s %s  %d bytes" % (name, guid, len(data)))
        p += len(rec)

    open(dst, "wb").write(bytes(fd))
    print("efivars: wrote %s (%d bytes) -- the firmware will leave setup mode"
          % (dst, len(fd)))


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    src, dst = argv[0], argv[1]
    certs = [open(a, "rb").read() for a in argv[2:]]
    pk = certs[0]
    kek = certs[1] if len(certs) > 1 else certs[0]
    dbs = certs[2:] if len(certs) > 2 else [certs[0]]
    enroll(src, dst, pk, kek, dbs)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
