#!/usr/bin/env python3
"""Check the C protocol against Espressif's unmodified Python Security1 client.

IDF_PATH must identify an ESP-IDF checkout (PlatformIO's package also works).
The firmware itself does not depend on ESP-IDF. No Bluetooth or flash access.
"""
import ctypes as C
import hashlib
import os
from pathlib import Path
import random
import subprocess
import sys
import tempfile
import unittest

from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives import serialization

ROOT = Path(__file__).resolve().parents[1]
IDF = Path(os.environ.get("IDF_PATH", Path.home() / ".platformio/packages/framework-espidf"))
os.environ["IDF_PATH"] = str(IDF)
sys.path.insert(0, str(IDF / "tools/esp_prov"))
from security.security1 import Security1
import proto

U8 = C.c_uint8
P8 = C.POINTER(U8)
class AP(C.Structure):
    _fields_ = [("ssid", U8 * 32), ("ssid_len", U8), ("bssid", U8 * 6),
                ("auth", U8), ("channel", C.c_uint32), ("rssi", C.c_int32)]
class Status(C.Structure):
    _fields_ = [("state", C.c_uint32), ("fail_reason", C.c_uint32), ("ip", C.c_char * 16), ("ap", AP)]
B = C.CFUNCTYPE(C.c_int, C.c_void_p, P8, P8, P8)
X = C.CFUNCTYPE(C.c_int, C.c_void_p, P8, C.c_size_t)
R = C.CFUNCTYPE(None, C.c_void_p)
S = C.CFUNCTYPE(C.c_int, C.c_void_p, P8, C.c_size_t, P8, C.c_size_t)
A = C.CFUNCTYPE(C.c_int, C.c_void_p)
T = C.CFUNCTYPE(None, C.c_void_p, C.POINTER(Status))
SC = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_bool, C.c_bool)
SS = C.CFUNCTYPE(None, C.c_void_p, C.POINTER(C.c_bool), C.POINTER(C.c_uint32))
SR = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_uint32, C.POINTER(AP))
class Ops(C.Structure):
    _fields_ = [("begin", B), ("crypt", X), ("reset", R), ("set", S),
                ("apply", A), ("status", T), ("scan", SC), ("scan_status", SS), ("scan_result", SR)]
class Protocol(C.Structure):
    _fields_ = [("ops", C.POINTER(Ops)), ("user", C.c_void_p), ("state", U8), ("client", U8 * 32), ("device", U8 * 32)]

class Server:
    def __init__(self, lib, pop="test-only-proof"):
        self.lib, self.pop, self.cipher = lib, pop, None
        self.ssid, self.password, self.applies, self.state = b"", b"", 0, 2
        self.ops = Ops(B(self.begin), X(self.crypt), R(self.reset), S(self.set), A(self.apply),
                       T(self.status), SC(self.scan), SS(self.scan_status), SR(self.scan_result))
        self.protocol = Protocol(C.pointer(self.ops), None, 0)

    def begin(self, _, client, device, iv):
        try:
            private = X25519PrivateKey.generate()
            public = private.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
            secret = private.exchange(X25519PublicKey.from_public_bytes(C.string_at(client, 32)))
            digest = hashlib.sha256(self.pop.encode()).digest()
            key = bytes(a ^ b for a, b in zip(secret, digest))
            nonce = os.urandom(16)
            C.memmove(device, public, 32); C.memmove(iv, nonce, 16)
            self.cipher = Cipher(algorithms.AES(key), modes.CTR(nonce)).encryptor()
            return 0
        except ValueError:
            return -22

    def crypt(self, _, buf, length):
        if self.cipher is None: return -1
        C.memmove(buf, self.cipher.update(C.string_at(buf, length)), length)
        return 0
    def reset(self, _): self.cipher = None
    def set(self, _, ssid, sn, password, pn):
        self.ssid, self.password = C.string_at(ssid, sn), C.string_at(password, pn)
        return 0
    def apply(self, _):
        self.applies += 1; self.state = 0
        return 0
    def status(self, _, result):
        result.contents.state = self.state
        result.contents.ip = b"192.0.2.10"
        result.contents.ap.ssid_len = len(self.ssid)
        result.contents.ap.ssid[:len(self.ssid)] = self.ssid
        result.contents.ap.auth = 6
    def scan(self, _, passive, blocking): return 0
    def scan_status(self, _, done, count): done[0], count[0] = True, 8
    def scan_result(self, _, index, result):
        if index >= 8: return -2
        result.contents.ssid[:] = bytes([65 + index]) * 32
        result.contents.ssid_len = 32; result.contents.rssi = -60 - index
        result.contents.channel = index + 1; result.contents.auth = 6
        return 0
    def exchange(self, endpoint, request, capacity=512):
        if isinstance(request, str): request = request.encode("latin-1")
        req = (U8 * len(request)).from_buffer_copy(request); out = (U8 * 512)()
        count = self.lib.prov_protocol_exchange(C.byref(self.protocol), endpoint, req, len(request), out, capacity)
        if count < 0: raise ValueError(count)
        return bytes(out[:count])
    def handshake(self, pop=None):
        client = Security1(self.pop if pop is None else pop, False)
        req = client.security1_session(None)
        req = client.security1_session(self.exchange(0, req).decode("latin-1"))
        assert client.security1_session(self.exchange(0, req).decode("latin-1")) is None
        return client
    def secure(self, client, endpoint, request, response_type):
        raw = self.exchange(endpoint, client.encrypt_data(request.SerializeToString()))
        result = response_type(); result.ParseFromString(client.decrypt_data(raw))
        return result

