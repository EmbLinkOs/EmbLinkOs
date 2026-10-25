# System and application updates

Software Update in Settings talks to a supervised system service. It checks a
signed release catalog, installs a complete OS image and paired EFI loader in
the inactive system slot, verifies disk readback, and offers a restart. The
current slot stays read-only and running throughout preparation. A new boot is
confirmed after the real desktop and top bar have presented frames, the update
service is alive, and those conditions remain true for five seconds. An
unconfirmed trial gets one boot attempt; the next boot selects the confirmed
system. Settings can also select the retained previous system without a download.

This is currently an x86-64 UEFI feature with 512-byte logical sectors. Existing
legacy two-partition installations continue to boot. Their Settings pane explains
that a managed installation is required; the updater never repartitions a live
installation or silently migrates personal data. No official release endpoint or
production signing key is included. Unconfigured, offline, invalid-signature,
unsupported-layout, failed-write and restart-required states are reported directly.

## Disk layout and persistence

| Partition | Contents | Update behavior |
| --- | --- | --- |
| ESP | Stable boot manager, two preallocated EFI slots, boot journal | Write inactive EFI data only; flush journal separately |
| OS-A | Complete kernel-compatible userspace deployment | Read-only when selected |
| OS-B | Alternate complete deployment | Never mounted while staging |
| Data | Accounts, homes, preferences, developer files, app versions | Shared across boots and system rollback |

The image builder defaults to a 64 MiB ESP and two 128 MiB OS partitions. The
Data partition defaults to 128 MiB and grows to fit the target during recovery
installation. Releases must fit the existing inactive OS partition. Capacity
changes require a separately planned installation/migration; the updater refuses
oversized images. Recovery installation generates fresh disk/partition identities
and rewrites all three boot records to those identities. It preserves both OS
payloads byte-for-byte and grows only Data. It requires the existing reviewed
`install erase` plan and a different unmounted target disk.

Managed boots identify Root, Data and ESP by exact GPT GUIDs on the actual boot
disk. The OS is mounted read-only before any filesystem self-healing. `/data`,
`/home` and `/etc` come from Data. `/data/apps` resolves published package
versions first, legacy installed apps second, and the selected OS's built-in
applications last. Open handles retain the old vnode during app publication.
Read-only volume checks happen before writable opens or page-cache writes;
file mappings preserve descriptor authority and private mappings retain COW
when protections change.

## Journal and firmware handoff

`kernel/update/protocol.{c,h}` is shared by the kernel and EFI manager. Records
are explicit little-endian 512-byte structures with a schema version, revision,
CRC32, exact identities, active/pending/previous slots, one trial attempt,
release versions, sizes and SHA-256 digests. Independent 4 KiB journal pages are
at ESP offsets 16 KiB and 32 KiB. A factory recovery record is at 48 KiB.
Managed FAT reserves at least 128 sectors and carries the `EMBUPD1 ` marker.

A staging BEGIN durably invalidates the previous slot's eligibility **before**
any overwrite. Payload writes are bounded and sequential; the kernel chooses
all target devices from the boot journal. The worker cannot pass a disk name.
COMMIT flushes the devices and reads back/hashes both images in bounded chunks,
allowing status polling between calls. Only then is a pending trial published.
Interrupted staging leaves the confirmed slot selected. Ambiguous or damaged
control records fail closed into recovery. A journal I/O failure disables further
runtime mutations until reboot.

EFI slot files are fixed 16 MiB allocations. Staging writes only their existing
data sectors, leaving FAT chains and file sizes unchanged. The firmware reads
exactly the journal's EFI byte count, verifies its SHA-256, and calls LoadImage
with that verified buffer. The selected loader embeds and checks its paired
kernel. The stable manager retains an embedded recovery loader. This is release
manager protocol 1: catalogs requiring another manager version are rejected.
Replacing the stable first-stage manager itself is a separate recovery operation,
not an in-place automatic update of the running boot path.

The x86 kernel is linked at physical 32 MiB, clear of firmware reservations near
8 MiB and the BIOS ELF staging buffer at 16 MiB. Both boot paths still load the
ELF's linked addresses; the higher-half mapping convention is unchanged.

## Trust and authority

A 448-byte `release.emu` is signed with ECDSA P-256 over its first 384 bytes
(SHA-256). The canonical descriptor fixes release sequence, architecture, native
ABI, boot-manager protocol, version, exact OS/EFI lengths and hashes, and bounded
release notes. Unknown formats, trailing padding, unsupported ABI/architecture,
invalid sizes and bad signatures are rejected. Install rechecks the signed
catalog instead of trusting an earlier Check result. A candidate must be newer
than the confirmed release and cannot replace a remembered higher release with
older or different bytes. Manual rollback selects only the retained local slot.

The trusted public key and source are sealed in `/system/update/key` (64-byte
P-256 X||Y) and `/system/update/source`. Production transport requires HTTPS with
the existing certificate-verifying TLS stack. HTTP is restricted to the explicit
local fixture prefixes `10.0.2.2` and `127.0.0.1` with a port. The downloader accepts
an exact Content-Length and HTTP 200; it rejects redirects, transfer codings,
compression, truncation and hash mismatches. Images stream into the inactive
slot, so a second full OS download on Data is unnecessary.

