#!/usr/bin/env python3
"""Fake Fire TV adbd for testing the 3DS ADB client on a PC.
Implements CNXN/AUTH (with real RSA verification of the Android pubkey format),
OPEN shell:, WRTE/OKAY flow control. Usage: mock_adbd.py <bind-ip> <port>"""
import base64, os, socket, struct, sys
from cryptography.hazmat.primitives.asymmetric import padding, rsa, utils
from cryptography.hazmat.primitives import hashes
from cryptography.exceptions import InvalidSignature

CNXN, AUTH, OPEN, OKAY, CLSE, WRTE = 0x4e584e43, 0x48545541, 0x4e45504f, 0x59414b4f, 0x45534c43, 0x45545257
trusted = []   # accepted public keys
log = open(sys.argv[3], "a") if len(sys.argv) > 3 else sys.stdout

def send(c, cmd, a0, a1, data=b""):
    c.sendall(struct.pack("<6I", cmd, a0, a1, len(data), sum(data) & 0xffffffff, cmd ^ 0xffffffff) + data)

def recv(c):
    h = b""
    while len(h) < 24:
        d = c.recv(24 - len(h))
        if not d: raise EOFError
        h += d
    cmd, a0, a1, ln, crc, magic = struct.unpack("<6I", h)
    assert magic == cmd ^ 0xffffffff, "bad magic"
    data = b""
    while len(data) < ln:
        data += c.recv(ln - len(data))
    assert sum(data) & 0xffffffff == crc, "bad checksum"
    return cmd, a0, a1, data

def parse_pub(s):
    b64 = s.split(b" ")[0]
    blob = base64.b64decode(b64)
    assert len(blob) == 524, len(blob)
    ln, n0inv = struct.unpack("<II", blob[:8])
    assert ln == 64
    n = int.from_bytes(blob[8:264], "little")
    rr = int.from_bytes(blob[264:520], "little")
    e = struct.unpack("<I", blob[520:524])[0]
    assert (n0inv * n) % 2**32 == 2**32 - 1, "n0inv wrong"
    assert rr == pow(2, 4096, n), "rr wrong"
    return rsa.RSAPublicNumbers(e, n).public_key()

def verify(pub, token, sig):
    try:
        pub.verify(sig, token, padding.PKCS1v15(), utils.Prehashed(hashes.SHA1()))
        return True
    except InvalidSignature:
        return False

def session(c):
    cmd, a0, a1, data = recv(c)
    assert cmd == CNXN and data.startswith(b"host::"), data
    token = os.urandom(20)
    send(c, AUTH, 1, 0, token)
    last_sig = None
    while True:
        cmd, a0, a1, data = recv(c)
        assert cmd == AUTH
        if a0 == 2:
            last_sig = data
            if any(verify(k, token, data) for k in trusted):
                print("AUTH ok (known key)", file=log, flush=True); break
            token = os.urandom(20); send(c, AUTH, 1, 0, token)
        elif a0 == 3:
            assert data.endswith(b"\0")
            pub = parse_pub(data[:-1])
            print("got pubkey:", data[-25:-1], file=log, flush=True)
            trusted.append(pub)   # "user taps Allow"
            break
    send(c, CNXN, 0x01000001, 256 * 1024, b"device::ro.product.name=mock;ro.product.model=AFTMOCK;ro.product.device=x;\0")
    cmd, a0, a1, data = recv(c)
    assert cmd == OPEN and data == b"shell:\0", data
    local = a0
    send(c, OKAY, 77, local)
    can_write = True   # client may write only after our OKAY
    unacked = 0        # our WRTEs the client hasn't OKAYed
    while True:
        cmd, a0, a1, data = recv(c)
        if cmd == WRTE:
            assert can_write, "client wrote twice without waiting for OKAY"
            can_write = False
            assert a0 == local and a1 == 77
            print("SHELL:", data.decode().rstrip(), file=log, flush=True)
            send(c, WRTE, 77, local, b"$ ")   # prompt the client must ack
            unacked += 1
            assert unacked < 3, "client is not acking shell output"
            send(c, OKAY, 77, local); can_write = True
        elif cmd == OKAY:
            unacked -= 1

s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind((sys.argv[1], int(sys.argv[2]))); s.listen(4)
while True:
    c, _ = s.accept()
    try: session(c)
    except EOFError: print("client closed", file=log, flush=True)
    except AssertionError as e: print("ASSERT FAIL:", e, file=log, flush=True)
    c.close()
