# TLS — our own, from the crypto up

*Design record for the TLS campaign: give EmbLinkOS an HTTPS-capable TLS stack,
grown on our own crypto rather than ported. The gate to the real internet — pip,
git-over-HTTPS, the package registry, online self-update. Nothing here is built
yet beyond what each phase marks ✅.*

## 0. Thesis

> **TLS is a userspace library over our sockets, using our own crypto. We target
> TLS 1.3 only, and we build it bottom-up: crypto the handshake needs → an
> (unverified) encrypted handshake with a real server → certificate verification
> → an HTTPS client → the consumers (pip, git, the registry).**

We already have the hard-won half: SHA-256, HMAC, AES, PBKDF2 (`kernel/crypto/`),
TCP/DNS/DHCP, and a working HTTP client. TLS is the missing layer between "we can
open a TCP socket" and "we can talk to pypi.org."

## 1. Decisions

### 1.1 TLS 1.3 only (RFC 8446)
No TLS 1.2. Rationale: 1.3 is a cleaner, smaller target — one handshake shape,
mandatory AEAD, ECDHE-only key exchange (no RSA key transport), no CBC, no
renegotiation, no compression. All the legacy attack surface (BEAST, CRIME,
Lucky13, RSA padding oracles) is *absent by construction*. pypi.org, github.com,
and effectively the entire modern web speak 1.3. **Cost:** a server that is
1.3-incapable won't connect — acceptable for a from-scratch stack aimed at the
current internet; 1.2 can be added later if a real target forces it.

### 1.2 Userspace library, not in-kernel
TLS lives in **`user/lib/tls/`** (a `libtls`), over the socket syscalls
(`embk_net_*`). Not in the kernel: TLS needs no privilege, and a userspace TLS
keeps the kernel minimal and the failure blast-radius per-process — the same
reason Linux does TLS in userland. A process links `libtls`, holds `CAP_NETWORK`,
and the kernel never grows a TLS attack surface.

### 1.3 One crypto codebase, compiled twice
The crypto in `kernel/crypto/` is pure C (its only kernel ties are `include/
types.h`, `kstring.h`, `kprintf.h`). We provide tiny userspace shims for those
three and **compile the same `sha256.c`/`aes.c`/`hmac.c` for userspace** — one
implementation, kernel and user, no fork. New crypto TLS needs (below) is written
in `user/lib/tls/crypto/` and can be lifted into the kernel later if wanted.

### 1.4 First ciphersuite: `TLS_AES_128_GCM_SHA256`
The one suite TLS 1.3 **mandates** every implementation support → guaranteed
interop with any 1.3 server. Needs: AES-128 (generalize our AES-256 key
schedule), **AES-GCM** (GHASH over our AES), SHA-256 (have), HKDF-SHA256. Add
`TLS_CHACHA20_POLY1305_SHA256` (self-contained, great on cores without AES-NI) and
`TLS_AES_256_GCM_SHA384` afterward.

### 1.5 Key exchange: X25519
Universal in TLS 1.3, and the cleanest to implement correctly (a well-specified
Montgomery-ladder over one prime field, ~300 lines, constant-time by shape).
P-256 later only if a target requires it.

### 1.6 Split by concern (standing house rule)
`user/lib/tls/` from the start:
```
tls.h                 the public contract (tls_connect/read/write/close)
tls.c                 the client state machine glue
record.c              the TLS record layer (frag/AEAD-seal/open, seq nums)
handshake.c           ClientHello..Finished, the 1.3 key schedule
crypto/  gcm.c  aead.c  hkdf.c  x25519.c   (+ shims to reuse sha256/aes/hmac)
x509/    asn1.c  cert.c  rsa.c  ecdsa.c  trust.c   (phase T3)
```
Shared header = contract; one file per concern; subdirs for `crypto`/`x509`.

## 2. The stack, bottom to top

```
  https_get / wget / pip ssl / git / pkg        <- consumers (T4, T5)
  ────────────────────────────────────────
  tls.c   handshake.c   record.c                <- TLS 1.3 (T2), certs (T3)
  ────────────────────────────────────────
  hkdf  x25519  aes-gcm  (sha256 aes hmac reused)  <- handshake crypto (T1)
  ────────────────────────────────────────
  embk_net_socket/connect/send/recv             <- our TCP (have)
```

## 3. Phasing

- **T1 — handshake crypto. ✅ DONE.** AES-128 + **AES-GCM** (AEAD),
  **HKDF-SHA256**, and **X25519**, each unit-tested against RFC test vectors
  (RFC 5869 HKDF, SP 800-38D GCM, RFC 7748 X25519). The kernel's `sha256`/`hmac`/
  `aes` are reused verbatim in userspace via the kshim; AES was generalized to
  AES-128 (kernel `aes256_*` byte-identical, FIPS C.3 selftest guards it). Green
  two ways: `make test-tls-crypto` runs every vector on the host (no boot), and
  `test tls crypto` runs the same vectors **on the metal** — both report OK
  (HKDF PRK/OKM, GCM ciphertext+tag+forged-tag-rejection, X25519 §5.2 KATs + the
  full §6.1 DH agreement). Self-contained; no network.
    - Files: `user/lib/tls/crypto/{hkdf,gcm,x25519,selftest}.c`, the kshim under
      `user/lib/tls/kshim/`, host tests `tools/tls/test_*.c`.
