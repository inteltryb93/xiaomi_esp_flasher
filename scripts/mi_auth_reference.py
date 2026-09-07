#!/usr/bin/env python3
"""Reference implementation of the Mi activation key derivation (deriveTheKey / do_login_generate from
TelinkMiFlasher.html) with the 'cryptography' package.  Prints the known-answer vectors used by
MiAuth::self_test() in components/xiaomi_esp_flasher/mi_auth.cpp so the ESP32 crypto can be verified."""
from cryptography.hazmat.primitives import hashes, hmac
from cryptography.hazmat.primitives.kdf.hkdf import HKDF
from cryptography.hazmat.primitives.ciphers.aead import AESCCM

shared = bytes(range(32))
derived = HKDF(algorithm=hashes.SHA256(), length=64, salt=None, info=b"mible-setup-info").derive(shared)
print("hkdf:" + derived.hex())
token, bind_key, mi_bind_a = derived[:12], derived[12:28], derived[28:44]
did = b"\x00blt.3.129vabcdefg00"
assert len(did) == 20
enc = AESCCM(mi_bind_a, tag_length=4).encrypt(bytes.fromhex("101112131415161718191A1B"), did, b"devID")
print("ccm:" + enc.hex())
rand_host = bytes(range(16))
rand_dev = bytes(range(0x20, 0x30))
salt = rand_host + rand_dev
salt1 = rand_dev + rand_host
d2 = HKDF(algorithm=hashes.SHA256(), length=64, salt=salt, info=b"mible-login-info").derive(token)
h = hmac.HMAC(d2[:16], hashes.SHA256()); h.update(salt1); expected = h.finalize()
h = hmac.HMAC(d2[16:32], hashes.SHA256()); h.update(salt); send = h.finalize()
print("expected:" + expected.hex())
print("send:" + send.hex())
