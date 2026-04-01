#!/usr/bin/env python3
"""Unit tests for gen_sdf.py"""

import sys
import os
import unittest
import tempfile
import textwrap

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_sdf import validate, emit_sdf, alloc_channel_ids, load_topology

class TestValidation(unittest.TestCase):

    def _base(self):
        return {
            "pds": [
                {"name": "ctrl", "priority": 50},
                {"name": "server", "priority": 200, "passive": True},
            ],
            "channels": [],
            "memory_regions": [],
        }

    def test_valid_minimal(self):
        errors = validate(self._base())
        self.assertEqual(errors, [])

    def test_duplicate_pd_name(self):
        t = self._base()
        t["pds"].append({"name": "ctrl", "priority": 80})
        errors = validate(t)
        self.assertTrue(any("Duplicate PD name" in e for e in errors))

    def test_missing_pd_priority(self):
        t = self._base()
        t["pds"].append({"name": "orphan"})
        errors = validate(t)
        self.assertTrue(any("priority" in e for e in errors))

    def test_priority_out_of_range(self):
        t = self._base()
        t["pds"].append({"name": "high", "priority": 255})
        errors = validate(t)
        self.assertTrue(any("out of range" in e for e in errors))

    def test_channel_id_overflow(self):
        t = self._base()
        t["channels"].append({
            "name": "overflow", "pd_a": "ctrl", "id_a": 63,
            "pd_b": "server", "id_b": 0,
        })
        errors = validate(t)
        self.assertTrue(any("out of range" in e for e in errors))

    def test_channel_id_collision(self):
        t = self._base()
        t["channels"] += [
            {"name": "ch1", "pd_a": "ctrl", "id_a": 0, "pd_b": "server", "id_b": 0},
            {"name": "ch2", "pd_a": "ctrl", "id_a": 0, "pd_b": "server", "id_b": 1},
        ]
        errors = validate(t)
        self.assertTrue(any("collision" in e for e in errors))

    def test_unknown_pd_in_channel(self):
        t = self._base()
        t["channels"].append({
            "name": "ghost", "pd_a": "ctrl", "id_a": 0,
            "pd_b": "nobody", "id_b": 0,
        })
        errors = validate(t)
        self.assertTrue(any("unknown PD" in e for e in errors))

    def test_unknown_mr_in_map(self):
        t = self._base()
        t["pds"][0]["maps"] = [{"mr": "ghost_mr", "vaddr": 0x1000, "perms": "rw"}]
        errors = validate(t)
        self.assertTrue(any("unknown MR" in e for e in errors))

    def test_valid_channel_max_id(self):
        t = self._base()
        t["channels"].append({
            "name": "max_id", "pd_a": "ctrl", "id_a": 62,
            "pd_b": "server", "id_b": 62,
        })
        errors = validate(t)
        self.assertEqual(errors, [])

    def test_valid_mr_map(self):
        t = self._base()
        t["memory_regions"].append({"name": "shared", "size": 0x1000})
        t["pds"][0]["maps"] = [{"mr": "shared", "vaddr": 0x5000000, "perms": "rw"}]
        errors = validate(t)
        self.assertEqual(errors, [])

    def test_invalid_perms(self):
        t = self._base()
        t["memory_regions"].append({"name": "mr1", "size": 0x1000})
        t["pds"][0]["maps"] = [{"mr": "mr1", "vaddr": 0x5000000, "perms": "zz"}]
        errors = validate(t)
        self.assertTrue(any("perms" in e for e in errors))


class TestEmit(unittest.TestCase):

    def _topo(self):
        return {
            "pds": [
                {"name": "ctrl", "priority": 50, "program_image": "ctrl.elf"},
                {"name": "srv",  "priority": 200, "passive": True,
                 "program_image": "srv.elf"},
            ],
            "channels": [
                {"name": "c0", "pd_a": "ctrl", "id_a": 0, "pp_a": True,
                 "pd_b": "srv", "id_b": 0},
            ],
            "memory_regions": [
                {"name": "shared", "size": 0x1000},
            ],
        }

    def test_emit_contains_pds(self):
        sdf = emit_sdf(self._topo())
        self.assertIn('<protection_domain name="ctrl"', sdf)
        self.assertIn('<protection_domain name="srv"', sdf)
        self.assertIn('passive="true"', sdf)

    def test_emit_contains_channel(self):
        sdf = emit_sdf(self._topo())
        self.assertIn('<channel>', sdf)
        self.assertIn('pp="true"', sdf)
        self.assertIn('id="0"', sdf)

    def test_emit_contains_mr(self):
        sdf = emit_sdf(self._topo())
        self.assertIn('<memory_region', sdf)
        self.assertIn('name="shared"', sdf)

    def test_emit_channel_table_in_comment(self):
        sdf = emit_sdf(self._topo())
        self.assertIn("Channel ID allocation table", sdf)
        self.assertIn("c0", sdf)

    def test_emit_valid_xml(self):
        import xml.etree.ElementTree as ET
        sdf = emit_sdf(self._topo())
        # Should parse without exception
        ET.fromstring(sdf)


class TestAutoAlloc(unittest.TestCase):

    def test_auto_ids_assigned(self):
        topo = {
            "pds": [{"name": "a", "priority": 10}, {"name": "b", "priority": 20}],
            "channels": [
                {"name": "c", "pd_a": "a", "id_a": "auto",
                 "pd_b": "b", "id_b": "auto"},
            ],
            "memory_regions": [],
        }
        result = alloc_channel_ids(topo)
        ch = result["channels"][0]
        self.assertIsInstance(ch["id_a"], int)
        self.assertIsInstance(ch["id_b"], int)
        self.assertGreaterEqual(ch["id_a"], 0)
        self.assertGreaterEqual(ch["id_b"], 0)

    def test_auto_ids_no_collision(self):
        topo = {
            "pds": [{"name": "a", "priority": 10}],
            "channels": [
                {"name": "c1", "pd_a": "a", "id_a": 0, "pd_b": "a", "id_b": "auto"},
                {"name": "c2", "pd_a": "a", "id_a": "auto", "pd_b": "a", "id_b": "auto"},
            ],
            "memory_regions": [],
        }
        result = alloc_channel_ids(topo)
        ids = [ch["id_a"] for ch in result["channels"]] + \
              [ch["id_b"] for ch in result["channels"]]
        # All assigned IDs on PD 'a' should be distinct
        a_ids = [result["channels"][0]["id_a"],
                 result["channels"][0]["id_b"],
                 result["channels"][1]["id_a"],
                 result["channels"][1]["id_b"]]
        self.assertEqual(len(set(a_ids)), len(a_ids))


class TestLoadTopology(unittest.TestCase):

    def test_load_real_topology(self):
        path = os.path.join(os.path.dirname(__file__), "topology.yaml")
        if not os.path.exists(path):
            self.skipTest("topology.yaml not found")
        topo = load_topology(path)
        self.assertIn("pds", topo)
        self.assertIn("channels", topo)
        errors = validate(topo)
        self.assertEqual(errors, [], f"topology.yaml has validation errors: {errors}")


if __name__ == "__main__":
    unittest.main(verbosity=2)
