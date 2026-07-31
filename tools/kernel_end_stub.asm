; kernel_end stub for the on-OS embld link (docs/BUILD.md §12, the G2/L1 bridge).
;
; The host kernel/linker.ld ends with `kernel_end = .`, the true end of .bss, and
; pmm.c places the PMM bitmap there. EmbLD has no linker-script symbol assignment
; yet (todo L1 / gap G2), so this stub supplies `kernel_end` as a fixed absolute
; address, chosen safely PAST any embcc-built kernel image: the kernel loads at
; 0xFFFFFFFF80100000 and is ~1.3 MB, so 0x...80800000 (7 MB of span) never
; overlaps it -- over-estimating only parks the PMM bitmap a little higher in RAM.
;
; This is a temporary bridge so EmbBuild can rebuild + link the kernel on the OS
; today (KM1). Retire it the moment EmbLD emits kernel_end itself (L1), after
; which the real kernel/linker.ld value is used and this file is deleted.
global kernel_end
kernel_end equ 0xFFFFFFFF80800000
