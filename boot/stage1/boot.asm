[BITS 16]

[ORG 0x7C00]

; Number of sectors to load for Stage 2.
; Injected by the Makefile as -D STAGE2_LOAD_SECTORS=<n>.
; The default keeps a plain `nasm boot.asm` working for quick experiments.
%ifndef STAGE2_LOAD_SECTORS
%define STAGE2_LOAD_SECTORS 8
%endif

; Set up the stack



start:
    cli                     ; Clear interrupts
    xor ax, ax              ; Zero out AX
    mov ds, ax              ; Set DS to 0
    mov es, ax              ; Set ES to 0
    mov ss, ax              ; Set SS to 0
    mov sp, 0x7C00          ; Set stack pointer to the beginning of the bootloader
    ; BIOS leaves the drive we were booted from in DL. Save it BEFORE anything
    ; can clobber it: hardcoding 0x80 only ever worked because that is what a
    ; single BIOS hard disk happens to be -- a USB stick (BIOS USB-HDD legacy
    ; emulation) or a second disk arrives with a different DL, and reading the
    ; wrong drive is a dead boot. Honour what BIOS handed us instead.
    mov [boot_drive], dl
    sti                     ; Enable interrupts

    mov si, msg_hello       ; Load the address of the message into SI
    call print_string       ; Call the print_string function

    ; load stage 2 from disk, with retries: a single INT 13h failure is common
    ; on real hardware (especially USB / removable media that reports "not ready"
    ; on the first spin-up), so reset the controller and retry rather than dying
    ; on the first carry. All read parameters are reloaded each attempt because a
    ; failed INT 13h may clobber them.
    mov si, 3               ; attempts remaining (SI is free -- print_string done)
.load_stage2:
    mov ah, 0x02            ; BIOS read sector function
    mov al, STAGE2_LOAD_SECTORS  ; exact stage2 size, computed by Makefile
    mov ch, 0               ; Cylinder 0
    mov cl, 2               ; Sector 2 (first sector is 1)
    mov dh, 0               ; Head 0
    mov dl, [boot_drive]    ; the drive BIOS actually booted us from
    mov bx, 0x7E00          ; Load the sector into memory at 0x7E00
    int 0x13                ; Call BIOS disk interrupt
    jnc .stage2_loaded      ; no carry -> success
    xor ah, ah              ; INT 13h AH=00: reset disk system before retrying
    mov dl, [boot_drive]
    int 0x13
    dec si
    jnz .load_stage2
    jmp disk_error          ; out of retries

.stage2_loaded:
    mov dl, [boot_drive]    ; hand the boot drive to stage2 in DL (int 13h may
                            ; have touched it) -- stage2 reads the kernel from
                            ; this same drive.
    jmp 0x7E00              ; Jump to the loaded stage 2 code

print_string:
    pusha                    ; Save all registers
.loop:
    lodsb                   ; Load byte at DS:SI into AL and increment SI
    or al, al               ; Check if AL is zero (end of string)
    jz .done                ; If zero, jump to done
    mov ah, 0x0E            ; BIOS teletype function
    mov bh, 0x00            ; Page number
    int 0x10                ; Call BIOS video interrupt
    jmp .loop               ; Repeat for the next character
.done:
    popa                     ; Restore all registers
    ret                      ; Return from the function


disk_error:
    mov si, msg_error   ; Load the address of the error message into SI
    call print_string   ; Call the print_string function
    jmp $               ; Infinite loop to halt the system


msg_hello db 'MyOS Stage 1 loading...', 0x0D, 0x0A, 0 ; Message to display (null-terminated)
msg_error db 'Disk read error!', 0x0D, 0x0A, 0 ; Error message (null-terminated)

; The drive number BIOS booted us from. Default 0x80 keeps a hand-assembled
; boot.bin sane, but the real value is stored from DL at entry. Lives well
; before offset 440, so sfdisk's disk-signature + partition table (440..509)
; never overlap it when this sector becomes a partitioned disk's MBR.
boot_drive db 0x80


; Bootloader signature (must be 0x55AA at the end of the 512-byte sector)
times 510 - ($ - $$) db 0 ; Pad the rest of the sector with zeros
dw 0xAA55                  ; Boot signature