- **T2 — the 1.3 handshake + record layer, NO cert verification. ✅ DONE.**
  ClientHello (X25519 key_share, the one suite) → ServerHello → derive the
  handshake secrets (the 1.3 key schedule) → AEAD-open EncryptedExtensions /
  Certificate / CertificateVerify / Finished → verify the server Finished MAC →
  send our Finished → exchange encrypted application data. Certificates are
  *parsed but not verified* (accept any) — INSECURE, explicitly, for the
  milestone. **Green, on the metal:** `test tls` drove a full TLS 1.3 handshake
  against **cloudflare.com:443** — server Finished verified (which proves the
  ECDHE + key schedule + transcript + record layer are all byte-correct), then an
  encrypted HTTPS GET returned `HTTP/1.1 301` and 1166 bytes of decrypted app
  data. **The OS speaks TLS.**
    - `user/lib/tls/{handshake,tls}.{c,h}`, on-OS driver `user/bin/tlstest.c`
      (packed at `/data/apps/tlstest/`, CAP_NETWORK + RDRAND); host message tests
      `tools/tls/test_handshake.c`. libtls links the kernel crypto compiled for
      userspace (the kshim), same one-codebase trick as T1.
    - Known v1 gaps (fine at this milestone): cert **not** verified (→ T3), no
      HelloRetryRequest, no KeyUpdate, one suite (AES-128-GCM), blocking API.
- **T3 — certificate verification (makes it real). ✅ DONE (EC path).** A DER/
  ASN.1 reader, X.509 v3 parsing, **ECDSA P-256 + P-384** verify (own Montgomery
  bignum + Jacobian curve math) with **SHA-256/384**, a bundled **trust store**
  (GTS Root R4), chain building to the anchor, validity dates, and **hostname/
  SAN** matching -- plus the **CertificateVerify** signature check. All wired into
  the handshake: `tls_connect` refuses before sending Finished if anything fails.
  **Green, on the metal:** `test tls` verified Cloudflare's real chain (leaf ←
  WE1 ← GTS Root R4) to the anchor + host + CertificateVerify, then read
  `HTTP/1.1 301`; host suites also prove wrong-host / expired / tampered are
  refused. Now it's TLS, not "encrypted to someone."
    - `user/lib/tls/crypto/{sha512,bignum,ecdsa}.{c,h}`, `user/lib/tls/x509/
      {asn1,cert,trust}.{c,h}` + roots.h; host tests test_{asn1,sha512,bignum,
      ecdsa,x509,chain}.c against OpenSSL-generated vectors + a frozen real chain.
- **T3.5 — RSA. ✅ DONE.** RSASSA-PKCS1-v1.5 (RFC 8017 §8.2.2, for RSA cert-chain
  signatures) and RSA-PSS (§9.1.2, for TLS 1.3's RSA CertificateVerify), on a
  Montgomery bignum widened to 4096-bit (`mont_pow`). X.509 parses RSA keys +
  RSA sig algs; the verify dispatch and trust anchors handle EC *and* RSA. Second
  anchor bundled: **ISRG Root X1** (RSA-4096, Let's Encrypt). **Green on the
  metal:** `test tls rsa` authenticated valid-isrgrootx1.letsencrypt.org --
  RSA-PSS CertificateVerify + an all-RSA chain to ISRG Root X1 -- and read
  `HTTP/1.1 200 OK`. Host: `test_{rsa,pss,rsachain}.c` (OpenSSL vectors + a frozen
  real LE chain), 15 suites green.
    - **Still open:** more bundled roots; RSA-PSS-*signed* certs (id-RSASSA-PSS,
      rare for CAs); a live wrong-host/expired *negative* boot test.
- **T4 — the HTTPS client. ✅ DONE.** `wget` speaks `https://` over `libtls`:
  parse http/https, TCP connect, authenticated `tls_connect`, then HTTP over a
  raw-socket-or-TLS transport shim. Refuses on cert failure (no insecure
  fallback). **Green on the metal:** `test wget https` fetched
  https://valid-isrgrootx1.letsencrypt.org/ -> `HTTP 200`, 4067 decrypted bytes
  written to `/wgets.out` on EMBKFS. The OS reads a real HTTPS URL end to end.
- **T5 — the consumers.** Wire Python's `ssl`/`_socket` to `libtls` (→ `pip
  install` from PyPI); git-over-HTTPS; and the package registry's network fetch
  (packaging PK4). This is where TLS pays for itself.

## 4. What we deliberately do NOT do

- **No TLS 1.2 / no legacy suites.** 1.3-only; no RSA key exchange, CBC, RC4,
  compression, renegotiation, or session tickets (v1).
- **No OpenSSL.** Our own record layer, handshake, and cert verification, on our
  own crypto — the same "own the whole stack" position as the compiler and libc.
- **No in-kernel TLS.** Userspace library; the kernel stays out of it.
- **No blind trust past T2.** The unverified-cert phase is a *labelled milestone*,
  never a shipped default — T3 lands before any consumer (pip/git) rides on it.

## 5. Open sub-decisions

1. **Root CA store shape** — bundle the Mozilla set (large) vs a curated few (pypi/
   github issuers) vs an on-image `/system/etc/ca` the user grows. *Lean: a small
   curated bundle under `/system`, verified-boot-sealed.*
2. **Blocking vs non-blocking API** — a blocking `tls_read/write` (simplest, fits
   the current socket shim) vs an event/poll surface. *Lean: blocking v1.*
3. **Where the crypto shims live** — `user/lib/tls/crypto/shim/` re-exporting
   kernel `types.h`/`kstring.h`/`kprintf.h` to stdint/string/stdio. *Lean: yes,
   there.*
