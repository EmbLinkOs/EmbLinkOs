/* _embtls -- a minimal TLS 1.3 client for CPython on EmbLinkOS, backed by our
 * own libtls (user/lib/tls, docs/TLS.md) instead of OpenSSL. CPython's real
 * _ssl wants the whole OpenSSL API; we don't have it and won't. Instead this
 * tiny module exposes just what a blocking https:// fetch needs -- connect over
 * an existing socket fd, then read/write plaintext -- and a pure-Python ssl.py
 * shim (Lib/ssl.py in the stdlib zip) dresses it up as SSLContext/wrap_socket
 * for http.client and urllib.
 *
 * It includes ONLY tls_handle.h (an opaque-handle wrapper), never tls.h: the
 * full struct tls_conn drags in the kernel/kshim include world, which cannot
 * coexist with Python.h in one TU. tls_handle.c bridges the two, compiled with
 * the TLS include set. The libtls objects are linked in via Modules/Setup.local.
 *
 * OWNERSHIP: connect() takes ownership of `fd` -- libtls closes it on close().
 * ssl.py detaches the fd from the Python socket before handing it over, so the
 * descriptor has exactly one owner. */
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "tls_handle.h"

#define CAPSULE_NAME "_embtls.conn"

/* The capsule holds a `box`, not the raw tls_conn*, so close() can null the
 * inner pointer WITHOUT PyCapsule_SetPointer (which rejects NULL) -- that keeps
 * close() double-free-safe and lets the destructor be the single fallback
 * freer. box itself is freed only by the destructor. */
struct box { struct tls_conn *c; };

static void conn_destructor(PyObject *cap)
{
    struct box *b = (struct box *)PyCapsule_GetPointer(cap, CAPSULE_NAME);
    if (b) {
        if (b->c)
            tls_handle_free(b->c);   /* close() didn't run */
        PyMem_Free(b);
    }
}

/* connect(fd, server_name) -> capsule.  Runs the TLS 1.3 handshake (incl.
 * certificate + hostname verification) over the connected socket `fd`. */
static PyObject *embtls_connect(PyObject *self, PyObject *args)
{
    int fd;
    const char *host;
    if (!PyArg_ParseTuple(args, "is:connect", &fd, &host))
        return NULL;

    struct box *b = (struct box *)PyMem_Malloc(sizeof(*b));
    if (!b)
        return PyErr_NoMemory();
    b->c = tls_handle_new();
    if (!b->c) {
        PyMem_Free(b);
        return PyErr_NoMemory();
    }

    int rc;
    Py_BEGIN_ALLOW_THREADS
    rc = tls_handle_connect(b->c, fd, host);
    Py_END_ALLOW_THREADS

    if (rc != 0) {
        tls_handle_free(b->c);   /* also closes fd */
        PyMem_Free(b);
        PyErr_Format(PyExc_OSError,
                     "TLS handshake failed (rc=%d) for %s", rc, host);
        return NULL;
    }

    PyObject *cap = PyCapsule_New(b, CAPSULE_NAME, conn_destructor);
    if (!cap) {
        tls_handle_free(b->c);
        PyMem_Free(b);
        return NULL;
    }
    return cap;
}

static struct tls_conn *conn_of(PyObject *cap)
{
    struct box *b = (struct box *)PyCapsule_GetPointer(cap, CAPSULE_NAME);
    if (!b || !b->c) {
        PyErr_SetString(PyExc_ValueError, "TLS connection is closed");
        return NULL;
    }
    return b->c;
}

/* write(cap, bytes) -> int (bytes accepted). */
static PyObject *embtls_write(PyObject *self, PyObject *args)
{
    PyObject *cap;
    Py_buffer buf;
    if (!PyArg_ParseTuple(args, "Oy*:write", &cap, &buf))
        return NULL;
    struct tls_conn *c = conn_of(cap);
    if (!c) { PyBuffer_Release(&buf); return NULL; }

    long n;
    Py_BEGIN_ALLOW_THREADS
    n = tls_handle_write(c, buf.buf, (size_t)buf.len);
    Py_END_ALLOW_THREADS
    PyBuffer_Release(&buf);

    if (n < 0)
        return PyErr_SetString(PyExc_OSError, "TLS write failed"), NULL;
    return PyLong_FromLong(n);
}

/* read(cap, size) -> bytes.  b'' on clean close (close_notify/EOF). */
static PyObject *embtls_read(PyObject *self, PyObject *args)
{
    PyObject *cap;
    Py_ssize_t size;
    if (!PyArg_ParseTuple(args, "On:read", &cap, &size))
        return NULL;
    struct tls_conn *c = conn_of(cap);
    if (!c)
        return NULL;
    if (size < 0)
        return PyErr_SetString(PyExc_ValueError, "negative read size"), NULL;

    PyObject *out = PyBytes_FromStringAndSize(NULL, size);
    if (!out)
        return NULL;

    long n;
    char *dst = PyBytes_AS_STRING(out);
    Py_BEGIN_ALLOW_THREADS
    n = tls_handle_read(c, dst, (size_t)size);
    Py_END_ALLOW_THREADS

    if (n < 0) {
        Py_DECREF(out);
        return PyErr_SetString(PyExc_OSError, "TLS read failed"), NULL;
    }
    if (n != size)
        _PyBytes_Resize(&out, n);   /* may set out=NULL on failure */
    return out;
}

/* close(cap) -> None.  Idempotent; invalidates the capsule so read/write after
 * raise ValueError rather than touching freed memory. */
static PyObject *embtls_close(PyObject *self, PyObject *args)
{
    PyObject *cap;
    if (!PyArg_ParseTuple(args, "O:close", &cap))
        return NULL;
    struct box *b = (struct box *)PyCapsule_GetPointer(cap, CAPSULE_NAME);
    if (b && b->c) {
        struct tls_conn *c = b->c;
        b->c = NULL;                       /* the destructor now no-ops on it */
        Py_BEGIN_ALLOW_THREADS
        tls_handle_free(c);
        Py_END_ALLOW_THREADS
    } else {
        PyErr_Clear();   /* already closed / bad capsule -- close is idempotent */
    }
    Py_RETURN_NONE;
}

static PyMethodDef embtls_methods[] = {
    {"connect", embtls_connect, METH_VARARGS,
     "connect(fd, server_name) -> conn: run the TLS 1.3 handshake over a "
     "connected socket fd (ownership of fd transfers to the connection)."},
    {"write", embtls_write, METH_VARARGS, "write(conn, bytes) -> int"},
    {"read",  embtls_read,  METH_VARARGS, "read(conn, size) -> bytes"},
    {"close", embtls_close, METH_VARARGS, "close(conn) -> None"},
    {NULL, NULL, 0, NULL},
};

static struct PyModuleDef embtlsmodule = {
    PyModuleDef_HEAD_INIT, "_embtls",
    "Minimal TLS 1.3 client backed by EmbLinkOS libtls.",
    -1, embtls_methods, NULL, NULL, NULL, NULL,
};

PyMODINIT_FUNC PyInit__embtls(void)
{
    return PyModule_Create(&embtlsmodule);
}
