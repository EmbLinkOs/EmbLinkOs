# EmbBoot — Design

*Status: design. Nothing here is built yet. This is the plan to turn the
from-scratch UEFI loader (`boot/uefi/`) into a menu-driven boot manager with its
own signed boot-payload format (`.embfw`) and its own verification layer.*

```
EmbBoot
  ├── Boot EmbLinkOS
  ├── Recovery
  ├── Diagnostics
  ├── Firmware Update
  ├── Boot Manager
  └── Secure Boot Verification
```

---

## 1. What EmbBoot is — and the layer it lives in

EmbBoot is a **UEFI boot manager application** — our own `.efi`, the direct
evolution of `boot/uefi/loader.c`. It is **not firmware**. This distinction is
load-bearing, so state it plainly:

```
Platform firmware  (OVMF on QEMU / the motherboard UEFI on real hardware)
      │  POSTs the platform, provides Boot Services, then loads:
EmbBoot            (our .efi — everything in the tree above lives here)   ← OURS
      │  presents the menu, verifies + loads:
A payload          (EmbLinkOS kernel, a recovery kernel, a diagnostic tool)
```

We own EmbBoot completely and can make it do anything a UEFI application can.
We do **not** own or replace the platform firmware — that is coreboot territory
and is an explicit **non-goal** (see §10). GRUB, systemd-boot and rEFInd sit at
exactly EmbBoot's layer; EmbBoot is that, built our way, with our formats.

The symmetry with the rest of the project is the point: the OS has its own
executable format ([EMBX](EMBX_Specification_v2.md)); EmbBoot gets its own
boot-payload format (`.embfw`). Same philosophy — a self-describing, capability-
carrying, verifiable container — one layer down.

## 2. Goals and non-goals

**Goals**
- A menu (text first, GOP-graphical later) driven by a config the user controls.
- Our own boot-payload container, **`.embfw`** — self-contained and signed.
- **Verification we own**: every payload is checked against a trust root baked
  into EmbBoot before it runs. This is the "Secure Boot Verification" node.
- Recovery and Diagnostics as first-class menu entries, not afterthoughts.
- Safe self-update (EmbBoot + payloads) with A/B fallback.
- Reuse, not reinvention: OS entries go through the **exact** load-kernel +
  `boot_protocol` + `ExitBootServices` handoff we already wrote and proved.

**Non-goals (for now — see §10 for why)**
- Replacing the platform firmware (coreboot / a custom `-bios`).
- Flashing the motherboard SPI chip / `UpdateCapsule` platform-firmware update.
- Enrolling into UEFI Secure Boot's `db`/`KEK`/`PK` key hierarchy.
- Network boot (PXE/HTTP). A plausible later entry, out of the first design.

## 3. Architecture

EmbBoot is `boot/uefi/` grown up. Keep the one-file-per-concern discipline the
rest of the tree uses:

```
boot/embboot/
  uefi.h          the firmware surface (grows: SimpleTextInput, LoadImage,
                  SimpleFileSystem, config-table already there)
  crt0.S efi.lds  unchanged (self-relocating PIE + PE layout)
  main.c          efi_main: init console, load config, run the menu loop
  menu.c          the menu model + renderer (text now; a GOP renderer later)
  input.c         keyboard via EFI_SIMPLE_TEXT_INPUT_PROTOCOL (ConIn)
  fs.c            read files off the ESP (EFI_SIMPLE_FILE_SYSTEM_PROTOCOL)
  embfw.c         parse + validate an .embfw container
  verify.c        the trust root + signature check (the "Secure Boot" engine)
  boot_os.c       load a kernel payload + hand off (today's loader.c logic)
  boot_tool.c     run a UEFI-context tool payload (diagnostics) without exiting
  entries/        recovery.c, diagnostics.c, fwupdate.c, bootmgr.c
tools/
  mkembfw.py      HOST tool: package a payload into a signed .embfw
```

Two execution modes matter:

- **OS handoff** (Boot EmbLinkOS, Recovery): build page tables, fill
  `boot_protocol`, `ExitBootServices`, jump to the kernel. Firmware is gone
  after this. This is `boot/uefi/loader.c`'s existing path, factored into
  `boot_os.c`.
- **In-firmware tool** (Diagnostics, Firmware Update, Boot Manager editing):
  runs *with Boot Services still up*, so it can use firmware for the memory map,
  disk I/O, console, and clock, and then **return to the menu**. Never calls
  `ExitBootServices`.

The menu loop, then, is: *load config → draw → wait for input or timeout →
dispatch entry → (OS entries never return; tool entries return to the menu)*.

## 4. The `.embfw` format

