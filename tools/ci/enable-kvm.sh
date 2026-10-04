#!/bin/sh
# Opens /dev/kvm to the runner user and exports NOVARUN_ACCEL for the later
# steps, and says which accelerator the test VMs use in the job summary.
# Usage: enable-kvm.sh [required|optional]   (default required)
#   required: a runner without a usable /dev/kvm fails the job here with a
#             clear message, unless NOVARUN_ALLOW_TCG=true (the workflow_dispatch
#             input allow_tcg) asks for a deliberate TCG run.
#   optional: such a runner falls back to TCG with a warning.
# tools/novarun.py reads NOVARUN_ACCEL; a test is never skipped either way.
mode=${1:-required}
if [ -e /dev/kvm ]; then
    echo 'KERNEL=="kvm", GROUP="kvm", MODE="0666", OPTIONS+="static_node=kvm"' |
        sudo tee /etc/udev/rules.d/99-kvm4all.rules >/dev/null
    sudo udevadm control --reload-rules
    sudo udevadm trigger --name-match=kvm
fi
if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
    accel=kvm
    note="KVM"
else
    accel=tcg
    why="/dev/kvm is not usable on this runner ($(ls -l /dev/kvm 2>&1 | head -1); image $ImageOS $ImageVersion)"
    if [ "$mode" = required ] && [ "$NOVARUN_ALLOW_TCG" != true ]; then
        echo "::error::$why, but this job expects KVM: its timings and the KVM-only fixes it checks do not hold under TCG. Re-run the job (a different runner usually has KVM) or start the workflow by hand with allow_tcg set for a deliberate TCG run."
        echo "### Test VMs: no KVM, job stopped" >> "${GITHUB_STEP_SUMMARY:-/dev/null}"
        echo "$why" >> "${GITHUB_STEP_SUMMARY:-/dev/null}"
        exit 1
    fi
    echo "::warning::$why; the test VMs run under TCG"
    note="TCG (software emulation)"
fi
echo "NOVARUN_ACCEL=$accel" >> "${GITHUB_ENV:-/dev/null}"
echo "Test VMs run under $accel"
echo "**Test VMs run under $note.**" >> "${GITHUB_STEP_SUMMARY:-/dev/null}"