The committed package development key is **not** OS update trust. Tests generate
an external temporary key. Publisher builds supply `EMBK_UPDATE_KEY` and
`EMBK_UPDATE_SOURCE` together. Keys are not generated silently, and private keys
must never be copied into an image or committed.

Native syscall 153 (`update`) exposes status and bounded requests. Mutation
requests require the authenticated kernel session's administrator flag and
power authority. Init sets administrator identity after account authentication;
`USER` or other environment values cannot supply it. Internal staging, progress,
commit and abort operations are bound to one supervised worker PID started by
init outside all user sessions. A privileged application cannot act as that
worker. Its restart clears interrupted transactions before accepting another job.

## Application packages

`pkg install` verifies the signed manifest, EMBX build ID, ABI and declared
capabilities before publication. Each complete bundle is written under
`/data/pkg/apps/<name>/versions/<64-hex-id>`, including binary, manifest and
generated namespace declaration. Files and directories are synced before an
atomic, synced CURRENT replacement publishes current/previous IDs. Rollback
revalidates the retained bundle and flips the pointer; it does not copy files
over a running version. Interrupted unpublished versions are harmless orphans.
A per-package lock refuses concurrent publishers. An interrupted installer may
leave a lock; its diagnostic gives the exact path for recovery after checking
that no installer is running. Old immutable versions are retained; garbage
collection is not yet automatic. Deactivation reveals any underlying OS app.

The configured system key also verifies packages on publisher builds. Manual
legacy development builds retain the existing package development workflow,
including explicit `--local`. Invalid signatures cannot trigger a remote asset
fetch. Authority widening still requires explicit `--allow-widen`; automatic
batch updates never grant it.

An optional sealed HTTPS registry URL is supplied with
`EMBK_UPDATE_APPS_SOURCE`. Settings then offers Update Applications. The service
runs sealed package/transport helpers, syncs the registry, and invokes
`pkg update-all`. Installed packages that need broader permissions remain for
explicit review. Built-in applications and shared libraries are replaced as part
of the OS image. Updates to separately installed apps take effect on relaunch.
Unconfigured application registries are shown honestly in Settings.

## Build and publish

Normal builds have no release trust configured. Build a managed image from the
same completed OS/EFI build:

```sh
make managed-image UPDATE_RELEASE=1 UPDATE_VERSION=1.0.0
```

For a publisher build, set both external public-key and immutable channel URL
inputs before rebuilding the filesystem image. Changing those environment
inputs requires an explicit filesystem repack. Supply the matching private key
only to the publisher outside the repository:

```sh
EMBK_UPDATE_KEY=/external/keys/public-key.bin \
EMBK_UPDATE_SOURCE=https://updates.example.org/stable/x86_64 \
  make -W tools/embkfs_mkfs/mkfs_embkfs.py managed-image
python3 tools/update/publish.py --root embkfs.img --efi build/BOOTX64.EFI \
  --key /external/keys/private.pem --release 2 --version 1.1.0 \
  --notes 'Release notes for this system.' --output /external/releases/1.1.0
```

The publisher requires a new output directory, signs exact artifact hashes, and
emits `release.emu`, `os.img`, `boot.efi`, and the public key. Publish immutable
asset sets together at the configured channel; never switch a channel while
clients could observe a catalog from one release and assets from another.
Versioned server directories with an atomic channel switch are suitable. The
client rejects mismatches even if a server publishes an inconsistent set.

`updatectl status|check|install|rollback|apps` exposes the same engine to the
native shell. Restart remains explicit and uses the existing system power flow.

## Validation tools and boundaries

- `python3 tests/update/run.py`: sanitizer-enabled boot journal transitions,
  corruption/torn records, publisher/C codec parity, and signatures checked by
  the production release parser and ECDSA implementation.
- `tests/drivers/run.sh`: existing device protocols and legacy/managed installer
  validation, including new GUIDs, immutable OS partitions and Data expansion.
- `tools/update_smoke.py`: disposable NVMe guest, real administrator setup/login,
  native file creation, saved preferences, signed HTTP fixture, Settings actions,
  upgrade and manual rollback across cold boots, real frame captures.
- `tools/update_failure_smoke.py`: package regressions, bad signatures, truncated
  and altered assets, hard interruption, unloadable signed EFI, and confirmed
  OS hash preservation across rejection/fallback.
- `tools/update_install_smoke.py`: recovery-install a managed image onto a
  disposable NVMe target, verify the source hash is unchanged, and boot the
  installed target alone with read-only OS and writable account setup.
- `tools/update_legacy_apps_smoke.py`: import a real legacy signed bundle,
  update/rollback it, remove its modern and legacy copies, and reinstall.
- `updateprobe`: native administrator/worker isolation and immutable/file-mapping
  checks. Run on a disposable managed image through the kernel debug console.

These tools never select a host block device. Physical laptop/firmware testing,
a production signing/hosting service, safe legacy Data migration, larger-slot
migration, stable-manager protocol upgrades, automatic package garbage collection,
and scheduled unattended updates are separate remaining work. ARM64 can compile
shared code but does not yet have the managed UEFI update boot path.
