import importlib.util
import struct
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).parents[1] / "tools" / "paimage_trace_analyze.py"
SPEC = importlib.util.spec_from_file_location("paimage_trace_analyze", SCRIPT)
ANALYZER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ANALYZER)


def block(kind, body):
    size = 12 + len(body)
    return struct.pack("<II", kind, size) + body + struct.pack("<I", size)


def packet_block(timestamp_us, packet):
    padded = packet + bytes((-len(packet)) % 4)
    body = struct.pack("<IIIII", 0, timestamp_us >> 32, timestamp_us & 0xFFFFFFFF,
                       len(packet), len(packet)) + padded
    return block(6, body)


def ethernet_udp(packet_id=7, trigger_id=42):
    payload = struct.pack("<HH", packet_id, trigger_id)
    udp = struct.pack("!HHHH", 4321, 8001, 8 + len(payload), 0) + payload
    ip = bytearray(20)
    ip[0] = 0x45
    struct.pack_into("!H", ip, 2, len(ip) + len(udp))
    ip[8] = 64
    ip[9] = 17
    ip[12:16] = bytes((192, 168, 0, 2))
    ip[16:20] = bytes((192, 168, 0, 10))
    return bytes(12) + struct.pack("!H", 0x0800) + bytes(ip) + udp


class PcapngChecks(unittest.TestCase):
    def test_network_order_protocol_order_and_cross_round_dedup(self):
        section = block(0x0A0D0D0A, struct.pack("<IHHq", 0x1A2B3C4D, 1, 0, -1))
        options = struct.pack("<HHB3x", 9, 1, 6) + struct.pack("<HH", 0, 0)
        interface = block(1, struct.pack("<HHI", 1, 0, 65535) + options)
        frame = ethernet_udp()
        # The second observation is the same datagram seen by another pktmon
        # component. The third is the same 16-bit IDs in a later round.
        content = section + interface + packet_block(1_000_000, frame) \
            + packet_block(1_000_500, frame) + packet_block(2_000_000, frame)
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "sample.pcapng"
            path.write_bytes(content)
            packets = ANALYZER._pcapng_packets(path)
        self.assertEqual(len(packets), 2)
        self.assertEqual(packets[0]["src"], "192.168.0.2")
        self.assertEqual(packets[0]["dport"], 8001)
        self.assertEqual(packets[0]["packet"], 7)
        self.assertEqual(packets[0]["trigger"], 42)
        self.assertAlmostEqual(packets[0]["tsMs"], 1000.0)
        self.assertAlmostEqual(packets[1]["tsMs"], 2000.0)


if __name__ == "__main__":
    unittest.main()
