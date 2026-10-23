# Wi-Fi: AX200 baseline

The experimental driver targets the discrete PCIe Intel AX200, PCI ID
`8086:2723`. It loads the bundled, unmodified `iwlwifi-cc-a0-77.ucode`, validates
its pinned SHA-256 and TLV/API layouts, starts the device through context-info
DMA, and reads its NVM/MAC and current regulatory profile. Both the firmware
license and the iwx source notices are included in boot images.
DMA buffers use a permanent contiguous physical allocation made only when an
AX200 is present; they do not enlarge the fixed-address kernel image. The native
`test dma` command checks contiguous/aligned allocations and page ownership.

The driver implements polling command/RX/TX queues, passive scanning, PHY/MAC
contexts, AP station and transmit-queue configuration, and association. CCMP
runs in software; firmware decryption is disabled and received ciphertext is
authenticated before it reaches the Ethernet/IP stack. A successful WPA2
handshake opens the controlled port. A separate worker obtains and refreshes
the DHCP lease while the normal RX worker continues receiving packets. Failed
DHCP does not invent a VM address or gateway for Wi-Fi. Disconnect/reconnect
clears the address and ARP cache; failed connections retry after five seconds.

## Connect from the desktop

Open **Settings → Wi-Fi** on an AX200 machine. Choose a nearby WPA2 Personal
network, enter its password, and select **Connect & save**. Status separates
scanning, association, the security handshake, DHCP and an acquired address.
The top bar shows live connection status and opens Settings.

Up to eight networks are remembered. **Join** selects a saved network as the
boot preference; **Forget** removes its key, disconnecting if it is the current
preference. **Disconnect** pauses automatic connection for this boot. Scanning
other channels requires disconnection. The nearby list describes the last
passive scan; it does not invent signal-strength readings. Unsupported security
modes are visible without offering to join them. Hidden SSIDs remain unsupported.

Changes require `network_admin` (ID 14), separate from ordinary socket access.
The worker derives the PMK outside the network lock and wipes passphrases.
The read API returns SSIDs and public status, never keys. The bounded, versioned
`/data/system/wifi.profiles` store holds PMKs with mode 0600 outside the desktop
session namespace. This is namespace isolation, not an encrypted key vault.
Writes flush a temporary file, atomically replace the store, and flush again;
storage errors are returned through asynchronous completion status.

## Optional image-builder profile


On the build host, run:

```sh
python3 tools/configure_wifi.py --ssid 'Your network'
make images uefi-usb.img
```

The tool prompts for the WPA2 passphrase without echo, derives the PMK, and
writes `system/config/wifi.conf` with mode 0600. The local file is ignored by
Git. The image carries it at `/system/config/wifi.conf`, also with mode 0600.
It contains a usable network key, so keep configured images private.
`--ssid-hex` supports SSIDs containing arbitrary bytes. The passphrase accepts
8–63 printable ASCII characters or a 64-digit hexadecimal PSK. No credentials
are supplied in the repository's default image.

Native kernel-console commands:

```text
wifi status
wifi reconnect
wifi disconnect
wifi scan
```

`reconnect` joins the preferred saved network. A builder profile is imported
only if no runtime store exists; an intentionally empty store after forgetting
all networks suppresses import. No passphrase is passed through or echoed by
the console. The existing Ethernet/USB selection runs first. AX200 is selected only when
no earlier supported interface is ready; one interface remains active at a time.
See [LAPTOP_DESKTOP.md](LAPTOP_DESKTOP.md) for the Settings/service ABI.

## Supported mode and remaining limits

- Broadcast SSID, infrastructure AP, WPA2-Personal with CCMP-128 and PSK AKM.
- Legacy 20-MHz operation on firmware-permitted 2.4/5-GHz channels; transmit
  starts at the lowest basic rate. This favors initial compatibility and is
  not a high-throughput implementation. DFS/radar channels are refused.
- Optional PMF on an AP is usable with PMF disabled for this station. Required
  PMF, WPA3/SAE, enterprise EAP, WEP/TKIP, hidden SSIDs, 6 GHz, fragmentation,
  A-MSDU/HT/VHT aggregation, roaming and suspend/resume are not implemented.
- Initial four-way authentication and GTK rekey are implemented. An AP-initiated
  pairwise-key rekey is not negotiated in place; an AP disconnect is followed
  by a fresh association. Keys and packet numbers are never reset by a retry
  of an already authenticated handshake message or GTK installation.
- AX201 CNVio, AX210 and other vendors need their own transport support.
  An AX200 timeout/fatal firmware error stops DMA and disables PCI bus mastering;
  recovering that transport currently requires rebooting.

## Validation and physical acceptance

```sh
TEST_CFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' tests/drivers/run.sh
```

Host tests run production code with independent Python-derived PMK/PTK/EAPOL,
AES-wrap and AES-CCM vectors. They cover corrupted frames, replay, retransmitted
M1/M3, GTK changes/index changes, failed installation/TX, closed-port filtering,
timeouts and malformed IE/EAPOL/firmware/DMA input. The adapter model executes
the real AX200 driver and station against the bundled firmware's API table,
including context DMA, NVM/regulatory commands, a scripted AP association,
encrypted Ethernet, 1,100 TX with RX/TX queue wrapping, teardown and timeout
fail-stop. It does not execute the firmware on a radio or validate RF behavior.

Service tests additionally cover request serialization, secret wiping, saved-network
reload/selection/forget, binary SSIDs, malformed discovery frames and failed
pre-commit flushes preserving the old profile store.

Physical acceptance remains open. On an AX200 laptop, boot a configured UEFI
image near a non-DFS WPA2 AP and record:

1. `wifi status` reaches `authorized`, with a DHCP address and resolver.
2. `test dhcp`, `test dns` and `test tcp` succeed; then load a page in the browser.
3. Disconnect/reconnect and AP restart recover without accepting old encrypted
   packets; idle operation lasts beyond a lease renewal and a GTK rotation.
4. Hardware radio-block/unblock, poor signal and shutdown do not hang the kernel.

Existing QEMU Ethernet DHCP/DNS/HTTP checks exercise the shared network stack.
They are not physical Wi-Fi acceptance, and the driver remains experimental
until those laptop checks pass.

For deterministic shared-stack validation without an upstream resolver, run
`EMBLINK_CPUS=2 python3 tools/network_smoke.py` with host `cc`, QEMU and curl on
PATH. It uses loopback UDP port 53 and a temporary HTTP server; the test-only
host interposer is never included in the OS. The guest resolves `example.com`
through its DHCP-provided resolver and fetches the fixture page over TCP.
`test netservices` separately checks that concurrent kernel service requests
retain capture ownership while yielding the recursive network monitor to RX.
