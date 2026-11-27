"""
Part sizes of the QR encoders that show a PSBT on the screen.

The density of an animated QR code is given as the text length of one code
(QR_SIZES in gui/components/qrcode.py), for every encoder. Runs on CPython,
with the MicroPython-only helpers replaced by equivalents.
"""
import binascii
import builtins
import importlib.util
import io
import os
import sys
import tempfile
import types
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..", "..")
LIBS = os.path.join(ROOT, "f469-disco", "libs", "common")

# Default density of gui/components/qrcode.py: QR_SIZES[QRCode.QR_VERSION]
COARSE = 154
DENSE = 858


def _helpers():
    helpers = types.ModuleType("helpers")

    def b2a_base64_stream(sin, sout):
        total = 0
        while True:
            chunk = sin.read(48)
            if not chunk:
                return total
            total += sout.write(binascii.b2a_base64(chunk).strip())

    def read_write(fin, fout, chunk_size=32):
        total = 0
        while True:
            chunk = fin.read(chunk_size)
            if not chunk:
                return total
            total += fout.write(chunk)

    helpers.b2a_base64_stream = b2a_base64_stream
    helpers.read_write = read_write
    return helpers


def _load_qrencoder():
    sys.path.insert(0, LIBS)
    try:
        with mock.patch.dict(sys.modules, {"helpers": _helpers()}):
            spec = importlib.util.spec_from_file_location(
                "qrencoder", os.path.join(ROOT, "src", "qrencoder.py"))
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
    finally:
        sys.path.remove(LIBS)
    return module


qrencoder = _load_qrencoder()

# a signed PSBT of a typical size, about 500 bytes; the content does not
# matter for the part sizes
PSBT = b"psbt\xff" + bytes(range(256)) * 2


class EncoderTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.path = os.path.join(self.tmp.name, "qrtmp")

    def tearDown(self):
        self.tmp.cleanup()

    def encode(self, cls, data=PSBT):
        if cls is qrencoder.CryptoPSBTEncoder:
            # MicroPython text files have readinto(), which the UR encoder
            # uses; CPython ones do not, so hand it the file in binary mode
            binary = lambda path, mode="r": builtins.open(path, "rb" if mode == "r" else mode)
            patch = mock.patch.object(qrencoder, "open", binary, create=True)
            patch.start()
            self.addCleanup(patch.stop)
        return cls(io.BytesIO(data), tempfile=self.path)

    def frames(self, enc, count=None):
        return [enc[i] for i in range(count or len(enc))]

    def test_base64_coarse_parts(self):
        with self.encode(qrencoder.Base64QREncoder) as enc:
            enc.part_len = COARSE
            self.assertEqual(enc.get_full(COARSE), "")
            frames = self.frames(enc)
            self.assertGreater(len(frames), 1)
            for i, frame in enumerate(frames):
                self.assertTrue(frame.startswith("p%dof%d " % (i + 1, len(frames))))
                self.assertLessEqual(len(frame), COARSE)
            joined = "".join(frame.split(" ", 1)[1] for frame in frames)
            self.assertEqual(binascii.a2b_base64(joined), PSBT)

    def test_base64_single_part_is_plain(self):
        with self.encode(qrencoder.Base64QREncoder) as enc:
            enc.part_len = DENSE
            self.assertEqual(len(enc), 1)
            self.assertEqual(binascii.a2b_base64(enc[0]), PSBT)
            # the animation asks for the same frame again
            self.assertEqual(enc[0], enc[1])

    def test_small_psbt_fits_one_coarse_code(self):
        small = b"psbt\xff" + bytes(40)
        with self.encode(qrencoder.Base64QREncoder, small) as enc:
            enc.part_len = COARSE
            self.assertEqual(binascii.a2b_base64(enc.get_full(COARSE)), small)

    def test_ur_parts_have_the_size_of_the_density(self):
        with self.encode(qrencoder.CryptoPSBTEncoder) as enc:
            enc.part_len = COARSE
            frames = self.frames(enc)
            self.assertGreater(len(frames), 1)
            for frame in frames:
                self.assertTrue(frame.upper().startswith("UR:CRYPTO-PSBT/"))
                # capital letters take 11 bits for every 8 of a byte in a
                # QR code; plus header and checksum
                self.assertLessEqual(len(frame), COARSE * 11 // 8 + 40)

    def test_ur_density_changes_the_number_of_parts(self):
        with self.encode(qrencoder.CryptoPSBTEncoder) as enc:
            enc.part_len = COARSE
            coarse = len(enc)
            enc.part_len = DENSE
            self.assertLess(len(enc), coarse)


if __name__ == "__main__":
    unittest.main()
