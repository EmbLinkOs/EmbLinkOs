#!/usr/bin/env python3
"""pkgsign -- sign a package manifest with the build key (PK3).

The signed message is the manifest's CANONICAL bytes: the file with any
`signature:` line removed (so signing is idempotent and the signature covers
name/version/abi/build_id/caps/namespace/provides -- everything). We sign
SHA-256(canonical) with ECDSA P-256 and append `signature: <r||s hex>`. The OS's
`pkg` verifies that against the trusted public key baked into user/pkg/pkgkey.h
(ecdsa_verify) -- trust is in the signature, not the channel.

    pkgsign.py <manifest.pkg> <privkey.pem>       # sign in place
    pkgsign.py --emit-key <privkey.pem> <out.h>   # regenerate the pubkey header
"""
import sys
from cryptography.hazmat.primitives.serialization import load_pem_private_key
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature

def canonical(text):
    """The manifest bytes with any `signature:` line dropped (kept as bytes)."""
    out = []
    for line in text.splitlines(keepends=True):
        if line.lstrip().startswith("signature:"):
            continue
        out.append(line)
    b = "".join(out).encode()
    if b and not b.endswith(b"\n"):
        b += b"\n"
    return b

def emit_key(priv, outh):
    k = load_pem_private_key(open(priv, "rb").read(), None)
    n = k.public_key().public_numbers()
    x, y = n.x.to_bytes(32, "big"), n.y.to_bytes(32, "big")
    carr = lambda b: ",".join(f"0x{c:02x}" for c in b)
    open(outh, "w").write(
        "#ifndef EMBK_PKG_KEY_H\n#define EMBK_PKG_KEY_H\n"
        "/* Trusted package-signing public key (ECDSA P-256). pkg verifies every\n"
        " * manifest signature against THIS key. Regenerate with pkgsign.py --emit-key. */\n"
        "#include <stdint.h>\n\n"
        f"static const uint8_t PKG_SIGN_QX[32] = {{ {carr(x)} }};\n"
        f"static const uint8_t PKG_SIGN_QY[32] = {{ {carr(y)} }};\n\n"
        "#endif\n")
    print(f"pkgsign: wrote {outh}")

def sign(manifest, priv):
    k = load_pem_private_key(open(priv, "rb").read(), None)
    text = open(manifest).read()
    msg = canonical(text)
    der = k.sign(msg, ec.ECDSA(hashes.SHA256()))
    r, s = decode_dss_signature(der)
    sig = r.to_bytes(32, "big").hex() + s.to_bytes(32, "big").hex()   # r||s, 128 hex
    open(manifest, "wb").write(msg + f"signature: {sig}\n".encode())
    print(f"pkgsign: signed {manifest} (r||s, {len(sig)} hex)")

def main():
    if len(sys.argv) == 4 and sys.argv[1] == "--emit-key":
        emit_key(sys.argv[2], sys.argv[3])
    elif len(sys.argv) == 3:
        sign(sys.argv[1], sys.argv[2])
    else:
        sys.exit("usage: pkgsign.py <manifest.pkg> <privkey.pem> | --emit-key <priv.pem> <out.h>")

if __name__ == "__main__":
    main()
