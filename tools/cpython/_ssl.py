"""_ssl -- minimal stub so TLS-capability probes pass on EmbLinkOS.

Normally CPython's C OpenSSL binding. Here, real TLS is provided by the
`_embtls` builtin behind our pure-Python `ssl.py`; there is no OpenSSL. Some
callers (e.g. pip's `has_tls()` -> `import _ssl`) use the mere importability of
`_ssl` as "this build can do TLS" -- which is TRUE here -- and read a couple of
version identifiers from it. This provides exactly that and nothing more.

It is NOT a drop-in for OpenSSL's _ssl: anything that expects the real
SSLContext/SSLSocket C types should import `ssl` (our shim), not this.
"""

OPENSSL_VERSION = "EmbLinkOS libtls (TLS 1.3)"
OPENSSL_VERSION_INFO = (3, 0, 0, 0, 0)
OPENSSL_VERSION_NUMBER = 0x30000000
