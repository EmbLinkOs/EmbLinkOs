# Packaging & the SDK — authority-declaring bundles

*Design record. Builds directly on [USERSPACE_v2.md](USERSPACE_v2.md) (authority
IS the namespace), [EMBX](own-exe-format) (the capability-declaring executable),
and EmbBuild.*

> **Status: PK1 + PK2 SHIPPED + metal-proven** (§11).
> **PK1** — the manifest format (§3), `user/pkg/` (manifest parser + EMBX
> reader/`build_id` verifier), and `pkg` (`verify`/`install`/`run`/`list`).
> `test pkg`: installs a staged bundle (recomputes the EMBX `build_id`,
> cross-checks caps/abi against the manifest, presents the declared authority,
> adopts into `/data/apps/<name>/` writing the `.ns` home enforces), then
> `pkg run` spawns it under EXACTLY its declared caps (SET_CAPS) + namespace
> (NS_BIND) — the app holds only `filesystem`, reaches `/system`, CANNOT name
> `/data/users` — and a tampered bundle (build_id fails to recompute) is refused.
> **PK2** — the SDK generator `tools/embx/pkggen.py`: ONE `.pkgspec` (name,
> version, caps, grant) → all three views (EMBX cap table, `.ns`, package
> manifest), consistent BY CONSTRUCTION — mutate the spec's caps and both the
> binary's cap table AND the manifest follow; you cannot build a self-disagreeing
> bundle (§4). `pkgprobe`'s authority is declared once in
> `user/pkg/pkgprobe.pkgspec`.
> **PK3 SHIPPED** — (a) **signing**: every manifest is signed at build time
> (`tools/embx/pkgsign.py`, ECDSA P-256 over the canonical manifest); `pkg`
> VERIFIES it against the trusted key baked into `user/pkg/pkgkey.h` (our own
> `ecdsa_verify`) and refuses unsigned/altered/wrongly-keyed manifests. (b)
> **update + rollback + authority re-negotiation**: `pkg install` over an
> installed version retains the previous bundle as the rollback point (§6 — each
> app is self-contained, so its 3 files ARE the rollback point, no whole-FS
> snapshot needed), and REFUSES an update that WIDENS caps/namespace unless
> `--allow-widen` — a new version cannot silently widen its reach. `pkg
> rollback/remove/info` round it out. `test pkg` (8 checks, metal) proves
> tampered-binary + altered-signature rejection, update, rollback, refused
> widening, and consented widening. *Remaining:* an EMBKFS-snapshot-backed
> variant of update (a future optimization once a snapshot syscall exists); PK4
> the git registry; and PK2b, driving pkggen from an EmbBuild `build.ebm`
> `package:` stanza on-device.
> The rest of this doc is the shape decided before code, like the userspace.*

## 0. Thesis

> **A package is a self-contained, authority-declaring bundle. The SDK's job is to
> make declaring that authority part of building. Installing is granting it — and
> the app physically cannot exceed the grant.**

Everything below is a consequence of that sentence and of choices the OS already
made. We are not designing apt/npm; those solve problems (system-wide shared
dependencies, root installers, post-hoc trust) that our design deleted.

## 1. Why this is different here — install is not a trust cliff

On every mainstream OS, installing software is the weakest security moment: an
installer runs with full privilege and *could* do anything; app-store permission
prompts are advisory and granted after the code is already trusted to run.

Our OS already inverted this. Every app **declares its authority** up front — its
capabilities (in the EMBX header's capability table) and its namespace (the UP4
`.ns` manifest) — and that authority is **structurally enforced**: capability
attenuation is monotonic, namespace absence is physical (you cannot name what you
were not granted). So the package manager's core job is not copying files; it is
**mediating an authority grant the kernel will then enforce**. Refusing a
capability at install time is a normal, safe outcome — the app simply can't do
that thing, and can't lie about it later.

## 2. The pieces already exist (we are composing, not inventing)

