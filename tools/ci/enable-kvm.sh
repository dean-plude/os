#!/usr/bin/env bash
# Default requires working KVM; optional is used by bounded PR OS suites.
set -euo pipefail
mode=${1:-required}
case "$mode" in required|optional) ;; *) echo "invalid KVM mode: $mode" >&2; exit 2 ;; esac
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
if [ -e /dev/kvm ]; then
    echo 'KERNEL=="kvm", GROUP="kvm", MODE="0666", OPTIONS+="static_node=kvm"' |
        sudo tee /etc/udev/rules.d/99-kvm4all.rules >/dev/null
    sudo udevadm control --reload-rules
    sudo udevadm trigger --name-match=kvm
    sudo udevadm settle --timeout=10
    sudo chmod a+rw /dev/kvm
fi
if python3 "$root/tools/ci/probe_kvm.py"; then
    accel=kvm
elif [ "$mode" = optional ] || [ "${NOVARUN_ALLOW_TCG:-false}" = true ]; then
    accel=tcg
    echo '::warning::KVM probe failed; running every selected test under TCG.'
else
    echo '::error::KVM cannot create a VM on this runner. Re-run this job or dispatch with allow_tcg.'
    echo '### Infrastructure failure: KVM unavailable' >> "${GITHUB_STEP_SUMMARY:-/dev/null}"
    exit 1
fi
printf 'NOVARUN_ACCEL=%s\n' "$accel" >> "${GITHUB_ENV:-/dev/null}"
printf '**Test VMs use %s.**\n' "$accel" >> "${GITHUB_STEP_SUMMARY:-/dev/null}"
