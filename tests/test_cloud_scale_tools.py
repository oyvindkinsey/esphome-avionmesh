"""Pure fixture tests; HTTPS roundtrips run via cloud_scale_report.py."""
import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from cloud_scale import make_inventory as upstream
from cloud_scale_fixtures import make_inventory, manifest


class Fixtures(unittest.TestCase):
    def test_upstream_generator_compatibility(self):
        expected_devices, expected_groups = upstream(76, 16, 7)
        devices, groups = make_inventory(76, 16, 7)
        for device in devices:
            del device["metadata"]
        self.assertEqual((devices, groups), (expected_devices, expected_groups))

    def test_deterministic_minimum_payload(self):
        fixture = make_inventory(76, 16)
        result = manifest(*fixture)
        self.assertEqual(result, manifest(*make_inventory(76, 16)))
        self.assertEqual((result["devices"], result["groups"], result["memberships"]), (76, 16, 64))
        self.assertGreater(result["inventory_bytes"], 45000)
        self.assertLess(result["inventory_bytes"], 49000)

    def test_sparse_dense(self):
        self.assertEqual(manifest(*make_inventory(200, 40, members=1))["memberships"], 40)
        self.assertEqual(manifest(*make_inventory(200, 40, members=200))["memberships"], 8000)


if __name__ == "__main__":
    unittest.main()
