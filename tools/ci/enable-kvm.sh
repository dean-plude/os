#!/bin/sh
# Opens /dev/kvm to the runner user and exports NOVARUN_ACCEL for the later
# steps: kvm when the device is usable, tcg (with a warning) when it is not.
# tools/novarun.py reads NOVARUN_ACCEL; a test is never skipped either way.
if [ -e /dev/kvm ]; then
    echo 'KERNEL=="kvm", GROUP="kvm", MODE="0666", OPTIONS+="static_node=kvm"' |
        sudo tee /etc/udev/rules.d/99-kvm4all.rules >/dev/null
    sudo udevadm control --reload-rules
    sudo udevadm trigger --name-match=kvm
fi
if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
    accel=kvm
else
    accel=tcg
    echo "::warning::/dev/kvm is not usable on this runner; the test VMs run under TCG"
fi
echo "NOVARUN_ACCEL=$accel" >> "${GITHUB_ENV:-/dev/null}"
echo "Test VMs run under $accel"
