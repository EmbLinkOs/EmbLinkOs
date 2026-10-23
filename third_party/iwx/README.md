`if_iwxreg.h` is unchanged OpenBSD iwx register/firmware wire documentation,
revision 1.75 (2026-05-28), downloaded from
https://raw.githubusercontent.com/openbsd/src/master/sys/dev/pci/if_iwxreg.h.

The AX200 transport's power/context/command sequence is adapted from
`if_iwx.c` revision 1.230 (2026-09-19), at
https://raw.githubusercontent.com/openbsd/src/master/sys/dev/pci/if_iwx.c.
Original notices are in LICENSE and copied into the OS image at
`/system/licenses/iwx.txt`. Intel's dual-licensed portions use the BSD option.

EmbLinkOS integration lives in `kernel/net/wifi/ax200*.c`. This does not import
OpenBSD's kernel or networking subsystem. Firmware is separately licensed;
see `system/firmware/intel/README.md`.
