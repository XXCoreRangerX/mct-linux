#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""TCP tee: qemu (usb-redir) <-> usbredirserver, logging everything.
Listens on 127.0.0.1:32001, forwards to 127.0.0.1:32002.
Log format per record: u64 usec | u8 dir (0=qemu->srv, 1=srv->qemu) | u32 len | bytes.
"""
import socket, struct, sys, threading, time

LISTEN = ("127.0.0.1", 32001)
UPSTREAM = ("127.0.0.1", 32002)
LOG = sys.argv[1] if len(sys.argv) > 1 else "redir.bin"


def pump(src, dst, direction, log):
    try:
        while True:
            data = src.recv(65536)
            if not data:
                break
            rec = struct.pack("<Q B I", int(time.time() * 1e6), direction, len(data)) + data
            log.write(rec)
            log.flush()
            dst.sendall(data)
    except OSError:
        pass
    try:
        dst.shutdown(socket.SHUT_WR)
    except OSError:
        pass


def main():
    ls = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(LISTEN)
    ls.listen(1)
    print(f"tee listening {LISTEN}, upstream {UPSTREAM}, log {LOG}", flush=True)
    log = open(LOG, "wb")
    conn, addr = ls.accept()
    print(f"qemu connected from {addr}", flush=True)
    up = socket.create_connection(UPSTREAM)
    print("upstream connected", flush=True)
    t1 = threading.Thread(target=pump, args=(conn, up, 0, log), daemon=True)
    t2 = threading.Thread(target=pump, args=(up, conn, 1, log), daemon=True)
    t1.start()
    t2.start()
    t1.join()
    t2.join()
    print("connection closed", flush=True)


main()
