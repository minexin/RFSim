import copy
import importlib.util
import json
from pathlib import Path
import unittest
HERE=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location("near_zero",HERE/"identify-attenuator-near-zero.py")
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)


class NearZeroTests(unittest.TestCase):
    def setUp(self):
        self.e=json.loads((HERE.parents[1]/"validation/systemvue-2023-attenuator-near-zero-evidence.json").read_text(encoding="utf-8"))
    def test_reconstruction_and_rejected_hypothesis(self):
        r=m.analyze(self.e)
        self.assertTrue(r["reconstruction_passed"])
        h=r["preregistered_hypotheses_consistent_with_two_new_unity_patterns"]
        self.assertFalse(h["hypothesis_round_to_unity_below_1e_6"])
        self.assertFalse(h["hypothesis_no_threshold"])
        self.assertTrue(h["hypothesis_db_threshold_1e_5"])
    def test_reported_unity_is_not_physical_bypass(self):
        r=m.analyze(self.e)
        for c in r["cases"][-2:]:
            self.assertEqual(c["reported_gain"],1)
            self.assertLess(float(c["signal_transfer"]),1)
            self.assertGreater(float(c["endpoints"][0]["device_noise_raw"]),0)
    def test_fixed_normalization_is_rejected_at_smallest_loss(self):
        c=self.e["cases"][1]
        total=next(t for t in c["spectral_terms"] if t["role"]=="total_out" and t["frequency_hz"]==99999999.5)
        c["measurements"]["CND"][1]=total["power"]*(1-5e-9)
        self.assertFalse(m.analyze(self.e)["reconstruction_passed"])
    def test_wrong_z_breaks_power_relation(self):
        self.e["cases"][-1]["spectral_terms"][-1]["impedance"][0]=51
        self.assertFalse(m.analyze(self.e)["reconstruction_passed"])
    def test_stale_dataset_rejected(self):
        self.e["cases"][-1]["dataset_timestamp"]=self.e["cases"][0]["dataset_timestamp"]
        with self.assertRaisesRegex(ValueError,"Stale"):m.analyze(self.e)
    def test_restoration_required(self):
        self.e["cases"][-1]["restored"]["Sch1/PartList/Attn/ParamSet/L"]["data_entry"]="6E-06"
        with self.assertRaisesRegex(ValueError,"Restoration"):m.analyze(self.e)
    def test_missing_component_not_called_physical_zero(self):
        r=m.analyze(self.e)
        endpoint=r["cases"][1]["endpoints"][0]
        self.assertFalse(endpoint["device_exported"])
        self.assertIsNone(endpoint["device_noise_raw"])
    def test_incorrect_actual_loss_rejected(self):
        self.e["cases"][-1]["parameters"]["Sch1/PartList/Attn/ParamSet/L"]["data"]=1.1
        with self.assertRaisesRegex(ValueError,"actual loss"):m.analyze(self.e)


if __name__=="__main__":
    unittest.main()
