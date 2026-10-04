## Two test races fixed: qtthemetest and prioritytest net

Pull request #206 went red on two self-tests its changes do not touch.

- **qtthemetest** (from #203): the UISettings watcher thread holds its own
  reference on the colour-change handler while it calls it, and the handler
  sets its event before that call returns.  The test removed the handler as
  soon as it saw the event, so on a slow run it counted the watcher's
  reference and `h.refs == 1` failed.  The test now waits until the watcher
  has let go (`h.refs == 2`) before removing the handler.
- **prioritytest net**: the slowest loopback round trip was 250.084 ms
  against a 250 ms limit on a hosted runner without KVM (the boot test falls
  back to QEMU's TCG there, #202).  The test now reads the hypervisor CPUID
  leaf: under TCG ("TCGTCGTCGTCG") the limit is 500 ms, under KVM and on
  real hardware it stays 250 ms.  A starved network thread (3 s before the
  kernel bands) still fails either way.
