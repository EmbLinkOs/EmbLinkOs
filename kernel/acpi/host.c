/* uACPI 7 host services. Initialized on the BSP after the scheduler/timebase.
 * SCI owns IRQ slot 9 exclusively; shared SCI and PCI segments are unsupported.
 */
#include "acpi/acpi.h"
#include "include/kmalloc.h"
#include "include/kprintf.h"
#include "include/io.h"
#include "drivers/bus/pci.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "process/ksync.h"
#include "drivers/timer/timer.h"
#include "arch/x86_64/irq/irq.h"
#include "arch/x86_64/irq/ioapic.h"
#include "arch/x86_64/irq/lapic.h"
#include <uacpi/kernel_api.h>

uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr *out) {
    *out = acpi_rsdp_address();
    return *out ? UACPI_STATUS_OK : UACPI_STATUS_NOT_FOUND;
}
void *uacpi_kernel_map(uacpi_phys_addr p, uacpi_size n) {
    if (!n || p + n < p || p + n >= (1ULL << 46)) return UACPI_MAP_FAILED;
    if (vmm_get_phys(P2V(p)) == p && vmm_get_phys(P2V(p + n - 1)) == p + n - 1)
        return (void *)P2V(p);
    void *v = (void *)vmm_map_mmio(p, n);
    return v ? v : UACPI_MAP_FAILED;
}
void uacpi_kernel_unmap(void *v, uacpi_size n) {
    if ((uint64_t)v >= 0xffffc00000000000ULL) vmm_unmap_mmio((uint64_t)v, n);
}
void uacpi_kernel_log(uacpi_log_level level, const char *s) {
    (void)level; kprintf("[acpi] %s", s);
}
void *uacpi_kernel_alloc(uacpi_size n) { return kmalloc(n); }
void uacpi_kernel_free(void *p) { kfree(p); }
uacpi_u64 uacpi_kernel_get_nanoseconds_since_boot(void) { return time_get_ns(); }
void uacpi_kernel_stall(uacpi_u8 us) {
    uint64_t start = time_get_us();
    while (time_get_us() - start < us) __asm__ volatile("pause");
}
void uacpi_kernel_sleep(uacpi_u64 ms) {
    uint64_t start = time_get_ns();
    if (ms > UINT64_MAX / 1000000) ms = UINT64_MAX / 1000000;
    while (time_get_ns() - start < ms * 1000000) schedule();
}
uacpi_thread_id uacpi_kernel_get_thread_id(void) {
    return (uacpi_thread_id)(uintptr_t)current_thread;
}
uacpi_interrupt_state uacpi_kernel_disable_interrupts(void) {
    uint64_t flags; __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}
void uacpi_kernel_restore_interrupts(uacpi_interrupt_state flags) {
    if (flags & (1 << 9)) __asm__ volatile("sti" ::: "memory");
}
uacpi_handle uacpi_kernel_create_spinlock(void) { return kcalloc(1, sizeof(uint32_t)); }
void uacpi_kernel_free_spinlock(uacpi_handle p) { kfree(p); }
uacpi_cpu_flags uacpi_kernel_lock_spinlock(uacpi_handle p) {
    uacpi_cpu_flags flags = uacpi_kernel_disable_interrupts();
    while (__atomic_exchange_n((uint32_t *)p, 1, __ATOMIC_ACQUIRE)) __asm__ volatile("pause");
    return flags;
}
void uacpi_kernel_unlock_spinlock(uacpi_handle p, uacpi_cpu_flags flags) {
    __atomic_store_n((uint32_t *)p, 0, __ATOMIC_RELEASE);
    uacpi_kernel_restore_interrupts(flags);
}
uacpi_handle uacpi_kernel_create_semaphore(uacpi_u32 n) {
    if (n > INT32_MAX) return NULL;
    struct semaphore *s = kmalloc(sizeof *s);
    if (s) sem_init(s, n);
    return s;
}
void uacpi_kernel_free_semaphore(uacpi_handle s) { kfree(s); }
uacpi_status uacpi_kernel_wait_for_semaphore(uacpi_handle s, uacpi_u16 ms) {
    if (ms == 0xffff) { sem_wait(s); return UACPI_STATUS_OK; }
    uint64_t start = time_get_ns();
    do {
        if (sem_trywait(s)) return UACPI_STATUS_OK;
        if (!ms || time_get_ns() - start >= (uint64_t)ms * 1000000) break;
        schedule();
    } while (1);
    return UACPI_STATUS_TIMEOUT;
}
void uacpi_kernel_signal_semaphore(uacpi_handle s) { sem_post(s); }
uacpi_status uacpi_kernel_handle_firmware_request(uacpi_firmware_request *r) {
    (void)r; kprintf("[acpi] firmware Breakpoint/Fatal request\n");
    return UACPI_STATUS_UNIMPLEMENTED;
}

