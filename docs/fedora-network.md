# MiniPC REAC transport with standard Fedora networking

The OpenWrt `reac-transport` package is just declarative netifd/firewall4 config
for VLANs + a gretap tunnel. On the Fedora MiniPC you don't need it — the same
fabric is native, with one REAC-specific nuance:

> **REAC is raw Layer-2 (EtherType 0x8819), no IP.** The interface our daemons
> capture on (`REAC_IFACE`) must be **up but address-less** — give it no IPv4/IPv6.
> The bridge / reac-pw open an AF_PACKET socket on it; addressing is irrelevant
> and only adds noise.

Pick whichever your image uses. **systemd-networkd** suits a headless appliance
(declarative, reproducible — closest to the `reac-transport` philosophy);
**NetworkManager** is the Fedora default and works equally well.

## A. systemd-networkd (recommended for an appliance)

REAC zone A = VLAN 11 on `enp1s0`, presented as `reac0`. The parent keeps normal
DHCP for management; `reac0` is L2-only.

`/etc/systemd/network/10-mgmt.network` — parent NIC, normal management IP + it
hosts the VLAN:
```ini
[Match]
Name=enp1s0
[Network]
DHCP=yes
VLAN=reac0
```

`/etc/systemd/network/20-reac0.netdev` — the VLAN device:
```ini
[NetDev]
Name=reac0
Kind=vlan
[VLAN]
Id=11
```

`/etc/systemd/network/20-reac0.network` — bring it up, **no addressing**:
```ini
[Match]
Name=reac0
[Network]
LinkLocalAddressing=no
[Link]
RequiredForOnline=no
```
Apply: `systemctl enable --now systemd-networkd && networkctl reload`, then
`networkctl status reac0` should show it `carrier`/`up` with no IP. Point the
service config at it: `REAC_IFACE=reac0` in `/etc/reac-pw/reac-pw.conf` (or
`reac-aes67.conf`).

If the desk is on an **untagged** segment, skip the VLAN entirely and set
`REAC_IFACE=enp1s0` (still leave that NIC address-less if it carries only REAC).

## B. NetworkManager (nmcli — the Fedora default)

```
nmcli con add type vlan con-name reac0 dev enp1s0 id 11 ipv4.method disabled ipv6.method disabled connection.autoconnect yes
nmcli con up reac0
```
`ipv4.method disabled` + `ipv6.method disabled` = the L2-only, no-address state.
Keyfile lands in `/etc/NetworkManager/system-connections/reac0.nmconnection`.

## C. gretap tunnel (only if the MiniPC must cross an IP underlay)

Usually the router already tunnels REAC and the MiniPC just plugs into the
segment — but if the PC itself must carry REAC over L3/Wi-Fi, that's also native:

`/etc/systemd/network/30-reactap.netdev`:
```ini
[NetDev]
Name=reactap
Kind=gretap
[Tunnel]
Local=<minipc-ip>
Remote=<peer-ip>
```
plus a matching `30-reactap.network` (`LinkLocalAddressing=no`), then
`REAC_IFACE=reactap`. VLAN-tag the tunnel by stacking a `.netdev` VLAN on
`reactap` exactly as in section A.

## Why this is enough

`reac-transport` exists because OpenWrt's netifd/fw4 is the only config surface
there. Fedora already has two first-class ones; the daemons don't care how the
interface came to exist, only that `REAC_IFACE` is up and carries 0x8819. So the
MiniPC needs **no transport package** — just one of the snippets above.
