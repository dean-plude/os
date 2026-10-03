# the network suite's e1000e boot: an Intel 82574L (QEMU's e1000e, the
# family of the I219 that PCs have built in), whose driver reads the PHY
# over MDIO and has it auto-negotiate; the link is pulled and plugged back
# (QEMU's set_link), the machine sleeps (S3) and wakes, the adapter is set
# up again, and then the IPv4 tests run again (tools/selftest.py)
import re
import time


def link(up, wait):
    def act(nova):
        nova.hmp(f'set_link nic0 {"on" if up else "off"}')
        time.sleep(wait)
    return act


def address_back(marker, timeout=90):
    """retry `ipconfig` until the address is back after `marker` (the link
    takes 5 to 10 s to come up and DHCP then renews; both take longer under
    CI load), so a slow boot passes and only a real failure times out"""
    def act(nova):
        deadline = time.time() + timeout
        while time.time() < deadline:
            log = open(nova.serial_path, 'rb').read().decode('latin-1')
            if '[E1000] Link up' in log[log.rfind(marker):]:
                out, _ = nova.run('ipconfig', 15)
                if re.search(r'IPv4 Address[ .]*: 10\.0\.2\.15', out):
                    return
            time.sleep(1)
    return act


def plug(nova):
    nova.hmp('set_link nic0 on')
    address_back('[E1000] Link down')(nova)


def wake(nova):
    """sleeptest went to sleep: wake the machine once it is suspended"""
    for _ in range(240):
        if nova.status() == 'suspended':
            break
        time.sleep(0.25)
    time.sleep(2)
    nova.qmp.cmd('system_wakeup')


TESTS = [
    Test('e1000e', 'ipconfig', [r'Intel 82574L', r'IPv4 Address[ .]*: 10\.0\.2\.15'], builtin=True, before=address_back('[E1000] Intel 82574L'),
         boot_expect=[r'\[E1000\] Intel 82574L [^\n]* at [0-9a-f:.]+, MAC [0-9a-f:]+',
                      r'\[E1000\] PHY [0-9a-f]{4}:[0-9a-f]{4}, auto-negotiating',
                      r'\[E1000\] Link up at 1000 Mb/s, full duplex']),
    Test('e1000e link down', 'ipconfig', [r'Media disconnected'], builtin=True, before=link(False, 3),
         boot_expect=[r'\[E1000\] Link down']),
    Test('e1000e link up', 'ipconfig', [r'IPv4 Address[ .]*: 10\.0\.2\.15'], builtin=True, before=plug,
         boot_expect=[r'\[E1000\] Link down[\s\S]*\[E1000\] Link up at 1000 Mb/s']),
    Test('ping e1000e', 'ping 10.0.2.2', [r'Received = 4, Lost = 0'], builtin=True),
    Test('sleep e1000e', 'sleeptest', [r'Awake at', r'PASS'], acts=[(r'\[SHELL\] Sleeping', wake)], timeout=240),
    Test('ping e1000e after sleep', 'ping 10.0.2.2', [r'Received = 4, Lost = 0'], builtin=True,
         before=address_back('[SLEEP] Woke up'), boot_expect=[r'\[SLEEP\] Woke up[\s\S]*\[E1000\] Link up at 1000 Mb/s']),
]
