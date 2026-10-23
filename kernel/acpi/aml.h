/* kernel/acpi/aml.h -- ACPI Machine Language: the namespace, and running it.
 *
 * WHY A WHOLE INTERPRETER IS UNAVOIDABLE. Almost everything a laptop needs to
 * be usable is not in any ACPI table as data. The battery's charge, whether
 * the lid is shut, the CPU's temperature, which interrupt line a PCI device
 * actually lands on -- all of it is behind CODE in the DSDT, compiled to a
 * bytecode the OS is expected to execute. The firmware ships a program and the
 * operating system is its runtime. There is no way to read around it: a table
 * parser gets you the addresses of things and none of their behaviour.
 *
 * acpi.c already found the one exception and says so: `\_S5_` is usually a
 * named package of constants, so powering off could be done by RECOGNISING the
 * encoding of data without executing anything. That trick does not extend --
 * `_BST` is a method that talks to the embedded controller, and no amount of
 * pattern matching evaluates it.
 *
 * WHAT THIS IS AND IS NOT. It is a real interpreter: a namespace built from
 * every DSDT and SSDT on the machine, the object model (integers, strings,
 * buffers, packages, references, field units), control methods with their
 * arguments and locals, and the operation regions that let AML touch memory,
 * I/O ports, PCI configuration space and the embedded controller. It is not
 * ACPICA: there is no AML *compiler*, no table loading at runtime (`Load`), no
 * garbage-collected object store, and the concurrency model is a global lock
 * rather than per-method serialisation. Those are written up in docs/TODO.md
 * with what each would cost.
 */
#ifndef _EMBK_AML_H_
#define _EMBK_AML_H_

#include <stdint.h>
#include "include/types.h"

/* ---- the object model ---------------------------------------------------
 *
 * AML has no static types: a method argument is whatever the caller passed,
 * and `Store` converts between kinds by rules that depend on the DESTINATION.
 * So an object carries its kind with it. */
enum aml_kind {
    AML_UNINIT = 0,       /* an uninitialised Local or Arg                   */
    AML_INTEGER,
    AML_STRING,
    AML_BUFFER,
    AML_PACKAGE,
    AML_DEVICE,           /* a namespace node with no value of its own       */
    AML_METHOD,
    AML_REGION,           /* OperationRegion: an address space and a window  */
    AML_FIELD,            /* a named run of bits inside a region             */
    AML_BUFFER_FIELD,     /* a named run of bits inside a buffer             */
    AML_MUTEX,
    AML_EVENT,
    AML_POWER_RES,
    AML_PROCESSOR,
    AML_THERMAL_ZONE,
    AML_REF,              /* a reference to a node or an object              */
    /* A NAME THAT DID NOT EXIST YET. ACPI resolves names when they are USED,
     * not where they are written, and firmware relies on it: a _PRT declared
     * near the top of a table routinely names link devices declared near the
     * bottom. Keeping the text AND the scope it appeared in is what lets the
     * same upward search run later and get the right answer. */
    AML_UNRESOLVED,
    AML_DEBUG,            /* the Debug object: writes go to the log          */
};

struct aml_node;

struct aml_object {
    enum aml_kind kind;
    union {
        uint64_t integer;
        struct {                      /* STRING and BUFFER share this shape */
            uint8_t *data;
            uint32_t len;             /* bytes; a string's NUL is not counted */
        } buf;
        struct {
            struct aml_object **elem;
            uint32_t count;
        } pkg;
        struct {
            uint8_t  space;           /* AML_REGION_*                        */
            uint64_t offset;
            uint64_t length;
            uint64_t mapped;          /* lazy virtual mapping, 0 until used  */
            struct aml_node *owner;   /* for PCI_Config: whose _ADR applies  */
        } region;
        struct {
            struct aml_node *region;  /* which region the bits live in       */
            uint32_t bit_offset;
            uint32_t bit_width;
            uint8_t  access;          /* AML_ACCESS_*                        */
            uint8_t  lock;
            uint8_t  update;          /* AML_UPDATE_*                        */
        } field;
        struct {
            struct aml_object *buffer;
            uint32_t bit_offset;
            uint32_t bit_width;
        } bfield;
        struct {
            const uint8_t *body;      /* into the table; never freed         */
            uint32_t       len;
            uint8_t        argc;
            uint8_t        serialized;
        } method;
        struct {
            struct aml_node   *node;  /* a name reference                    */
            struct aml_object *obj;   /* or a direct object reference        */
            int                index; /* >= 0: element of a package/buffer   */
        } ref;
        struct { uint8_t sync_level; int depth; } mutex;
        struct { struct aml_node *scope; char *path; } uname;
    } u;
    /* Objects are reference-counted rather than copied: a Package holds
     * pointers to its elements and a method may return one of them. */
    int refs;
};

