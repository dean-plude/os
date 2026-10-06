#!/usr/bin/env python3
"""Check that the runner can open KVM and create a VM, not just stat its device."""
import fcntl
import os


def probe(path='/dev/kvm'):
    try:
        with open(path, 'r+b', buffering=0) as device:
            if fcntl.ioctl(device.fileno(), 0xAE00, 0) != 12:  # KVM_GET_API_VERSION
                return False
            vm = fcntl.ioctl(device.fileno(), 0xAE01, 0)  # KVM_CREATE_VM
            os.close(vm)
        return True
    except OSError:
        return False


if __name__ == '__main__':
    raise SystemExit(0 if probe() else 1)
