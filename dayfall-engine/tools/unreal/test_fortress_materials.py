"""Tests for fortress_materials.py: python tools/unreal/test_fortress_materials.py"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fortress_materials as F  # noqa: E402

FORT = r'''
import json, os
import unreal
LOOKS = {
    "FT_Sand": ((0.40, 0.40, 0.40), 0.88, 0.0, None, 0.0, 0.10),
    "FT_Glass": ((0.30, 0.17, 0.06), 0.3, 0.0, (1.0, 0.52, 0.16), 0.45, 0.0),
    "FT_Rock": ((0.36, 0.35, 0.34), 0.92, 0.0, None, 0.0, 0.2),
    "FT_CragRock": ((0.56, 0.55, 0.51), 0.92, 0.0, None, 0.0, 0.2),
    "FT_Iron": ((0.030, 0.030, 0.034), 0.45, 0.9, None, 0.0, 0.05),
}
DETAIL = {
    "FT_Sand": ("FT:Ashlar", 5.5, 1.0, 0.4, 0.9, 0.0),
    "FT_Glass": ("FT:Leaded", 0.9, 1.0, 0.5, 0.3, 0.0),
    "FT_Rock": ("Stone_1", 14.0, 1.0, 0.4, 0.9, 0.0),
}
PAINT = json.loads(os.environ.get("FORT_PAINT", '{"contact": 0.5, "top": 0.1, "warm": 0.6, "strokes": 0.07}'))
EMISSIVE_DETAIL = {"FT_Glass": 1.0}
WEATHER = {"FT_Sand": (0.55, 0.45)}
'''
TUNE = r'''
CFG = {"SUN": {"intensity": 9}, "MI": {"FT_Sand": [0.215, 0.203, 0.198]}, "SKY": some_call()}
'''


class FortressMaterialsTest(unittest.TestCase):
    def setUp(self):
        self.m = F.fortress_materials(F.read_tables(FORT, TUNE), "textures")

    def test_textured_triplanar(self):
        s = self.m["FT_Sand"]
        self.assertEqual(s["base_color"], [0.215, 0.203, 0.198])      # tune_fortress_look override
        self.assertEqual(s["textures"], {"base": "textures/T_FT_Ashlar_A.jpg", "normal": "textures/T_FT_Ashlar_N.png"})
        self.assertEqual(s["normal_convention"], "directx")
        t = s["triplanar"]
        self.assertEqual((t["tile_m"], t["chroma_mix"], t["vertex_color_scale"]), (5.5, 0.4, 1.6))
        self.assertEqual((t["streaks"], t["moss"], t["contact_dark"]), (0.55, 0.45, 0.5))   # WEATHER, PAINT default

    def test_emissive_detail(self):
        g = self.m["FT_Glass"]
        self.assertEqual((g["emissive"], g["emissive_strength"]), ([1.0, 0.52, 0.16], 0.45))
        self.assertEqual(g["triplanar"]["emissive_detail"], 1.0)

    def test_untextured(self):
        self.assertNotIn("textures", self.m["FT_Rock"])               # third-party Stone_1 set: no texture
        self.assertEqual(self.m["FT_Rock"]["triplanar"]["tile_m"], 14.0)
        self.assertNotIn("tile_m", self.m["FT_Iron"]["triplanar"])
        self.assertEqual(self.m["FT_Iron"]["metallic"], 0.9)
        self.assertEqual(self.m["FT_CragRock"]["triplanar"]["ignore_vertex_color"], 1.0)

    def test_missing_table(self):
        with self.assertRaises(ValueError):
            F.read_tables("LOOKS = {}\n")


if __name__ == "__main__":
    unittest.main()
