#!/usr/bin/env python3
"""The access point's half of a WPA2-Personal handshake, written independently
of kernel/network/wpa_handshake.c: Python's hmac and hashlib for the key
hierarchy and the MICs, the host's openssl for AES. It prints the frames an
authenticator sends and the frames a correct supplicant must answer with, as
C arrays for tests/test_wpa_handshake.c. Run it again only if the scenario
changes; the output is checked in."""
import hashlib, hmac, struct, subprocess

def aes_encrypt(key, block):
    return subprocess.run(["openssl", "enc", "-aes-128-ecb", "-nopad", "-K", key.hex()],
                          input=block, capture_output=True, check=True).stdout

def key_wrap(kek, plain):
    n = len(plain) // 8
    a = b"\xa6" * 8
    r = [plain[i * 8:i * 8 + 8] for i in range(n)]
    for j in range(6):
        for i in range(n):
            b = aes_encrypt(kek, a + r[i])
            t = n * j + i + 1
            a = (int.from_bytes(b[:8], "big") ^ t).to_bytes(8, "big")
            r[i] = b[8:]
    return a + b"".join(r)

def prf(key, label, data, n):
    out, i = b"", 0
    while len(out) < n:
        out += hmac.new(key, label + b"\0" + data + bytes([i]), hashlib.sha1).digest()
        i += 1
    return out[:n]

PMK = hashlib.pbkdf2_hmac("sha1", b"ThisIsAPassword", b"ThisIsASSID", 4096, 32)
AA = bytes.fromhex("020000000001")
SPA = bytes.fromhex("020000000002")
ANONCE = bytes(range(0xA0, 0xC0))
SNONCE = bytes(range(0x40, 0x60))
GTK1 = bytes(range(0xC0, 0xD0))
GTK2 = bytes(range(0xD0, 0xE0))
RSN = bytes.fromhex("30140100000fac040100000fac040100000fac020000")
data = min(AA, SPA) + max(AA, SPA) + min(ANONCE, SNONCE) + max(ANONCE, SNONCE)
ptk = prf(PMK, b"Pairwise key expansion", data, 48)
KCK, KEK, TK = ptk[:16], ptk[16:32], ptk[32:48]

def frame(info, key_length, replay, nonce, key_data, rsc=b"\0" * 8, version=2):
    body = struct.pack(">BHH", 2, info, key_length) + struct.pack(">Q", replay) + nonce + b"\0" * 16 + rsc + \
        b"\0" * 8 + b"\0" * 16 + struct.pack(">H", len(key_data)) + key_data
    f = struct.pack(">BBH", version, 3, len(body)) + body
    return f

def with_mic(f):
    mic = hmac.new(KCK, f, hashlib.sha1).digest()[:16]
    return f[:81] + mic + f[97:]

def kde_padded(content):
    if len(content) % 8 or len(content) < 16:
        content += b"\xdd"
        while len(content) % 8 or len(content) < 16:
            content += b"\0"
    return content

def gtk_kde(index, gtk):
    return bytes([0xDD, 6 + len(gtk), 0x00, 0x0F, 0xAC, 0x01, index, 0]) + gtk

msg1 = frame(0x008A, 16, 1, ANONCE, b"")
msg2 = with_mic(frame(0x010A, 0, 1, SNONCE, RSN))
msg3 = with_mic(frame(0x13CA, 16, 2, ANONCE, key_wrap(KEK, kde_padded(RSN + gtk_kde(1, GTK1))),
                      rsc=bytes([5, 0, 0, 0, 0, 0, 0, 0])))
msg4 = with_mic(frame(0x030A, 0, 2, b"\0" * 32, b""))
group1 = with_mic(frame(0x1382, 0, 3, b"\0" * 32, key_wrap(KEK, kde_padded(gtk_kde(2, GTK2)))))
group2 = with_mic(frame(0x0302, 0, 3, b"\0" * 32, b""))
bad_rsn = bytes.fromhex("30140100000fac020100000fac020100000fac020000")
msg3_downgraded = with_mic(frame(0x13CA, 16, 2, ANONCE, key_wrap(KEK, kde_padded(bad_rsn + gtk_kde(1, GTK1)))))

def c_array(name, b):
    lines = ", ".join("0x%02x" % x for x in b)
    return "static const uint8_t %s[%d] = {%s};" % (name, len(b), lines)

for name, value in [("pmk", PMK), ("ap_address", AA), ("own_address", SPA), ("anonce", ANONCE),
                    ("snonce", SNONCE), ("ap_rsn", RSN), ("expected_kck", KCK), ("expected_kek", KEK),
                    ("expected_tk", TK), ("gtk1", GTK1), ("gtk2", GTK2), ("message_1", msg1),
                    ("expected_message_2", msg2), ("message_3", msg3), ("expected_message_4", msg4),
                    ("group_message_1", group1), ("expected_group_message_2", group2),
                    ("message_3_downgraded", msg3_downgraded)]:
    print(c_array(name, value))
