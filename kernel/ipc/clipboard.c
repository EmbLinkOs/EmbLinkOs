/* kernel/ipc/clipboard.c -- see clipboard.h. */
#include "ipc/clipboard.h"
#include "include/usercopy.h"
#include "include/errno.h"
#include "include/kstring.h"
#include "include/spinlock.h"
#include "process/process.h"      /* current_process->session_id: whose clipboard it is */

/* Static storage, not kmalloc: 64KB of bss buys a clipboard that can never
 * fail to exist, and the lock is held only across bounded memcpys (the user
 * copies are access_ok-checked memcpys, never faulting or blocking). */
static uint8_t    g_clip[CLIPBOARD_MAX];
static size_t     g_clip_len;
static spinlock_t g_clip_lock;
static uint32_t   g_clip_session;      /* whose content this is */

static uint32_t caller_session(void) {
    return current_thread ? current_process->session_id : 0;
}

int64_t clipboard_set_user(const void *ubuf, size_t len) {
    if (len > CLIPBOARD_MAX) return -EMBK_EINVAL;
    spin_lock(&g_clip_lock);
    if (len && copy_from_user(g_clip, ubuf, len) != EMBK_OK) {
        spin_unlock(&g_clip_lock);
        return -EMBK_EFAULT;
    }
    /* A shorter copy must not leave the tail of a longer, older one behind
     * for the next owner's session to find in kernel memory. */
    if (len < g_clip_len)
        memset(g_clip + len, 0, g_clip_len - len);
    g_clip_len = len;
    g_clip_session = caller_session();
    spin_unlock(&g_clip_lock);
    return 0;
}

int64_t clipboard_get_user(void *ubuf, size_t cap) {
    spin_lock(&g_clip_lock);
    if (g_clip_session != caller_session()) {   /* someone else's: empty, as far as you know */
        spin_unlock(&g_clip_lock);
        return 0;
    }
    size_t n = g_clip_len < cap ? g_clip_len : cap;
    if (n && copy_to_user(ubuf, g_clip, n) != EMBK_OK) {
        spin_unlock(&g_clip_lock);
        return -EMBK_EFAULT;
    }
    int64_t held = (int64_t)g_clip_len;
    spin_unlock(&g_clip_lock);
    return held;
}

uint32_t clipboard_owner_session(void) {
    spin_lock(&g_clip_lock);
    uint32_t s = g_clip_len ? g_clip_session : 0;
    spin_unlock(&g_clip_lock);
    return s;
}

void clipboard_session_ended(uint32_t sid) {
    spin_lock(&g_clip_lock);
    if (g_clip_session == sid) {
        memset(g_clip, 0, g_clip_len);
        g_clip_len = 0;
        g_clip_session = 0;
    }
    spin_unlock(&g_clip_lock);
}