| Concern | Already have |
|---|---|
| Self-contained app layout | `/data/apps/<name>/{<name>.elf, <name>.ns, assets}` — every app is a directory bundle (like the Python bundle) |
| Declared capabilities | EMBX header: `capability_table` + `capability_count`, checked ⊆ grantor at load |
| Declared namespace | UP4 per-app manifest (`<ro\|rw> <prefix>`), granted via `SPAWN_ACTION_NS_BIND` |
| Content identity | EMBX `build_id[32]` = SHA-256, `version_major/minor`, `abi_version`, `header_checksum` (CRC32C) |
| Build tool + manifest | EmbBuild + `build.ebm` (per-unit `inputs`/`args`/`output`, content-stamped rebuild) |
| The ABI (what "targeting EmbLinkOS" means) | sealed `/system/abi` (crt0, syscalls, libc.a, headers) + `/system/lib/libembk.so` |
| On-OS toolchain | EmbCC + EmbLD (emit EMBX via `embld --embx --cap NAME`); tcc; run on the machine itself |
| Verify / rollback / provenance | EMBKFS: content fingerprints, provenance (writer identity), snapshots, verified-boot, kernel crypto (SHA-256/HMAC/AES) |

The package manager and SDK are mostly *assembly + one keystone artifact*, not new
subsystems.

## 3. The package — a bundle + one manifest

A package is the app's bundle directory, archived, plus a **package manifest** that
unifies the declarations scattered across the EMBX header and the `.ns` file into
one human-readable, signable source of truth:

```
name:        notes
version:     1.4.0
abi:         3                      # /system/abi version it targets
build_id:    <sha-256 of the EMBX>  # matches the EMBX header; the content identity
caps:        filesystem             # human-readable mirror of the EMBX cap table
namespace:                          # mirror of the .ns manifest
  ro  /system
  rw  /home/notes
provides:    /data/apps/notes/notes.embx
signature:   <ed25519 over the manifest + build_id>
```

- **The manifest is a *view*, not a second source of truth.** The SDK generates it
  from the build (§4); at install the manager cross-checks it against the EMBX
  header's `build_id` and cap table, so the human-readable declaration can never
  drift from what the binary actually is.
- **No dependency list, by design** (§7). If a `provides` entry needs a system
  library, it names a *sealed, abi-versioned* one under `/system/lib` — never
  another package.
- Bundle contents are content-addressed by `build_id`; the archive is what EMBKFS
  stores, with provenance, on install.

## 4. One source of truth: declaring authority IS part of building

Today an app's authority lives in three places (the EMBX cap table, the `.ns`
file, and — for a human — nowhere). The SDK collapses this: the **build manifest
is the source of truth**, and one build emits all three views.

Extend `build.ebm` with a package stanza:

```
package: notes
version: 1.4.0
caps:    filesystem
grant:   ro /system , rw /home/notes
```

From that single declaration the toolchain (EmbLD) bakes the cap table into the
EMBX, writes the `.ns` file, and stamps the package manifest — all consistent by
construction. **You cannot build an app whose declared authority disagrees with
itself.** That property is the whole point.

### Stage, then adopt (already the EmbBuild boundary)

EmbBuild deliberately has **no install stanza** — the shell's own `build.ebm` says
it: "the build tool STAGES, and ADOPTION is a separate act (snapshot, copy,
reboot) performed by the system." That line *is* the boundary between the SDK and
the package manager:

- **Build/stage** (SDK, EmbBuild): produce a verified-runnable bundle in a staging
  area. Untrusted, no authority granted, no adoption.
