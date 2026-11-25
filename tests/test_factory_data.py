#!/usr/bin/env python3
"""Exercise factory CRC/layout interoperability and safe host-side updates."""
import ctypes
import hashlib
import hmac
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('factory', ROOT / 'firmware/scripts/factory_data.py')
f = importlib.util.module_from_spec(spec); spec.loader.exec_module(f)

class Factory(ctypes.Structure):
    _fields_ = [('serial', ctypes.c_char*33), ('country', ctypes.c_char*3),
                ('ssid', ctypes.c_char*33), ('password', ctypes.c_char*65),
                ('public', ctypes.c_uint8*32), ('private', ctypes.c_uint8*32), ('channel', ctypes.c_uint8)]

class Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        lib = Path(cls.temp.name) / 'factory.so'
        subprocess.run(['cc', '-shared', '-fPIC', '-Wall', '-Wextra', '-Werror',
                        str(ROOT/'firmware/src/factory_record.c'), '-o', str(lib)], check=True)
        cls.lib = ctypes.CDLL(str(lib))
        cls.lib.ez_factory_parse.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(Factory)]
        cls.lib.ez_factory_has_wifi.argtypes = [ctypes.POINTER(Factory)]
        cls.lib.ez_factory_has_wifi.restype = ctypes.c_bool
    @classmethod
    def tearDownClass(cls): cls.temp.cleanup()
    def sample(self):
        return dict(serial='test-camera', country='US', channel=27, ssid='TestAP', password='test-only-password',
                    public_key=bytes(range(1,33)), private_key=bytes(range(33,65)))
    def parse(self, raw):
        result = Factory()
        rc = self.lib.ez_factory_parse(ctypes.create_string_buffer(raw), len(raw), ctypes.byref(result))
        return rc, result
    def test_hardware_extension(self):
        record = self.sample(); original = f.encode(record)
        for mode, name in enumerate(f.HARDWARE):
            record['hardware'] = name
            page = f.encode(record, original)
            self.assertEqual(page[:211], original[:211])
            self.assertEqual(f.decode(page)['hardware'], name)
            value = ctypes.c_uint8(99)
            self.assertEqual(self.lib.ez_factory_hardware_parse(ctypes.create_string_buffer(page), len(page), ctypes.byref(value)), 0)
            self.assertEqual(value.value, mode)
            corrupt = bytearray(page); corrupt[264] ^= 1
            self.assertNotIn('hardware', f.decode(corrupt))
            self.assertLess(self.lib.ez_factory_hardware_parse(ctypes.create_string_buffer(bytes(corrupt)), len(page), ctypes.byref(value)), 0)
        self.assertNotIn('hardware', f.decode(original))
        record['hardware'] = None
        self.assertNotIn('hardware', f.decode(f.encode(record, page)))
        occupied = bytearray(original); occupied[256:272] = b'unknown-layout!!'
        record['hardware'] = 'gps'
        with self.assertRaises(ValueError): f.encode(record, occupied)

    def test_motion_extension(self):
        original = f.encode(self.sample())
        self.assertEqual(f.summary(f.decode(original))['shake_threshold_mg'], 1000)
        for mg in (500, 1000, 2000, 4000):
            record = self.sample(); record.update(hardware='camera', shake_threshold_mg=mg)
            page = f.encode(record, original)
            self.assertEqual(page[:211], original[:211])
            self.assertEqual(f.decode(page)['shake_threshold_mg'], mg)
            self.assertEqual(f.decode(page)['hardware'], 'camera')
            self.assertEqual(f.ble_qr(record), f.ble_qr(self.sample()))
            threshold = ctypes.c_uint16(0)
            self.assertEqual(self.lib.ez_factory_motion_parse(ctypes.create_string_buffer(page), len(page), ctypes.byref(threshold)), 0)
            self.assertEqual(threshold.value, mg)
            for offset in (272, 276, 278, 280, 282, 284):
                corrupt = bytearray(page); corrupt[offset] ^= 1
                self.assertNotIn('shake_threshold_mg', f.decode(corrupt))
                self.assertLess(self.lib.ez_factory_motion_parse(ctypes.create_string_buffer(bytes(corrupt)), len(page), ctypes.byref(threshold)), 0)
            updated = f.decode(page); updated['country'] = 'EU'
            self.assertEqual(f.encode(updated, page)[272:288], page[272:288])
            updated['shake_threshold_mg'] = None
            cleared = f.encode(updated, page)
            self.assertNotIn('shake_threshold_mg', f.decode(cleared))
            self.assertEqual(cleared[256:272], page[256:272])
        threshold = ctypes.c_uint16(0)
        self.assertLess(self.lib.ez_factory_motion_parse(ctypes.create_string_buffer(original), len(original), ctypes.byref(threshold)), 0)
        for invalid in (0, 499, 4001, True, 1000.5):
            record = self.sample(); record['shake_threshold_mg'] = invalid
            with self.assertRaises(ValueError): f.encode(record)
        occupied = bytearray(original); occupied[272:288] = b'unknown-layout!!'
        record = self.sample(); record['shake_threshold_mg'] = 1000
        with self.assertRaises(ValueError): f.encode(record, occupied)

    def test_v2_interoperability_and_wifi(self):
        record = self.sample(); raw = f.encode(record)
        self.assertEqual(f.RECORD_SIZE, 211)
        self.assertEqual(f.decode(raw), record)
        rc, parsed = self.parse(raw); self.assertEqual(rc, 0)
        self.assertEqual(parsed.country, b'US'); self.assertEqual(parsed.serial, b'test-camera')
        self.assertTrue(self.lib.ez_factory_has_wifi(ctypes.byref(parsed)))
        record['ssid'] = record['password'] = ''
        rc, parsed = self.parse(f.encode(record)); self.assertEqual(rc, 0)
        self.assertFalse(self.lib.ez_factory_has_wifi(ctypes.byref(parsed)))
    def test_update_preserves_identity_and_trailing_page(self):
        record = self.sample(); original = f.encode(record)[:-1] + b'\xa5'
        updated = f.decode(original); updated.update(country='EU', ssid='', password='')
        raw = f.encode(updated, original)
        self.assertEqual(raw[8:105], original[8:105])
        self.assertEqual(raw[211:], original[211:])
        self.assertEqual(self.parse(raw)[1].country, b'EU')
        self.assertEqual(f.ble_qr(record), f.ble_qr(updated))
    def test_crc_and_invalid_header_rejected(self):
        raw = f.encode(self.sample())
        for offset in [0,4,6,8,41,73,105,108,141,142,207]:
            corrupt = bytearray(raw); corrupt[offset] ^= 1
            self.assertLess(self.parse(bytes(corrupt))[0], 0)
            with self.assertRaises(ValueError): f.decode(corrupt)
        self.assertLess(self.parse(raw[:200])[0], 0)
    def test_invalid_country_and_wifi_rejected(self):
        for patch in [dict(country='eu'), dict(country='ZZZ'), dict(ssid=''), dict(password='short'),
                      dict(password='x'*64), dict(ssid='x'*33), dict(serial=''), dict(private_key=b'\0'*32)]:
            record = self.sample(); record.update(patch)
            with self.assertRaises(ValueError): f.encode(record)
    def test_pop_and_private_output_permissions(self):
        record = self.sample(); qr = f.ble_qr(record)
        # Public fixture vectors pin the existing on-device credential format
        # across the EZ-DEV54 product rename (these are not device secrets).
        self.assertEqual(qr['pop'], 'dd2e90ccc90881d81e5c1de1b5f9ee36')
        self.assertEqual(qr['name'], 'PROV_CAMae216c')
        self.assertEqual(qr['pop'], hmac.new(record['private_key'], f.LABEL, hashlib.sha256).hexdigest()[:32])
        self.assertNotIn('password', f.summary(record)); self.assertNotIn('private_key', f.summary(record))
        path = Path(self.temp.name)/'secret.bin'; f.secret_write(path, f.encode(record))
        self.assertEqual(path.stat().st_mode & 0o777, 0o600)
        with self.assertRaises(FileExistsError): f.secret_write(path, b'overwrite')

if __name__ == '__main__': unittest.main()
