#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
build_dir=$(mktemp -d /tmp/emblink-driver-tests.XXXXXX)
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM
compile_and_run() {
    name=$1
    shift
    "${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror ${TEST_CFLAGS:-} -Ikernel "$@" -o "$build_dir/$name"
    "$build_dir/$name"
}
compile_and_run ec tests/drivers/ec_protocol_test.c kernel/drivers/platform/ec_protocol.c
compile_and_run designware tests/drivers/designware_test.c kernel/drivers/i2c/designware_protocol.c
compile_and_run hid tests/drivers/hid_report_test.c kernel/drivers/input/hid_report.c
compile_and_run i2chid tests/drivers/i2c_hid_test.c kernel/drivers/i2c/hid_protocol.c
compile_and_run acpi-i2c tests/drivers/acpi_i2c_test.c
compile_and_run wpa2 tests/drivers/wpa2_crypto_test.c kernel/net/wifi/wpa2_crypto.c kernel/crypto/sha1.c kernel/crypto/aes.c
compile_and_run wifi-security tests/drivers/wifi_security_test.c kernel/net/wifi/wpa2.c kernel/net/wifi/ccmp.c kernel/net/wifi/wpa2_crypto.c kernel/crypto/sha1.c kernel/crypto/aes.c
compile_and_run wifi-station tests/drivers/wifi_station_test.c kernel/net/wifi/station.c kernel/net/wifi/wpa2.c kernel/net/wifi/ccmp.c kernel/net/wifi/wpa2_crypto.c kernel/crypto/sha1.c kernel/crypto/aes.c
compile_and_run ax200 tests/drivers/ax200_transport_test.c kernel/net/wifi/ax200_protocol.c kernel/net/wifi/station.c kernel/net/wifi/wpa2.c kernel/net/wifi/ccmp.c kernel/net/wifi/wpa2_crypto.c kernel/crypto/sha1.c kernel/crypto/sha256.c kernel/crypto/aes.c
compile_and_run wifi-inputs tests/drivers/wifi_profile_test.c kernel/net/wifi/profile.c kernel/net/wifi/ax200_protocol.c
compile_and_run dhcp tests/drivers/dhcp_test.c kernel/net/dhcp/dhcp.c
compile_and_run udp-capture tests/drivers/udp_capture_test.c kernel/net/udp/udp.c
compile_and_run dns tests/drivers/dns_test.c kernel/net/dns/dns.c

compile_and_run wifi-control tests/drivers/wifi_control_test.c kernel/net/wifi/control.c kernel/net/wifi/wpa2_crypto.c kernel/crypto/sha1.c kernel/crypto/aes.c
compile_and_run audio-sink tests/drivers/audio_sink_test.c kernel/drivers/audio/audio.c
compile_and_run hda tests/drivers/hda_transport_test.c
compile_and_run wifi-service tests/drivers/wifi_service_test.c kernel/net/wifi/control.c kernel/net/wifi/profile.c kernel/net/wifi/station.c kernel/net/wifi/wpa2.c kernel/net/wifi/ccmp.c kernel/net/wifi/wpa2_crypto.c kernel/crypto/sha1.c kernel/crypto/aes.c
compile_and_run console-argv tests/drivers/console_argv_test.c

compile_and_run installer tests/drivers/install_test.c kernel/block/install_protocol.c kernel/update/protocol.c -lz
compile_and_run session tests/drivers/session_test.c
compile_and_run authentication tests/drivers/auth_test.c
compile_and_run fileops -DEMBK_FILEOPS_HOST tests/drivers/fileops_test.c user/lib/fileops.c -Wl,--wrap=read,--wrap=write,--wrap=fsync
compile_and_run intel-display tests/drivers/intel_display_test.c
compile_and_run intel-capture -DINTEL_CAPTURE_HOST -Itests/drivers tests/drivers/intel_capture_test.c kernel/drivers/video/intel_dpaux.c kernel/drivers/video/intel_gmbus.c kernel/drivers/video/intel_capture.c
INTEL_CAPTURE_OUT="$build_dir/capture.bin" "$build_dir/intel-capture" >/dev/null
python3 tests/drivers/intel_capture_decode_test.py "$build_dir/capture.bin"
compile_and_run intel-vbt -Itests/drivers tests/drivers/intel_vbt_test.c kernel/drivers/video/intel_vbt.c kernel/display/edid_modes.c
compile_and_run display-modes -Itests/drivers tests/drivers/display_modes_test.c kernel/display/edid_modes.c
compile_and_run intel-config tests/drivers/intel_config_test.c kernel/drivers/video/intel_config.c kernel/display/edid_modes.c
compile_and_run intel-hw -Itests/drivers tests/drivers/intel_hw_test.c kernel/drivers/video/intel_hw.c kernel/drivers/video/intel_config.c kernel/drivers/video/intel_dpaux.c kernel/display/edid_modes.c
compile_and_run intel-native -Itests/drivers tests/drivers/intel_native_test.c kernel/drivers/video/intel_native.c kernel/drivers/video/intel_hw.c kernel/drivers/video/intel_config.c kernel/drivers/video/intel_dpaux.c kernel/drivers/video/intel_gmbus.c kernel/drivers/video/intel_vbt.c kernel/display/edid_modes.c kernel/display/edid.c
compile_and_run cpu-power tests/drivers/cpu_power_test.c
compile_and_run power-policy tests/drivers/policy_test.c
compile_and_run notifications tests/drivers/notify_test.c
compile_and_run display-mode tests/drivers/display_mode_test.c
compile_and_run display-edid tests/drivers/display_edid_test.c kernel/display/edid.c
compile_and_run display-control tests/drivers/display_control_test.c kernel/display/edid.c kernel/display/layout.c
compile_and_run display-layout tests/drivers/display_layout_test.c kernel/display/layout.c
compile_and_run display-arrange -Iuser/lib -iquote kernel tests/desktop/display_arrange_test.c user/lib/display_arrange.c kernel/display/layout.c
compile_and_run live-boot tests/drivers/live_boot_test.c
compile_and_run recovery-check tests/drivers/recovery_check_test.c
compile_and_run session-profile -Iuser/lib tests/drivers/session_profile_test.c