/* Address spaces an OperationRegion can name. Anything else is refused at
 * ACCESS time rather than at load time -- a machine may declare a region this
 * kernel has no handler for and never touch it, and refusing to load its DSDT
 * over that would lose everything else in it. */
#define AML_REGION_SYSTEM_MEMORY  0x00
#define AML_REGION_SYSTEM_IO      0x01
#define AML_REGION_PCI_CONFIG     0x02
#define AML_REGION_EMBEDDED_CTRL  0x03
#define AML_REGION_SMBUS          0x04

#define AML_ACCESS_ANY   0
#define AML_ACCESS_BYTE  1
#define AML_ACCESS_WORD  2
#define AML_ACCESS_DWORD 3
#define AML_ACCESS_QWORD 4
#define AML_ACCESS_BUFFER 5

#define AML_UPDATE_PRESERVE      0
#define AML_UPDATE_WRITE_AS_ONES 1
#define AML_UPDATE_WRITE_AS_ZEROS 2

/* A namespace node. The namespace is a tree of four-character names, and a
 * path is those names joined: \_SB.PCI0.LPC_.EC__ . Four characters exactly,
 * padded with '_', which is why names in a DSDT look the way they do. */
#define AML_NAME_LEN 4
struct aml_node {
    char              name[AML_NAME_LEN + 1];
    struct aml_node  *parent;
    struct aml_node  *child;      /* first child            */
    struct aml_node  *sibling;    /* next sibling           */
    struct aml_object *value;     /* NULL for a bare scope  */
};

/* ---- building and using the namespace ---------------------------------- */

/* Load every DSDT and SSDT the machine has and build the namespace. Safe to
 * call once; later calls do nothing. False means no DSDT, or one that did not
 * parse -- the caller keeps whatever non-AML fallbacks it had. */
bool aml_init(void);

/* Did aml_init() build a usable namespace? */
bool aml_available(void);

/* Find a node by path. An absolute path starts with '\'; a relative one is
 * searched from `from` upwards, which is ACPI's own scoping rule. NULL if
 * there is no such name. `from` may be NULL, meaning the root. */
struct aml_node *aml_lookup(struct aml_node *from, const char *path);

struct aml_node *aml_root(void);

/* The node an object names: follows a reference, and resolves a name that did
 * not exist when it was parsed. NULL if it still does not exist. */
struct aml_node *aml_name_node(struct aml_object *o);

/* Evaluate a node: run it if it is a method, otherwise return its value.
 * `args`/`argc` are the method's arguments (up to 7). The result is a NEW
 * reference the caller must release with aml_put(). NULL means the name does
 * not exist, is not evaluable, or the method failed. */
struct aml_object *aml_evaluate(struct aml_node *node,
                                struct aml_object **args, int argc);

/* The common case: evaluate `path` relative to `from` and, if the result is an
 * integer, hand it back. False if the name is absent or not an integer -- and
 * ABSENT IS NORMAL: most _xxx methods are optional and a machine that does not
 * implement one is not broken. */
bool aml_eval_integer(struct aml_node *from, const char *path, uint64_t *out);

/* Walk every node under `from` (depth first, `from` included), calling `fn`
 * until it returns false. */
void aml_walk(struct aml_node *from,
              bool (*fn)(struct aml_node *n, void *ctx), void *ctx);

/* Find every device whose _HID or _CID matches `hid` (e.g. "PNP0C0A" for a
 * battery), calling `fn` for each. */
void aml_for_each_hid(const char *hid,
                      bool (*fn)(struct aml_node *n, void *ctx), void *ctx);

/* ---- objects ------------------------------------------------------------ */
struct aml_object *aml_integer(uint64_t v);
struct aml_object *aml_string(const char *s);
struct aml_object *aml_get(struct aml_object *o);   /* take a reference  */
void               aml_put(struct aml_object *o);   /* drop one          */

/* Print the whole namespace to the kernel log. `test aml` uses it; it is the
 * only way to see what a machine's firmware actually declared. */
void aml_dump(void);

/* How much got built, for the one line acpi_init prints and for the test. */
struct aml_stats {
    uint32_t tables;      /* DSDT + SSDTs loaded          */
    uint32_t nodes;       /* namespace nodes created      */
    uint32_t methods;
    uint32_t devices;
    uint32_t regions;
    uint32_t fields;
    uint32_t load_errors; /* opcodes the load pass refused */
};
const struct aml_stats *aml_get_stats(void);

#endif
