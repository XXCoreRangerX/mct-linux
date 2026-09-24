# Early live probes (before the Windows capture)

First contact with the T2-285A over raw bulk endpoints (pyusb). They
established the endpoint pairing rules and produced the files in `data/`.

| Script | What it does | Output / finding |
|---|---|---|
| `probe1.py` | enumerate `0711:520d`, dump descriptors | 6 bulk EPs, no kernel driver |
| `probe2.py` | `0x1d` device-info + `0x1b` EDID queries | `data/t2_1d_info.bin` (EDID at +0x80 → `data/panel_edid.bin`) |
| `probe3.py` | `0x91` ID-ROM reads | ID-ROM layout (`03 02 00 00 …`) |
| `probe4.py` | minimal `1b`/`1d` handshake | OUT/IN pairing is mandatory, else EP3 stalls |
| `probe5.py` | `0x05` register read sweep | `data/t2_regsweep.txt` (power-on register values) |
| `probe6.py` | EP `0x85` poll + standard descriptors | EP `0x85`/`0x06` unused |
| `probe8.py` | register read/write round-trips, `0x0a` clear | `0x02` write / `0x05` read semantics, `feb0`/`fc28` behaviour |

Run with the tablet's video function accessible (`chmod 666` on its
`/dev/bus/usb` node or the NixOS module's udev rule). Probes 7 and 9 were
early `0x11` fill attempts that didn't display (see `docs/first-light.md` for why),
so they aren't included.
