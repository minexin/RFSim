import copy
import importlib.util
import json
from pathlib import Path
import unittest

HERE=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location("identify",HERE/"identify-attenuator-noise.py")
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)


class NoiseIdentificationTests(unittest.TestCase):
    def setUp(self):
        self.e=json.loads((HERE.parents[1]/"validation/systemvue-2023-attenuator-noise-identification-evidence.json").read_text(encoding="utf-8"))
    def test_original_failure_and_heldout_prediction(self):
        r=m.analyze(self.e)
        self.assertTrue(r["diagnostic_passed"])
        self.assertFalse(r["vendor_noise_subset_passed"])
        self.assertTrue(r["slope_candidates"]["1.3806504E-23"]["passed"])
        self.assertFalse(r["slope_candidates"]["1.3806503E-23"]["passed"])
        self.assertFalse(r["cases"][-1]["source_corrected_only_comparison"]["passed"])
    def test_source_contamination_fails(self):
        self.e["cases"][-1]["spectral_terms"][2]["power_field"]*=1.001
        self.assertFalse(m.analyze(self.e)["diagnostic_passed"])
    def test_stale_dataset_rejected(self):
        self.e["cases"][-1]["dataset_timestamp"]=self.e["cases"][0]["dataset_timestamp"]
        with self.assertRaisesRegex(ValueError,"timestamp"):m.analyze(self.e)
    def test_wrong_effective_temperature_rejected(self):
        self.e["cases"][-1]["effective_device_temperature_c"]=[16.85]
        with self.assertRaisesRegex(ValueError,"temperature"):m.analyze(self.e)
    def test_restoration_not_assumed(self):
        self.e["cases"][-1]["restored_parameters"]["Sch1/PartList/Attn/ParamSet/Ta"]["data_entry"]="76.85"
        with self.assertRaisesRegex(ValueError,"Restoration"):m.analyze(self.e)
    def test_retrospective_prediction_rejected(self):
        self.e["preregistered_predictions"]["registered_utc"]="2027-01-01T00:00:00+00:00"
        with self.assertRaisesRegex(ValueError,"registered"):m.analyze(self.e)


if __name__=="__main__":
    unittest.main()
