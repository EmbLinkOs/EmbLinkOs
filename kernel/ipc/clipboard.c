/* kernel/ipc/clipboard.c -- see clipboard.h. */
#include "ipc/clipboard.h"
#include "arch/x86_64/syscall/usercopy.h"
#include "include/errno.h"
#include "include/kstring.h"
#include "arch/x86_64/cpu/spinlock.h"

/* Static storage, not kmalloc: 64KB of bss buys a clipboard that can never
 * fail to exist, and the lock is held only across bounded memcpys (the user
 * copies are access_ok-checked memcpys, never faulting or blocking). */
static uint8_t    g_clip[CLIPBOARD_MAX];
static size_t     g_clip_len;
static spinlock_t g_clip_lock;

int64_t clipboard_set_user(const void *ubuf, size_t len) {
    if (len > CLIPBOARD_MAX) return -EMBK_EINVAL;
    spin_lock(&g_clip_lock);
    if (len && copy_from_user(g_clip, ubuf, len) != EMBK_OK) {
        spin_unlock(&g_clip_lock);
        return -EMBK_EFAULT;
    }
    g_clip_len = len;
    spin_unlock(&g_clip_lock);
    return 0;
}

int64_t clipboard_get_user(void *ubuf, size_t cap) {
    spin_lock(&g_clip_lock);
    size_t n = g_clip_len < cap ? g_clip_len : cap;
    if (n && copy_to_user(ubuf, g_clip, n) != EMBK_OK) {
        spin_unlock(&g_clip_lock);
        return -EMBK_EFAULT;
    }
    int64_t held = (int64_t)g_clip_len;
    spin_unlock(&g_clip_lock);
    return held;
}
