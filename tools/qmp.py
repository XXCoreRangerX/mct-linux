#!/usr/bin/env python3
"""Tiny QMP client: qmp.py screendump out.ppm | key ctrl-alt-delete | type 'text' | click x y | cmd '{json}'"""
import json, os, socket, sys, time

SOCK = os.environ.get("QMP_SOCK", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "vm", "qmp.sock"))


def conn():
    s = socket.socket(socket.AF_UNIX); s.connect(SOCK); f = s.makefile("rw")
    json.loads(f.readline()); send(f, {"execute": "qmp_capabilities"}); return f


def send(f, obj):
    f.write(json.dumps(obj) + "\n"); f.flush()
    while True:
        r = json.loads(f.readline())
        if "return" in r or "error" in r:
            return r


KEYMAP = {" ": "spc", "\n": "ret", "-": "minus", "_": ("shift", "minus"), ".": "dot", ":": ("shift", "semicolon"),
          "\\": "backslash", "/": "slash", "=": "equal", "'": "apostrophe", '"': ("shift", "apostrophe"),
          "(": ("shift", "9"), ")": ("shift", "0"), ",": "comma", ";": "semicolon", "*": ("shift", "8"),
          "&": ("shift", "7"), "|": ("shift", "backslash"), ">": ("shift", "dot"), "<": ("shift", "comma"),
          "$": ("shift", "4"), "{": ("shift", "bracket_left"), "}": ("shift", "bracket_right"),
          "[": "bracket_left", "]": "bracket_right", "@": ("shift", "2"), "!": ("shift", "1"), "+": ("shift", "equal")}


def keys_for(ch):
    if ch in KEYMAP:
        k = KEYMAP[ch]
        return list(k) if isinstance(k, tuple) else [k]
    if ch.isupper():
        return ["shift", ch.lower()]
    return [ch]


def key(f, combo):
    send(f, {"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": k} for k in combo]}})
    time.sleep(0.05)


def click(f, x, y, w=1280, h=800, button="left"):
    ax, ay = int(x * 32767 / w), int(y * 32767 / h)
    send(f, {"execute": "input-send-event", "arguments": {"events": [
        {"type": "abs", "data": {"axis": "x", "value": ax}}, {"type": "abs", "data": {"axis": "y", "value": ay}}]}})
    for down in (True, False):
        send(f, {"execute": "input-send-event", "arguments": {"events": [
            {"type": "btn", "data": {"down": down, "button": button}}]}})
        time.sleep(0.05)


if __name__ == "__main__":
    f = conn(); a = sys.argv[1:]
    if a[0] == "screendump":
        print(send(f, {"execute": "screendump", "arguments": {"filename": a[1], "format": "png"}}))
    elif a[0] == "key":
        key(f, a[1].split("-"))
    elif a[0] == "type":
        for ch in a[1]:
            key(f, keys_for(ch))
    elif a[0] == "click":
        click(f, float(a[1]), float(a[2]), *(int(v) for v in a[3:5]))
    elif a[0] == "cmd":
        print(send(f, json.loads(a[1])))
