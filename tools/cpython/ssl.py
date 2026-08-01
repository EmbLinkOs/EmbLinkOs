"""ssl -- a minimal TLS client for CPython on EmbLinkOS, over the `_embtls`
builtin (our own libtls, docs/TLS.md) instead of OpenSSL.

This REPLACES the stdlib ssl.py (which imports _ssl / OpenSSL, absent here) in
the packed stdlib zip -- see tools/mkpystdlib.py OVERRIDES. It implements only
the surface http.client.HTTPSConnection, urllib.request, and pip actually use:
a context whose wrap_socket() runs the handshake over a connected socket and
returns a file-like TLS socket.

Honesty note: libtls ALWAYS authenticates the peer (certificate + hostname, vs
the embedded trust anchors) and refuses on failure -- there is no "unverified"
mode to expose. `_create_unverified_context` therefore returns a normal
verifying context: it errs toward MORE security, never less. A caller that
truly needs to talk to an untrusted/self-signed host will get an SSLError, which
is the honest outcome for an OS whose TLS only trusts real anchors.
"""
import _embtls
import io

# --- module constants the stdlib callers reference ---------------------------
CERT_NONE = 0
CERT_OPTIONAL = 1
CERT_REQUIRED = 2

PROTOCOL_TLS = 2
PROTOCOL_TLS_CLIENT = 2
PROTOCOL_TLS_SERVER = 3

# TLSVersion-ish sentinels (attributes only; libtls is 1.3-only regardless).
class TLSVersion:
    MINIMUM_SUPPORTED = -2
    TLSv1_2 = 771
    TLSv1_3 = 772
    MAXIMUM_SUPPORTED = -1

OP_ALL = 0
OP_NO_SSLv2 = 0
OP_NO_SSLv3 = 0
HAS_SNI = True
HAS_TLSv1_3 = True


class Purpose:
    SERVER_AUTH = "SERVER_AUTH"
    CLIENT_AUTH = "CLIENT_AUTH"


class SSLError(OSError):
    pass


class SSLCertVerificationError(SSLError):
    pass


class SSLEOFError(SSLError):
    pass


class _SSLRawIO(io.RawIOBase):
    """Adapts an SSLSocket to a raw byte stream so socket.makefile-style
    buffered/text wrappers work over the encrypted channel."""

    def __init__(self, sslsock):
        self._s = sslsock

    def readable(self):
        return True

    def writable(self):
        return True

    def readinto(self, b):
        data = self._s.recv(len(b))
        n = len(data)
        b[:n] = data
        return n            # 0 => EOF (clean close_notify)

    def write(self, b):
        return self._s.send(bytes(b))


class SSLSocket:
    """A TLS-wrapped socket. Owns the underlying fd (handed to _embtls at
    wrap_socket time); the plaintext socket has been detached and is dead."""

    def __init__(self, conn, server_hostname):
        self._conn = conn                 # opaque _embtls capsule
        self.server_hostname = server_hostname
        self._closed = False

    # -- byte I/O -------------------------------------------------------------
    def send(self, data):
        return _embtls.write(self._conn, data)

    def sendall(self, data):
        mv = memoryview(data).cast("B")
        total = 0
        while total < len(mv):
            n = _embtls.write(self._conn, mv[total:])
            if n <= 0:
                raise SSLError("TLS write failed")
            total += n
        return None

    def recv(self, bufsize, flags=0):
        return _embtls.read(self._conn, bufsize)

    def recv_into(self, buffer, nbytes=0, flags=0):
        want = nbytes if nbytes else len(buffer)
        data = _embtls.read(self._conn, want)
        n = len(data)
        memoryview(buffer)[:n] = data
        return n

    def read(self, len=1024, buffer=None):
        if buffer is not None:
            return self.recv_into(buffer, len)
        return self.recv(len)

    def write(self, data):
        return self.send(data)

    # -- file wrapper (http.client does sock.makefile("rb")) ------------------
    def makefile(self, mode="r", buffering=None, *,
                 encoding=None, errors=None, newline=None):
        raw = _SSLRawIO(self)
        writing = "w" in mode
        binary = "b" in mode
        buf = io.BufferedWriter(raw) if writing else io.BufferedReader(raw)
        if binary:
            return buf
        return io.TextIOWrapper(buf, encoding, errors, newline)

    # -- lifecycle / socket-ish no-ops ----------------------------------------
    def close(self):
        if not self._closed:
            self._closed = True
            _embtls.close(self._conn)

    def detach(self):
        self._closed = True
        return -1

    def settimeout(self, value):
        pass                              # blocking-only; see cpython-port memory

    def gettimeout(self):
        return None

    def setsockopt(self, *args):
        pass

    def setblocking(self, flag):
        pass

    def fileno(self):
        return -1                         # the fd now lives inside _embtls

    def getpeercert(self, binary_form=False):
        # libtls verified the peer but does not surface the parsed cert yet.
        return None

    def version(self):
        return "TLSv1.3"

    def cipher(self):
        return ("TLS_AES_128_GCM_SHA256", "TLSv1.3", 128)

    def selected_alpn_protocol(self):
        return None

    def do_handshake(self):
        pass                              # already done in wrap_socket

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


class SSLContext:
    def __init__(self, protocol=PROTOCOL_TLS_CLIENT):
        self.protocol = protocol
        self.check_hostname = True        # libtls always checks; reflect that
        self.verify_mode = CERT_REQUIRED  # and always requires a trusted cert
        self.post_handshake_auth = None   # http.client checks `is not None`
        self.options = 0
        self.minimum_version = TLSVersion.TLSv1_3
        self.maximum_version = TLSVersion.TLSv1_3
        self._alpn = []

    # config no-ops: libtls's policy is fixed (TLS 1.3, its own anchors).
    def load_default_certs(self, *a, **k):
        pass

    def load_verify_locations(self, *a, **k):
        pass

    def set_ciphers(self, *a):
        pass

    def set_alpn_protocols(self, protocols):
        self._alpn = list(protocols)

    def wrap_socket(self, sock, server_hostname=None, do_handshake_on_connect=True,
                    suppress_ragged_eofs=True, session=None):
        host = server_hostname or ""
        fd = sock.detach()                # transfer fd ownership to _embtls
        try:
            conn = _embtls.connect(fd, host)
        except OSError as e:
            raise SSLError(str(e)) from e
        return SSLSocket(conn, server_hostname)

    def wrap_bio(self, *a, **k):
        raise NotImplementedError("memory BIO not supported on EmbLinkOS")


def create_default_context(purpose=Purpose.SERVER_AUTH, *,
                           cafile=None, capath=None, cadata=None):
    return SSLContext(PROTOCOL_TLS_CLIENT)


def _create_unverified_context(*a, **k):
    # No unverified mode exists (see the module docstring); return a real one.
    return SSLContext(PROTOCOL_TLS_CLIENT)


def _create_stdlib_context(*a, **k):
    return SSLContext(PROTOCOL_TLS_CLIENT)


_create_default_https_context = create_default_context
