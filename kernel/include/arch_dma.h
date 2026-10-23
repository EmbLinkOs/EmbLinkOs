#ifndef _EMBK_ARCH_DMA_H_
#define _EMBK_ARCH_DMA_H_
#include <stdint.h>

/* DMA and the CPU's caches -- docs/RPI4.md P5.
 *
 * On a PC, and on QEMU, a device's DMA is COHERENT with the CPU's caches: a
 * byte the device writes to memory is what the CPU next reads, whatever was
 * cached, and every function here is a no-op. A Raspberry Pi 4's PCIe is
 * NOT coherent. The VL805 reads memory behind the ARM cores' caches (and so
 * misses anything still dirty there) and writes it behind them too (and so
 * the CPU keeps reading a stale cached copy). A driver written for a PC works
 * perfectly on QEMU and sees garbage on the Pi.
 *
 * Two tools, used by drivers that DMA:
 *   arch_dma_uncached()  remap a PAGE-ALIGNED range of the kernel image as
 *                        non-cacheable, for structures both sides touch all
 *                        the time (descriptor rings). Afterwards every CPU
 *                        access goes straight to memory. No atomics or locks
 *                        may live in such a range: exclusive accesses are not
 *                        guaranteed on non-cacheable memory.
 *   arch_dma_flush()     clean AND invalidate a range to memory, for a buffer
 *                        in ordinary cacheable memory: after the CPU writes
 *                        it (before the device reads) and before the CPU
 *                        reads what the device wrote. */
void arch_dma_uncached(void *va, uint64_t len);
void arch_dma_flush(const volatile void *va, uint64_t len);

#endif