- **Adopt/install** (package manager): verify signature + `build_id`, present the
  declared authority, and on consent place the bundle under `/data/apps/<name>/`
  (a snapshot first, so it's atomic and reversible).

The package manager is exactly "adoption, generalized to any bundle."

## 5. Install = an authority-grant negotiation (no root)

`pkg install notes.embpkg` is:

1. **Verify** — signature (kernel crypto) + `build_id` matches the EMBX + `abi`
   matches the running ABI. Reject on mismatch; EMBKFS records provenance.
2. **Present the declared authority** — plainly: *"notes wants — capability
   `filesystem`; namespace: `/home/notes` rw. Nothing else."* No `net`, no other
   home, no `/system` write — because it didn't declare them, so it can't get
   them.
3. **Adopt** — snapshot, unpack the bundle to its app dir, register it. The `.ns`
   the app runs under is exactly the granted namespace (the UP4 mechanism we
   already ship); the caps are exactly the granted cap set (EMBX §6 already
   enforces ⊆ grantor).

**No root, and a smooth privilege gradient instead of a cliff**, because authority
is per-namespace:

- **User install** — a user adopts into their *own* namespace (their home). Needs
  no system authority; another user cannot even name the result. This is the
  natural, common case, and it has no Unix equivalent.
- **System install** — adopting into `/data/apps` (shared, `ro` to apps) or the
  sealed `/system` requires the session/installer to *hold* authority over those
  subtrees. Same act, more authority — not a different, privileged tool.

The grant is **enforced by the kernel, not promised by the installer** — the
categorical difference from an app-store permission prompt.

## 6. Update, rollback, remove — from EMBKFS, nearly free

- **Update** = stage the new bundle, verify, **snapshot, atomic directory swap**.
  Because each app is a self-contained bundle (no shared files to half-replace),
  there is no partially-installed state. (Same self-containment that makes Python
  3.14 and 3.15 coexist — §USERSPACE tree.)
- **Rollback** = an EMBKFS snapshot restore. Free.
- **Remove** = delete the bundle dir + its registry entry + drop the granted
  bindings. Nothing sprawled elsewhere to garbage-collect.
- **Authority changes on update are re-negotiated.** If v2 declares a *new* cap or
  a wider namespace than v1, that is surfaced and consented to (or refused) exactly
  like a first install — a new version cannot silently widen its reach.

## 7. Dependencies: self-contained by default (the biggest simplification)

Our ports are static (Python, git, tcc). We keep that: **packages do not depend on
other packages.** The dependency graph — the source of essentially all
package-manager pain — stays empty.

Shared code that genuinely must be shared (`libembk.so`, the ABI) lives **sealed in
`/system`, versioned by `abi_version`**, not distributed as packages. A package
declares the `abi` it targets; the manager refuses an `abi` mismatch. That is the
*only* "dependency," it is on the OS itself, and it is a single integer check.

If we ever want optional shared runtimes (a Python interpreter shared by several
scripts), the namespace-native answer is a **binding**, not a dependency: grant the
consumer `ro /system/runtimes/python`, don't entangle install graphs.

## 8. The SDK

The SDK is what a developer uses to build *for* EmbLinkOS, and we already have its
parts — they need packaging and the §4 shift (authority is declared at build time):

- **The ABI** — `/system/abi` (crt0, syscalls, `libc.a`, `embk.h`,
  `embk_syscall.h`) + `/system/lib/libembk.so`. Versioned; it *is* the target.
- **The toolchain** — EmbCC + EmbLD (emit EMBX + caps), the UI (`em.h`), sockets
  (`embk_socket.h`).
- **The build tool** — EmbBuild + the extended `build.ebm` (the package stanza).
- **On-OS development** — tcc/EmbCC run on the machine; write, compile, declare,
  and install an app *on the OS itself*, no cross-host needed. (The kernel already
  self-hosts; app development on-device is a smaller ask.)

SDK deliverable = these, bundled and versioned together, with `embbuild` +
`pkg` as the front doors, and templates that scaffold `build.ebm` + a source app
that already declares a minimal namespace/cap set (so declaring authority is the
default, not an afterthought).

## 9. Registry / index — local first, then a git registry

- **Local index first**: the set of installed bundles (name → version → build_id →
  granted authority), queryable — `pkg list`, `pkg info notes`. This is just the
  `/data/apps` tree + a small registry; buildable now, no network.
- **The remote registry is a GIT REPO** (decided 2026-07-29) — e.g. a separate
  `emblink-packages`, distinct from the OS source (registry and OS have different
  lifecycles). This is the proven model (Homebrew taps, the Arch AUR are git
  repos), and it fits our "trust the signature, not the channel" rule exactly.

### 9.1 The index in git, the binaries in releases

Git is excellent at one half of this and bad at the other, so we split them:

- **Index / recipes → git.** The repo holds the signed **package manifests** (§3):
  name, version, `build_id`, declared caps + namespace, signature, and a
  *download URL* for the bytes. Versioned, diffable, auditable; contributions
  arrive as **pull requests** the maintainer reviews and merges. One directory per
  package, versioned manifests inside, plus a top-level index.
- **Binaries → release assets, NOT git history.** Git stores every blob's full
  history with no binary dedup — putting each multi-MB build in it turns the repo
  into a tar-pit. The bundles live as **release/CDN artifacts**, referenced by URL
  from the manifest. (Homebrew's shape exactly: formulae in the tap, bottles in a
  bucket.)

### 9.2 Two independent layers of trust

1. **The index is protected by git's signed commits** — the manifest you merged is
   the manifest the OS reads; the history is tamper-evident.
2. **The binary is protected by our `build_id` signature** — the OS fetches the
   bytes from wherever the manifest points, hashes them, and **rejects anything
   that does not match the signed `build_id`.** A compromised CDN or a hijacked
   release cannot inject a bad binary. Trust is in the maintainer's signature, not
   in GitHub and not in the transport.

The supply-chain story that falls out: anyone proposes a package by PR; the
maintainer signs the release; **the OS trusts the signature, not the host.**

### 9.3 Transport phasing (the TLS caveat)

GitHub is HTTPS-only, so the OS fetching *directly* needs TLS — the same blocker
as pip-from-PyPI (see the networking discussion). This phases the *transport*, not
the design:

- **Now / pre-TLS:** the git repo is the authoritative registry (manifests + signed
  release binaries). The OS syncs its index from a **self-hosted / LAN HTTP mirror**
  of the repo (no TLS), or via host-side download + copy. Everything is still
  `build_id` + signature verified on arrival.
- **After TLS lands:** the OS fetches straight from the repo + releases. And a
  self-consistent option, since we ported git: the OS can `git pull` the registry
  itself to sync the index (http:// from a mirror pre-TLS, https:// after).

Because **we are the source of EmbLinkOS builds** (our ABI; no third-party binary
compat), this registry is *ours* — there is no "download the vendor's `.deb`."
Every artifact is signature + `build_id` verified regardless of transport.

## 10. What's genuinely new to build (short)

Most of the machinery exists; the new work is small and keystone-shaped:

1. **The package manifest format** (§3) — the keystone; everything hangs off it.
2. **The SDK build→declarations generator** (§4) — `build.ebm` package stanza →
   EMBX caps + `.ns` + package manifest, consistent by construction.
3. **`pkg`** — verify + present-authority + adopt/update/remove/rollback (§5–6),
   reusing UP4 NS_BIND, EMBX cap checks, EMBKFS snapshots, kernel crypto.
4. **Signing** — ed25519 (or reuse existing kernel crypto), keys for the OS's own
   builds.
5. **The local registry**, then the mirror (§9).

## 11. Phasing

- **PK1 — the manifest + `pkg install` (local).** Define §3, and a `pkg install`
  that verifies a staged bundle, presents its declared authority, snapshots, and
  adopts it into `/data/apps/<name>/` with exactly its declared namespace. Reuses
  everything shipped through UP4. No network, no signing yet.
- **PK2 — the SDK generator.** `build.ebm` package stanza → EMBX caps + `.ns` +
  manifest, one source of truth (§4). Now apps are *built* declaring their
  authority.
- **PK3 — signing + update/rollback + registry.** ed25519 over `build_id`;
  snapshot-backed atomic update and rollback; `pkg list/info`.
- **PK4 — the git registry.** Publish the index as a git repo (`emblink-packages`)
  with signed manifests + release-asset binaries (§9); the OS syncs the index and
  `pkg install`s from it over HTTP (a mirror pre-TLS, the repo directly once TLS
  lands — rides the net stack). Signature + `build_id` verified regardless of
  transport.

## 12. What we deliberately do NOT do

- **No dependency resolution.** Packages don't depend on packages; the only
  "dependency" is a single `abi` integer against the OS.
- **No root installer / no privileged install cliff.** Install is authority-scoped;
  user installs need no system authority.
- **No post-hoc permission prompts.** Authority is declared before the code runs
  and enforced by the kernel — not requested by trusted-already code at runtime.
- **No third-party binary ABI.** An EmbLinkOS package targets our ABI; there is no
  "download the vendor's `.deb`." We produce the builds; trust is by signature.
- **No shared-file sprawl.** Every package is a self-contained bundle directory;
  nothing lands in a communal `/usr/lib` to conflict.