class Interop(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="ezdev54-proto-test-")
        library = Path(cls.temp.name) / "protocol.so"
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-dynamiclib" if sys.platform == "darwin" else "-shared", "-fPIC", "-O1",
                        str(ROOT / "firmware/src/prov_protocol.c"), "-o", str(library)], check=True)
        cls.lib = C.CDLL(str(library))
        cls.lib.prov_protocol_exchange.argtypes = [C.POINTER(Protocol), C.c_int, P8, C.c_size_t, P8, C.c_size_t]
        cls.lib.prov_protocol_exchange.restype = C.c_int
    @classmethod
    def tearDownClass(cls): cls.temp.cleanup()
    def setUp(self): self.server = Server(self.lib)

    def test_official_client_handshake_config_apply_and_status(self):
        client = self.server.handshake()
        pb = proto.wifi_config_pb2
        req = pb.WiFiConfigPayload(msg=2)
        req.cmd_set_config.ssid = b"halowlink2"; req.cmd_set_config.passphrase = b"test-password"
        resp = self.server.secure(client, 1, req, pb.WiFiConfigPayload)
        self.assertEqual((resp.msg, resp.resp_set_config.status), (3, 0))
        req = pb.WiFiConfigPayload(msg=4); req.cmd_apply_config.SetInParent()
        self.assertEqual(self.server.secure(client, 1, req, pb.WiFiConfigPayload).msg, 5)
        req = pb.WiFiConfigPayload(); req.cmd_get_status.SetInParent()
        resp = self.server.secure(client, 1, req, pb.WiFiConfigPayload)
        self.assertEqual(resp.resp_get_status.sta_state, 0)
        self.assertEqual(resp.resp_get_status.connected.ssid, b"halowlink2")
        self.assertEqual(resp.resp_get_status.connected.ip4_addr, "192.0.2.10")
        self.assertEqual(self.server.applies, 1)

    def test_scan_full_names_and_negative_rssi(self):
        client = self.server.handshake(); pb = proto.wifi_scan_pb2
        req = pb.WiFiScanPayload(); req.cmd_scan_start.SetInParent()
        self.assertEqual(self.server.secure(client, 2, req, pb.WiFiScanPayload).msg, 1)
        req = pb.WiFiScanPayload(msg=2); req.cmd_scan_status.SetInParent()
        resp = self.server.secure(client, 2, req, pb.WiFiScanPayload)
        self.assertTrue(resp.resp_scan_status.scan_finished)
        self.assertEqual(resp.resp_scan_status.result_count, 8)
        req = pb.WiFiScanPayload(msg=4); req.cmd_scan_result.count = 16
        resp = self.server.secure(client, 2, req, pb.WiFiScanPayload)
        self.assertEqual(len(resp.resp_scan_result.entries), 8)
        self.assertEqual(resp.resp_scan_result.entries[-1].rssi, -67)
        self.assertEqual(resp.resp_scan_result.entries[0].ssid, b"A" * 32)

    def test_wrong_pop_does_not_authorize(self):
        with self.assertRaises(ValueError): self.server.handshake("wrong-proof")
        self.assertEqual(self.server.protocol.state, 0)
        with self.assertRaises(ValueError): self.server.exchange(1, b"")
        self.assertEqual(self.server.applies, 0)

    def test_invalid_credentials_are_rejected(self):
        client = self.server.handshake(); pb = proto.wifi_config_pb2
        for ssid in [b"", b"x" * 33, b"bad\0name"]:
            req = pb.WiFiConfigPayload(msg=2); req.cmd_set_config.ssid = ssid
            resp = self.server.secure(client, 1, req, pb.WiFiConfigPayload)
            self.assertEqual(resp.resp_set_config.status, 4)
        self.assertEqual(self.server.ssid, b"")

    def test_encrypted_malformed_payload_invalidates_session(self):
        client = self.server.handshake()
        with self.assertRaises(ValueError): self.server.exchange(1, client.encrypt_data(b"\x62\xff\xff"))
        self.assertEqual(self.server.protocol.state, 0)
        self.assertEqual(self.server.applies, 0)

    def test_security0_and_low_order_public_key_are_rejected(self):
        req = proto.session_pb2.SessionData()
        req.sec0.sc.SetInParent()
        with self.assertRaises(ValueError): self.server.exchange(0, req.SerializeToString())
        req = proto.session_pb2.SessionData(sec_ver=1)
        req.sec1.sc0.client_pubkey = bytes(32)
        with self.assertRaises(ValueError): self.server.exchange(0, req.SerializeToString())

    def test_capacity_and_malformed_wire_bounds(self):
        with self.assertRaises(ValueError): self.server.exchange(3, b"", 1)
        rng = random.Random(137)
        for i in range(1000):
            data = rng.randbytes(rng.randrange(0, 96))
            try: self.server.exchange(0, data)
            except ValueError: pass
        self.assertEqual(self.server.applies, 0)

if __name__ == "__main__": unittest.main(verbosity=2)
