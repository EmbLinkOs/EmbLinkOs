#ifndef ARCH_AARCH64_BOARD_H
#define ARCH_AARCH64_BOARD_H

/* Which aarch64 machine this kernel is built for -- docs/RPI4.md §2.1.
 *
 * A build-time choice, not a runtime one, and deliberately so: the two boards
 * disagree about where RAM IS, and the kernel is linked at a fixed physical
 * address inside it (linker.ld). A kernel that could run on both would have to
 * be physically relocatable, which is a real project for no user-visible gain.
 * `make ARCH=aarch64 BOARD=rpi4` passes -DBOARD_RPI4; the default is `virt`.
 *
 * Only what is needed BEFORE the device tree can be read lives here: the
 * console for the boot beacon and the first kprintf, and the shape of physical
 * memory for boot.S's first page tables. Everything after that comes from the
 * device tree, on both boards, exactly as ARM64.md §5 prescribes.
 *
 * Plain #defines only: boot.S includes this file too. */

#if defined(BOARD_RPI4)

/* Raspberry Pi 4 Model B (BCM2711).
 *
 * DRAM starts at 0 and the firmware loads kernel8.img at 0x80000 (config.txt
 * pins kernel_address so a firmware update cannot move it). Peripherals sit at
 * the top of the 4th GiB in "low peripheral" mode, which is the default:
 * 0xFC000000..0xFF800000, the GIC-400 at 0xFF840000.
 *
 * UART0 is the PL011, at bus address 0x7E201000 = ARM 0xFE201000. The firmware
 * gives it GPIO 14/15 only with dtoverlay=disable-bt (otherwise the Bluetooth
 * module has it and the header pins get the mini-UART), and clocks it at
 * 48 MHz (init_uart_clock's default). */
#define BOARD_NAME              "rpi4"
#define BOARD_UART_PHYS         0xFE201000
#define BOARD_UART_CLK_HZ       48000000
#define BOARD_RAM_BASE          0x00000000

#else

/* QEMU `-M virt`: DRAM at 1 GiB, devices below it, a 24 MHz PL011 at
 * 0x09000000. hw/arm/virt.c's memory map is a documented, stable contract. */
#define BOARD_VIRT              1
#define BOARD_NAME              "virt"
#define BOARD_UART_PHYS         0x09000000
#define BOARD_UART_CLK_HZ       24000000
#define BOARD_RAM_BASE          0x40000000

#endif

#endif
