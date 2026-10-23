#!/usr/bin/env python3
"""Compatibility entry point for current native desktop/session acceptance.

Use tools/desktop_smoke.py for the UEFI account, controls, lock and reboot test.
The earlier session controller and its GDB layouts were replaced by master.
"""
from desktop_smoke import main

if __name__ == '__main__':
    main()