One `.embfw` is **one bootable payload**: a self-contained, signed container
carrying a kernel or a tool plus the metadata EmbBoot needs to name it, verify
it, and boot it. EmbBoot builds the menu by reading a small config that lists
`.embfw` files (§5). Modelled on the EMBX container, one layer down.

**Illustrative header (little-endian; final byte layout is TBD in the spec):**

| Off  | Size | Field            | Notes                                            |
|------|------|------------------|--------------------------------------------------|
| 0x00 | 8    | `magic`          | `"EMBFW\0\0\0"` — sniffed like EMBX/ELF          |
| 0x08 | 4    | `format_version` | container version                                |
| 0x0C | 4    | `header_size`    |                                                  |
| 0x10 | 4    | `entry_type`     | KERNEL / RECOVERY / DIAGNOSTIC / CHAINLOAD       |
| 0x14 | 4    | `payload_format` | ELF / EMBX / PE (chainload) / RAW                |
| 0x18 | 8    | `payload_offset` | where the payload starts in the file             |
| 0x20 | 8    | `payload_size`   |                                                  |
| 0x28 | 8    | `capabilities`   | EMBX-style bitmask the OS is *told* it was granted|
| 0x30 | 32   | `payload_sha256` | hash of the payload bytes                         |
| 0x50 | 8    | `build_id`       | provenance (build stamp)                          |
| 0x58 | 64   | `name`           | UTF-8, what the menu shows                         |
| 0x98 | 192  | `description`    | UTF-8                                             |
| 0x158| ...  | `signature`      | alg id + signature over `[0 .. signature)` (§7)  |
| ...  | ...  | `payload`        | the ELF / EMBX / PE image                          |

Design points:
- **Self-describing.** `entry_type` + `payload_format` tell EmbBoot how to boot
  it (OS handoff vs in-firmware tool; parse as ELF vs EMBX vs chainload a PE).
- **Capabilities travel with the payload** and flow into `boot_protocol` for OS
  entries — so "this recovery kernel may touch storage but not the network" is
  expressible at the boot layer, consistent with EMBX's model.
- **The hash is separate from the signature** so verification is two clean
  steps: recompute `payload_sha256`, then check the signature over the header.
- **Sniffed by magic**, exactly like the kernel's dual EMBX/ELF loader — a file
  that isn't a valid `.embfw` is rejected, not guessed at.

## 5. Config: the menu itself

A tiny text config on the ESP defines the menu — order, default, timeout — by
referencing `.embfw` files. Kept separate from the payloads so re-ordering the
menu never re-signs a kernel.

```
# /EFI/EmbLink/embboot.cfg
default   = emblinkos
timeout   = 5            # seconds; 0 = no menu, boot default; -1 = wait forever
verify    = enforce      # enforce | warn | off  (the Secure Boot policy, §7)

[emblinkos]  title = "Boot EmbLinkOS"   file = emblinkos.embfw
[recovery]   title = "Recovery"         file = recovery.embfw
[diag]       title = "Diagnostics"      builtin = diagnostics
[fwupdate]   title = "Firmware Update"  builtin = fwupdate
```

`builtin = …` entries are code compiled into EmbBoot (Diagnostics, Firmware
Update, Boot Manager); `file = …` entries are `.embfw` payloads. Boot Manager
and Secure Boot Verification are also builtins.

**ESP layout:**
```
/EFI/BOOT/BOOTX64.EFI          EmbBoot (the removable-media default path)
/EFI/EmbLink/embboot.cfg       the menu
/EFI/EmbLink/emblinkos.embfw   the OS payload
/EFI/EmbLink/recovery.embfw    the recovery payload
/EFI/EmbLink/trust.pub         (v2) the public trust root, if not embedded
```

## 6. The six menu entries

### Boot EmbLinkOS  — *done in substance*
Load `emblinkos.embfw` → verify (§7) → parse the payload (ELF today, EMBX later)
→ the existing page-table build + `boot_protocol` fill + `ExitBootServices` +
jump. This is today's `loader.c`, minus the *embedded* kernel: the kernel now
comes from the `.embfw` on the ESP instead of being baked into the `.efi`. That
alone is a nice win — EmbBoot stops being rebuilt every time the kernel changes.

### Recovery
A second OS payload, `recovery.embfw`: a known-good kernel plus a recovery root
(a small recovery EMBKFS image, or a ramdisk). Booted through the *same* handoff
as the normal OS, but flagged so the OS comes up in recovery mode — mount root
read-only, expose repair/reinstall tools, don't auto-start the desktop. The flag
rides `boot_protocol` (a new `boot_mode` field, §8).

### Diagnostics  — *in-firmware tool*
Runs with Boot Services up, returns to the menu. First cut:
- **Memory**: walk the memory map (firmware gives it to us), report totals and
  a light-touch pattern test over free conventional memory.