struct io_range { uint32_t base, len; };
uacpi_status uacpi_kernel_io_map(uacpi_io_addr base, uacpi_size len, uacpi_handle *out) {
    if (!len || base >= 65536 || len > 65536 - base) return UACPI_STATUS_INVALID_ARGUMENT;
    struct io_range *r = kmalloc(sizeof *r);
    if (!r) return UACPI_STATUS_OUT_OF_MEMORY;
    r->base = base; r->len = len; *out = r; return UACPI_STATUS_OK;
}
void uacpi_kernel_io_unmap(uacpi_handle r) { kfree(r); }
#define IO_ACCESS(bits, ctype, insn, outsn) \
uacpi_status uacpi_kernel_io_read##bits(uacpi_handle h, uacpi_size off, ctype *v) { \
    struct io_range *r = h; \
    if (off > r->len || sizeof(*v) > r->len - off) return UACPI_STATUS_INVALID_ARGUMENT; \
    *v = insn(r->base + off); return UACPI_STATUS_OK; \
} \
uacpi_status uacpi_kernel_io_write##bits(uacpi_handle h, uacpi_size off, ctype v) { \
    struct io_range *r = h; \
    if (off > r->len || sizeof(v) > r->len - off) return UACPI_STATUS_INVALID_ARGUMENT; \
    outsn(r->base + off, v); return UACPI_STATUS_OK; \
}
IO_ACCESS(8, uacpi_u8, inb, outb)
IO_ACCESS(16, uacpi_u16, inw, outw)
IO_ACCESS(32, uacpi_u32, inl, outl)
/* Configuration mechanism #1, using the shared PCI address/data lock. */
uacpi_status uacpi_kernel_pci_device_open(uacpi_pci_address a, uacpi_handle *out) {
    if (a.segment || a.device > 31 || a.function > 7) return UACPI_STATUS_UNIMPLEMENTED;
    *out = (void *)(uintptr_t)(0x80000000U | ((uint32_t)a.bus << 16)
                | ((uint32_t)a.device << 11) | ((uint32_t)a.function << 8));
    return UACPI_STATUS_OK;
}
void uacpi_kernel_pci_device_close(uacpi_handle h) { (void)h; }
#define PCI_ACCESS(bits, ctype) \
uacpi_status uacpi_kernel_pci_read##bits(uacpi_handle h, uacpi_size off, ctype *v) { \
    if (off > 256 - sizeof(*v) || (off & (sizeof(*v)-1))) return UACPI_STATUS_INVALID_ARGUMENT; \
    uint32_t a = (uintptr_t)h; \
    *v = pci_read##bits(a >> 16, (a >> 11) & 31, (a >> 8) & 7, off); \
    return UACPI_STATUS_OK; \
} \
uacpi_status uacpi_kernel_pci_write##bits(uacpi_handle h, uacpi_size off, ctype v) { \
    if (off > 256 - sizeof(v) || (off & (sizeof(v)-1))) return UACPI_STATUS_INVALID_ARGUMENT; \
    uint32_t a = (uintptr_t)h; \
    pci_write##bits(a >> 16, (a >> 11) & 31, (a >> 8) & 7, off, v); \
    return UACPI_STATUS_OK; \
}
PCI_ACCESS(8, uacpi_u8)
PCI_ACCESS(16, uacpi_u16)
PCI_ACCESS(32, uacpi_u32)

