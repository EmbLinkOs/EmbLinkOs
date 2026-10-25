AX200 firmware, API 77, unmodified binary from the Linux firmware repository:

https://gitlab.com/kernel-firmware/linux-firmware/-/raw/main/intel/iwlwifi/iwlwifi-cc-a0-77.ucode

Size: 1,368,100 bytes.
SHA-256: `1866a58a1daee56b120362122d5f7548b443b53d33f2558a7e4407d58fd27dab`.

Redistribution terms are in `LICENCE.iwlwifi_firmware`, obtained from
https://gitlab.com/kernel-firmware/linux-firmware/-/raw/main/LICENSES/LICENCE.iwlwifi_firmware.
The license and binary are both included in boot images. The driver validates
this exact digest before giving the firmware to the device. A firmware/API
upgrade requires updating the transport and its validation, not renaming a blob.