- **Storage**: enumerate block devices, read/verify a few sectors, dump the
  partition tables (GPT/MBR) — we already have parsers kernel-side to port.
- **Hardware inventory**: PCI enumeration, ACPI tables present, CPU/features,
  the framebuffer mode GOP reports.
Later it can become its own diagnostic *kernel* (`.embfw`, DIAGNOSTIC type) when
it needs more than firmware services give.

### Firmware Update  — *scoped carefully*
Three different things wear this name; be explicit about which we do:
- ✅ **Update EmbBoot + payloads** (what we build): write a new `BOOTX64.EFI`
  and/or `.embfw` to the ESP via the file-system protocol, **A/B**: stage to a
  slot, verify, then atomically swap the default (the same stage-then-adopt
  discipline EMBKFS and the git port use). A failed update never bricks the
  boot — the previous EmbBoot stays as the fallback entry.
- ⚠️ **Platform firmware update** (OVMF / motherboard UEFI): `UpdateCapsule`,
  vendor-specific, brick-risky, limited under QEMU. **Out of scope** (§10).

### Boot Manager
Enumerate and manage boot options: scan `/EFI/EmbLink/*.embfw`, read the config,
optionally surface UEFI `BootOrder`/`Boot####` variables and other ESPs. Edit the
default and timeout, boot a one-off entry, chainload another `.efi`
(`LoadImage`/`StartImage`) for a foreign OS. Writes go back to `embboot.cfg`.

### Secure Boot Verification
Not really a menu *destination* — it's the **policy panel** for the engine in §7:
show each payload's verification status (signed-by, hash, pass/fail), toggle the
policy (`enforce`/`warn`/`off`), and view the measured-boot log (§7). Booting an
entry always runs the check; this node makes it visible and configurable.

## 7. The trust model (Secure Boot, our way)

EmbBoot verifies every payload against a **trust root baked into EmbBoot** before
running it. Two honest tiers, because the crypto we have today is symmetric:

**v1 — HMAC-SHA256 (what we can build now).** We already ship SHA-256 + HMAC
(`kernel/crypto/`, used for EMBKFS verified-root). `mkembfw.py` computes
`payload_sha256` and an HMAC over the header with a key; EmbBoot recomputes and
compares. **Honest boundary:** this is *tamper-evidence*, not public-key secure
boot. The key is symmetric and lives inside EmbBoot, so anyone who extracts it
can forge a payload. It defeats accidental corruption and casual tampering, and
it's the same trust model as EMBKFS verified-root — but say what it is.

**v2 — asymmetric signatures (Ed25519).** The real thing: the **private** signing
key stays on the build host, EmbBoot embeds only the **public** key, and a stolen
device reveals nothing that lets an attacker sign. This needs an Ed25519 (or
minimal RSA) implementation we don't have yet — a real, self-contained crypto
addition, and the point at which "Secure Boot Verification" earns its name.

**Policy** (`verify` in the config): `enforce` refuses to boot a failed payload;
`warn` boots but flags it; `off` skips the check (dev only). Default `enforce`.

**Relationship to UEFI Secure Boot.** Ours is *independent of and layered on*
firmware Secure Boot. If firmware SB is enabled, it verifies **EmbBoot itself**
(EmbBoot would need signing/enrolling — a later concern); EmbBoot then verifies
**payloads** with *our* root. The two don't conflict; ours works even with
firmware SB off.

**Measured boot (future).** EmbBoot records each payload's hash into a small log
passed to the OS via `boot_protocol` (a `measure_log_phys` field), so the running
system — or a remote attestor — can see exactly what was booted. Cheap to add
once the hashing is in place; deferred past the first cut.

## 8. Relationship to `boot_protocol`

OS entries already hand off through
[`boot_protocol`](../kernel/arch/x86_64/boot/boot_protocol.h). EmbBoot extends it
additively (the header's documented growth rule: append, bump `size`, both
loaders write it):
- `boot_mode` — NORMAL / RECOVERY, so the OS knows how it was booted.
- `granted_caps` — the `.embfw`'s capabilities, carried into the OS.
- `verify_result` — passed / warned / skipped, so the OS can react to an
  unverified boot.
- (future) `measure_log_phys` — the measured-boot log.
BIOS stage2 writes zero/NORMAL for these, exactly as it does for `acpi_rsdp`.

## 9. Phasing

Each phase is independently useful and verifiable under OVMF (`make run-uefi`):

- **M1 — Menu skeleton.** SimpleTextInput + a text menu loop + one live entry
  ("Boot EmbLinkOS") wired to today's load path. Kernel still embedded. Proves
  the interaction model. *An afternoon.*
- **M2 — `.embfw` + ESP loading.** Define the container, write `mkembfw.py`,
  load the kernel from `emblinkos.embfw` on the ESP (SimpleFileSystem) instead
  of embedding it. Menu driven by `embboot.cfg`.
