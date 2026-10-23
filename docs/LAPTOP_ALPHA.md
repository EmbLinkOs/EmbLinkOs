# Laptop alpha acceptance

The four goals are tracked separately. QEMU acceptance covers real kernel and
userspace paths; it does not emulate the physical Intel I2C controller or a Wi-Fi
radio. Physical validation can follow implementation.

| Goal | Current implementation | Acceptance |
| --- | --- | --- |
| WPA2 Wi-Fi, DHCP, DNS, webpage | Experimental AX200 PCIe firmware/DMA transport, passive scan, authentication/association, four-way/group-key handshake, CCMP, controlled Ethernet delivery and asynchronous DHCP/lease refresh | Independent security vectors and a scripted firmware/MMIO/AP model pass, including encrypted Ethernet and 1,100 TX with queue wrapping. **Physical AX200 connection, DHCP, DNS and webpage acceptance remain unverified.** QEMU Ethernet checks validate the shared stack, not a physical radio. Setup and supported modes: [WIFI.md](WIFI.md). |
| UEFI + NVMe persistence | Relocatable EFI loader, single GPT image containing ESP/kernel and EMBKFS root, NVMe read/write/flush | A 64-KiB file is written and synchronized, then a separate UEFI VM boots the retained NVMe image and verifies every byte. |
| Keyboard/touchpad | Intel LPSS I2C-HID, native modifier/release events, common USB HID decoding, software key repeat, primary-contact touchpad motion/physical buttons | Protocol model tests plus native key/text/repeat tests. GPIO interrupts, touchpad gestures and physical controller validation remain outstanding. |
| Visible battery + shutdown/reboot | Firmware cache, top-bar battery status, capability-gated power requests, ACPI S5/FADT reset with 8042 fallback, disk write freeze/flush | Synthetic battery fixture, unprivileged-read/privileged-write checks, flush failure recovery, and QMP shutdown/reset events. |

## Boot and persistence

`make uefi-usb.img` creates one GPT disk with `/EFI/BOOT/BOOTX64.EFI` and the
EMBKFS root. The same image can be exposed as an NVMe namespace in QEMU. This
image uses 512-byte GPT sectors; bootable native-4-KiB GPT image generation is
not implemented, even though the NVMe block driver supports native-4-KiB disks.

The EFI intermediate must be ET_DYN. Bare-metal GCC does not forward `-shared`
like a hosted compiler, so the link rule explicitly passes `-Wl,-shared` and
`-Wl,-Bsymbolic`. Dynamic metadata has separate ELF output sections; merging it
with `.data` corrupts ordinary data such as compiler jump tables. A build-time
checker verifies the self-relocator's ELF/relocation assumptions.

```sh
make images uefi-usb.img
export EMBLINK_FIRMWARE=uefi EMBLINK_DISK=nvme EMBLINK_CPUS=2
export EMBLINK_OVMF_DIR=/usr/share/OVMF
# Choose a disposable image path. Reusing it deliberately preserves writes.
export EMBLINK_PERSIST_IMAGE=/tmp/emblink-alpha-test.img
EMBLINK_SMOKE_COMMAND='test persist write' \
  EMBLINK_SMOKE_EXPECT='[cmd] test persist write: OK' python3 tools/hardware_smoke.py
EMBLINK_SMOKE_COMMAND='test persist verify' \
  EMBLINK_SMOKE_EXPECT='[cmd] test persist verify: OK' python3 tools/hardware_smoke.py
```

## Power and input

`embk_power_get()` returns a fixed-size, versioned firmware snapshot. Sampling
runs on the ACPI worker every two seconds; UI polling never runs AML. Unknown
capacity, missing batteries, mixed battery units and stale samples are shown
without a fabricated percentage. The top bar offers Restart and Shut Down when
its process holds `CAP_POWER` (ID 11). Snapshot reads do not require this authority.
Power actions are asynchronous; failures are available as `last_error` and shown
in the top bar. Native syscall numbers are 97 (snapshot) and 98 (request).

The power worker runs on CPU 0. It finishes outstanding EMBKFS transactions,
freezes new block writes, flushes devices with pending writes, then runs firmware
power preparation and writes the S5/reset registers. A flush failure prevents the
transition and restores disk writes. Devices without a real flush callback cannot
silently clear pending durability. This does not save unsaved application buffers
or promise transaction-level shutdown safety for writable FAT32 volumes; the alpha
root is EMBKFS. Suspend/resume is not implemented.

USB and I2C HID keyboards share the native physical-key/layout decoder. Repeat
starts after 450 ms and then runs approximately every 33 ms (quantized by the
10-ms timer). Release cancels repeat; modifiers and lock keys do not repeat.
Legacy USB boot reports now supply modifier changes and key releases as well as
text. The xHCI disconnect path still needs full device-removal handling.

```sh
TEST_CFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' tests/drivers/run.sh
for test in hidkeys hidrepeat powerbarrier powerfailure powercaps; do
  EMBLINK_SMOKE_COMMAND="test $test" EMBLINK_SMOKE_EXPECT="[cmd] test $test: OK" \
    python3 tools/hardware_smoke.py
done
iasl -p /tmp/battery-live tests/acpi/battery-live.asl
EMBLINK_ACPI_TABLE=/tmp/battery-live.aml EMBLINK_SCREENSHOT=1 \
  EMBLINK_SMOKE_COMMAND='test powercache' EMBLINK_SMOKE_EXPECT='[cmd] test powercache: OK' \
  python3 tools/hardware_smoke.py
EMBLINK_SMOKE_COMMAND=poweroff EMBLINK_POWER_TEST=poweroff python3 tools/hardware_smoke.py
EMBLINK_SMOKE_COMMAND=reboot EMBLINK_POWER_TEST=reboot python3 tools/hardware_smoke.py
```

The power tests require the corresponding QMP `guest-shutdown` or `guest-reset`
event after the storage synchronization marker. A process exiting for another
reason is a failure. Each run prints its retained logs/artifact directory.

## Desktop controls

Settings and the top bar now expose Wi-Fi, sound, standard ACPI brightness and
power information. See [LAPTOP_DESKTOP.md](LAPTOP_DESKTOP.md) for implemented
controls, driver limits and acceptance commands. Physical laptop compatibility
and full suspend/resume remain unverified or unimplemented as described there.

Installation, read-only recovery, authenticated sessions, file operations and
power policies are tracked in [DAILY_USE.md](DAILY_USE.md). Physical acceptance
and native GPU modesetting/suspend remain outstanding.