static uacpi_interrupt_handler sci_fn;
static void *sci_ctx;
static uint32_t sci_gsi, irq_active;
static void sci_dispatch(void) {
    __atomic_add_fetch(&irq_active, 1, __ATOMIC_ACQ_REL);
    if (sci_fn) sci_fn(sci_ctx);
    __atomic_sub_fetch(&irq_active, 1, __ATOMIC_RELEASE);
}
uacpi_status uacpi_kernel_install_interrupt_handler(uacpi_u32 irq,
    uacpi_interrupt_handler fn, uacpi_handle ctx, uacpi_handle *out) {
    struct irq_line_info info[16]; irq_snapshot(info);
    if (irq != 9 || info[9].handler_addr || sci_fn) return UACPI_STATUS_UNIMPLEMENTED;
    sci_ctx = ctx; sci_fn = fn; sci_gsi = irq;
    irq_register(9, sci_dispatch);
    ioapic_route_level(irq, 41, lapic_get_id(), true);
    *out = &sci_fn; return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_uninstall_interrupt_handler(uacpi_interrupt_handler fn, uacpi_handle h) {
    if (h != &sci_fn || fn != sci_fn) return UACPI_STATUS_INVALID_ARGUMENT;
    ioapic_mask(sci_gsi);
    while (__atomic_load_n(&irq_active, __ATOMIC_ACQUIRE)) schedule();
    irq_unregister(9); sci_fn = NULL; return UACPI_STATUS_OK;
}
struct work { struct work *next; uacpi_work_handler fn; void *ctx; };
static struct work *work_head, *work_tail;
static struct wait_queue work_wait;
static unsigned work_active;
static void acpi_worker(void) {
    /* AML GPE methods must execute on CPU 0 (firmware SMI assumptions). */
    sched_lock(); current_thread->pinned_cpu = 0; sched_unlock();
    schedule();
    for (;;) {
        sched_lock();
        if (!work_head) { sched_block_current_locked(&work_wait); continue; }
        struct work *w = work_head;
        work_head = w->next; if (!work_head) work_tail = NULL;
        uacpi_work_handler fn = w->fn; void *ctx = w->ctx;
        work_active++; sched_unlock();
        fn(ctx); /* May free w: only touch queue-global state after this. */
        sched_lock(); work_active--; sched_unlock();
    }
}
int acpi_host_start(void) { return process_create_kthread(acpi_worker, NULL) != NULL; }
uacpi_handle uacpi_kernel_create_work_item(void) { return kcalloc(1, sizeof(struct work)); }
void uacpi_kernel_free_work_item(uacpi_handle w) { kfree(w); }
void uacpi_kernel_schedule_work(uacpi_work_type type, uacpi_handle item,
    uacpi_work_handler fn, uacpi_handle ctx) {
    (void)type;
    struct work *w = item;
    sched_lock(); w->next = NULL; w->fn = fn; w->ctx = ctx;
    if (work_tail) work_tail->next = w; else work_head = w;
    work_tail = w; wait_queue_wake_one(&work_wait); sched_unlock();
}
uacpi_status uacpi_kernel_wait_for_work_completion(void) {
    while (__atomic_load_n(&irq_active, __ATOMIC_ACQUIRE)) schedule();
    for (;;) {
        sched_lock(); int done = !work_head && !work_active; sched_unlock();
        if (done) return UACPI_STATUS_OK;
        schedule();
    }
}