- **M3 — Verification (v1 HMAC).** `verify.c` + the trust root + the Secure Boot
  Verification panel + the `enforce`/`warn`/`off` policy.
- **M4 — Recovery + Diagnostics.** The recovery payload + `boot_mode`; the
  in-firmware diagnostics tool.
- **M5 — Boot Manager + self-update.** Enumerate/edit/default/timeout; A/B
  EmbBoot + payload update.
- **Future.** GOP graphical menu; Ed25519 (v2 secure boot); measured boot;
  network boot; the one-universal-image (BIOS + UEFI + EMBKFS on one disk).

## 10. Honest boundaries (restate, so nobody over-claims)

- **EmbBoot is a bootloader, not firmware.** It runs *on* the platform firmware.
  Replacing that firmware (coreboot / custom `-bios`) is a separate, much bigger
  project and a non-goal here.
- **No SPI flashing / platform-firmware update.** "Firmware Update" means
  EmbBoot + payloads on the ESP, not the motherboard's firmware.
- **v1 verification is HMAC (symmetric)** — tamper-evidence, not public-key
  secure boot. True secure boot waits on v2 (Ed25519). Don't call v1 what it
  isn't.
- **We do not own the UEFI Secure Boot key hierarchy** (`db`/`KEK`/`PK`); our
  verification is a layer on top, not a replacement for it.

## 11. Firmware backends (BIOS + UEFI), GRUB-style

GRUB is not a UEFI program — it's a firmware-agnostic boot manager with a
platform-independent **core** (menu, config language, filesystem/disk drivers,
commands) sitting on a thin per-platform **backend** (`i386-pc` for BIOS,
`x86_64-efi` for UEFI, plus coreboot, OpenFirmware, …). One `grub.cfg` drives
them all. EmbBoot can follow the same shape, and we're already halfway there.

**The split we want:**

```
        EmbBoot CORE  (firmware-independent):
        menu · embboot.cfg · .embfw parse · verify · fill boot_protocol
                 ╱                                              ╲
   BIOS backend (stage2 grown up):              UEFI backend (our .efi):
   NO firmware services after boot -->          Boot Services available -->
   carries its OWN EMBKFS/disk reader,          leans on SimpleFileSystem,
   its own VBE modeset, its own memory          GetMemoryMap, GOP. Already
   map (e820). More machinery.                  built (boot/uefi/).
```

**What's already shared — the handoff.** `boot_protocol` is *the* unifying
contract, and both backends already fill it: `stage2.asm` (BIOS) and
`boot/uefi/loader.c` (UEFI) hand the kernel the identical struct. So the hardest
part of GRUB-style unification — "one handoff regardless of firmware" — is done.
The `.embfw` / menu / verify core would fill the *same* struct from either side.

**The asymmetry to respect (same one GRUB has).** After BIOS hands off there are
no firmware services: to read `.embfw` files *from a filesystem* the BIOS backend
must carry its own EMBKFS + partition reader in the boot environment. Today
`stage2` sidesteps this by reading the kernel from a fixed raw LBA — fine for one
kernel, not enough for a file-driven menu. GRUB solves it with `stage1.5` (just
enough filesystem code to load the full core off a real filesystem). The
kernel-side EMBKFS reader exists but is 64-bit kernel code, not usable in the
16/32-bit boot environment, so the BIOS backend needs its own port — the real
cost of a BIOS EmbBoot.

**Two ways to pay that cost (a decision for later):**
- **(a) Port a minimal EMBKFS reader into the BIOS backend** — true GRUB
  `stage1.5` style. Most work, fully self-contained, boots the menu in real/
  protected mode.
- **(b) Chainload a 64-bit EmbBoot core** — `stage2` loads a small long-mode
  "EmbBoot core" payload (which *does* have full drivers) that presents the menu
  and then loads the chosen `.embfw`. Less duplicated driver code (reuse the
  64-bit EMBKFS reader), at the cost of a two-stage BIOS boot. Likely the better
  trade for us, since we already own a 64-bit EMBKFS reader.

**Honest scoping.** The UEFI backend is the pragmatic first (and maybe only)
target: real hardware is nearly all UEFI now, and vendors are removing BIOS/CSM.
So **a BIOS EmbBoot backend is optional and deferred** — worth designing the core
firmware-agnostic *now* (so it can slot in), but not worth building until BIOS
boot is a goal in its own right. Until then, BIOS keeps its current direct
`stage1`/`stage2` → kernel path (no menu), and UEFI gets the full EmbBoot. The
`boot_protocol` foundation means adopting the BIOS backend later costs nothing
already shipped.

---

*Next concrete step when we start: M1 — the menu skeleton over the existing
loader. Everything else grows one entry at a time from there.*
