#!/usr/bin/env python3
"""Offline parser for tee.py logs (usbredir protocol).
Reassembles both directions, decodes packets, dumps per-endpoint bulk streams
and a human-readable command timeline.
Usage: parse_redir.py redir.bin [--timeline-only]
"""
import struct, sys

# packet types (usbredirproto.h)
TYPES = {
    0: "hello", 1: "device_connect", 2: "device_disconnect", 3: "reset",
    4: "interface_info", 5: "ep_info", 6: "configuration_status",
    7: "set_configuration", 8: "get_configuration", 9: "configuration",
    10: "set_alt_setting", 11: "get_alt_setting", 12: "alt_setting",
    13: "start_iso_stream", 14: "stop_iso_stream", 15: "iso_stream_status",
    16: "start_interrupt_receiving", 17: "stop_interrupt_receiving",
    18: "interrupt_receiving_status", 19: "alloc_bulk_streams",
    20: "free_bulk_streams", 21: "bulk_streams_status",
    22: "cancel_data_packet", 23: "filter_reject", 24: "filter", 25: "status",
    26: "control_packet", 27: "bulk_packet", 28: "iso_packet",
    29: "interrupt_packet", 30: "buffered_bulk_packet",
    31: "buffered_bulk_packet_no_data", 32: "iso_stream_desc",
}


def read_log(path):
    f = open(path, "rb")
    recs = []
    while True:
        h = f.read(13)
        if len(h) < 13:
            break
        usec, direction, ln = struct.unpack("<Q B I", h)
        recs.append((usec, direction, f.read(ln)))
    return recs


def reassemble(recs):
    bufs = {0: b"", 1: b""}
    for _, d, chunk in recs:
        bufs[d] += chunk
    return bufs


def parse_stream(buf):
    """Yield (type, id, header_bytes, data). Handles 32-bit ids; upgrades
    to 64-bit after hello with version>=? (detect: hello has version str)."""
    # hello (type 0) always uses a 12-byte header (32-bit id); everything
    # else uses 16-byte headers (64-bit ids, caps bitmask 0xFF negotiated)
    off = 0
    pkts = []
    while True:
        if off + 12 > len(buf):
            break
        typ, length = struct.unpack("<II", buf[off:off + 8])
        idsize = 4 if typ == 0 else 8
        if off + 8 + idsize > len(buf):
            break
        (pid,) = struct.unpack("<I" if idsize == 4 else "<Q",
                               buf[off + 8:off + 8 + idsize])
        total = 8 + idsize + length
        if off + total > len(buf):
            break  # truncated (log cut mid-packet)
        pkts.append((typ, pid, buf[off + 8 + idsize:off + total]))
        off += total
    return pkts, off


def main():
    path = sys.argv[1]
    tl_only = "--timeline-only" in sys.argv
    recs = read_log(path)
    print(f"{len(recs)} tee records")
    bufs = reassemble(recs)
    for d in (0, 1):
        pkts, used = parse_stream(bufs[d])
        print(f"dir{d}: {len(pkts)} packets, {used}/{len(bufs[d])} bytes parsed")
        n_bulk = sum(1 for t, _, _ in pkts if t in (27, 30))
        n_ctrl = sum(1 for t, _, _ in pkts if t == 26)
        print(f"  bulk={n_bulk} control={n_ctrl}")
    # per-endpoint bulk data reassembly (guest->host = OUT data, host->guest = IN data)
    streams = {}
    for d in (0, 1):
        pkts, _ = parse_stream(bufs[d])
        for typ, pid, payload in pkts:
            if typ not in (101, 104) or len(payload) < 10:
                continue
            ep, status = payload[0], payload[1]
            ln = struct.unpack("<H", payload[2:4])[0] | (struct.unpack("<H", payload[8:10])[0] << 16)
            data = payload[10:10 + ln] if len(payload) >= 10 + ln else payload[10:]
            # OUT data rides guest->host; IN data rides host->guest
            is_out_ep = (ep & 0x80) == 0
            if (d == 0 and is_out_ep and data) or (d == 1 and not is_out_ep and data):
                streams.setdefault((ep, d), []).append(data)
    for (ep, d), chunks in sorted(streams.items()):
        blob = b"".join(chunks)
        direction = "OUT" if d == 0 else "IN"
        fn = f"captures/redir_ep{ep:02x}_{direction}.bin"
        if not tl_only:
            open(fn, "wb").write(blob)
        print(f"EP {ep:#04x} {direction}: {len(chunks)} pkts, {len(blob)}B -> {fn}")
    # command timeline (EP3 OUT first bytes + EP2 headers)
    if not tl_only:
        return
    for d in (0,):
        pkts, _ = parse_stream(bufs[d])
        for typ, pid, payload in pkts:
            if typ not in (101,) or len(payload) < 11:
                continue
            ep = payload[0]
            ln = struct.unpack("<H", payload[2:4])[0] | (struct.unpack("<H", payload[8:10])[0] << 16)
            data = payload[10:10 + ln]
            if ep in (0x02, 0x03, 0x04) and data:
                print(f"OUT ep{ep:#04x} len={len(data)} {data[:40].hex()}")


main()
