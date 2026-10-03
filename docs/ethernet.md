# Intel Ethernet: e1000, e1000e and the I219

NovaOS has one driver for Intel's gigabit Ethernet family,
`kernel/drivers/e1000.c`.  It takes, in this order of preference:

| Adapter | PCI IDs (vendor 8086) | Where you meet it |
|---|---|---|
| **I219-LM and I219-V** ("Ethernet Connection (N) I219"), the MAC built into Intel chipsets since 2015 | 55 IDs, below | the wired port of most Intel laptops and desktops, the [reference machine](hardware.md) among them |
| **82574L** | `10d3` | QEMU's `e1000e` (q35's default NIC) |
| **82540EM, 82544, 82545EM** | `100e`, `100f`, `1004`, `100c`, `1015` | QEMU's `e1000`, VirtualBox, VMware |

The Terminal's `devices` lists the adapter with the driver `e1000e`
(the 82574L and the I219) or `e1000`, and `ipconfig` names it: the I219
by the name Windows gives it, such as "Intel Ethernet Connection (16)
I219-LM".

## The I219 IDs

Each ID belongs to one chipset (PCH) generation, which decides the errata
the driver applies.  The ThinkPad T14 Gen 4 (Intel) has a Raptor Lake-P
chipset, so its I219 is one of the Alder Lake / Raptor Lake IDs; which one
shows in `devices` on the machine.

| Chipset | I219-LM (vPro) | I219-V |
|---|---|---|
| Sunrise Point, Lewisburg, Comet Lake-V (6th to 10th generation Core) | `156f`, `15b7`, `15b9`, `15d7`, `15e3`, `0d53` | `1570`, `15b8`, `15d8`, `15d6`, `0d55` |
| Cannon Point, Ice Lake, Comet Lake | `15bd`, `15bb`, `15df`, `15e1`, `0d4e`, `0d4c` | `15be`, `15bc`, `15e0`, `15e2`, `0d4f`, `0d4d` |
| Tiger Lake (11th) | `15fb`, `15f9`, `15f4` | `15fc`, `15fa`, `15f5` |
| Alder Lake, Raptor Lake (12th to 14th) | `1a1e`, `1a1c`, `550c`, `0dc7`, `0dc5` | `1a1f`, `1a1d`, `550d`, `0dc8`, `0dc6` |
| Meteor Lake, Lunar Lake (Core Ultra 1 and 2) | `550a`, `550e`, `5510` | `550b`, `550f`, `5511` |
| Arrow Lake, Panther Lake | `57a0`, `57b3`, `57b5`, `57b7` | `57a1`, `57b4`, `57b6`, `57b8` |
| Nova Lake | `57b9` | `57ba` |

## How the I219 differs, and what the driver does

The I219 shares the 82574L's descriptor rings and most registers, which
is why QEMU's 82574L can test the common path.  What it adds is the PHY:
a separate chip that the MAC reaches over MDIO, or over SMBus while the
PHY sleeps, and that the chipset's management engine (ME, on vPro
models running AMT) shares.  The driver brings it up the way Intel's own
BSD-licensed driver code (FreeBSD's `e1000_ich8lan.c`) does:

1. Puts the PCI function in D0, if the firmware left it in D3.
2. Leaves **Ultra Low Power** mode, in which Windows' driver or the
   firmware may have left the PHY: asks the ME firmware to do it when
   there is one, otherwise power-cycles the PHY (the LANPHYPC pin) and
   clears its ULP and SMBus settings.
3. Checks that the PHY answers on MDIO (its ID registers), first as is,
   then with the MAC in SMBus mode, then after a LANPHYPC power cycle,
   unless the ME firmware blocks PHY resets.
4. Resets the MAC and the PHY together, holding the software semaphore
   the ME and hardware also use, and waits for the chipset to load the
   configuration (and the MAC address) from its NVM.
5. Sets the bits each chipset generation needs (descriptor write-back,
   transmit arbitration, ECC, the Sunrise Point errata, the DMA clock on
   Tiger Lake and later, the K1 exit timeout on Meteor Lake and later),
   then has the PHY auto-negotiate at 10, 100 and 1000 Mb/s.
6. When the link comes up, sets what the speed needs (the inter-packet
   gap, the PLL clock gate, K1 at gigabit speed, the transmit pointer
   gap), and logs `[E1000] Link up at 1000 Mb/s, full duplex`.

After sleep (S3) it does all of this again.  Every step that can fail
logs what happened, so the boot log of a machine whose port stays dark
shows how far it got.  On a working machine it should read like this
(the device ID, MAC and PHY revision differ):

```
[E1000] I219: ME firmware present, PHY answering
[E1000] Intel Ethernet Connection (16) I219-LM at 00:1f.6, MAC 00:11:22:33:44:55
[E1000] Device 1a1e, chipset Alder Lake / Raptor Lake
[E1000] PHY 0154:00a2, auto-negotiating
[E1000] Link up at 1000 Mb/s, full duplex
```

Not done yet: configuring the PHY from the NVM's extended configuration
region (the driver logs `The NVM asks the driver to configure the PHY`
when a machine wants it), energy-efficient Ethernet, the power-saving
latency settings (LTR, OBFF), Wake-on-LAN, jumbo frames, checksum and
segmentation offload, and interrupts (the network thread polls, as for
every adapter).

## Testing it

QEMU has no I219, so the self-tests run the shared path on the 82574L:
the network suite's third boot (`tests/selftest/network-e1000e`) checks
the PHY's ID and auto-negotiation in the boot log, pulls the link and
plugs it back (QEMU's `set_link`), sleeps and wakes the machine, and runs
the IPv4 tests (`ping`, Winsock, winhttp's HTTP/2, `looptest`) on it:

```bash
python3 tools/selftest.py --suite network
```

The I219's own steps run only on a real machine.  To check one, start
NovaOS on it with an Ethernet cable plugged into a network with DHCP,
open the Terminal and run:

```
devices
ipconfig
ping 1.1.1.1
curl https://example.com/
```

`devices` should show the I219 with the driver `e1000e`, `ipconfig` an
IPv4 address, and `ping` and `curl` answers.  If not, the `[E1000]` lines
of the boot log say which step stopped.
