"""Selected high-order native rules, JSON expansion, feedback and installed CLI."""
import cmath
import copy
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

LIBRARY_PATH = str(Path(sys.argv.pop(1)).resolve())
ROOT = Path(__file__).resolve().parents[1]
INSTALLED = '--installed' in sys.argv
if INSTALLED:
    sys.argv.remove('--installed')
else:
    sys.path.insert(0, str(ROOT/'python'))
from rfmodel import Library, RFModelError
from rfmodel.conversion_network_file import analyze_conversion_network


def number(value):
    return complex(*value) if isinstance(value, list) else complex(value)


def key(channel):
    return channel['device'], channel['port'], channel['bin']


def rule(generating=(1,), coefficients=(0, 0, 0, .1)):
    return dict(type='highorder', input_port=0, output_port=1,
                voltage_coefficients=list(coefficients), generating_bins=list(generating))


class HighOrderFrequencyPlanTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lib = Library(LIBRARY_PATH)

    def example(self):
        return json.loads((ROOT/'examples/auto-frequency-highorder-feedback.json').read_text(encoding='utf-8-sig'))

    def analyze(self, document):
        before = copy.deepcopy(document)
        result = analyze_conversion_network(self.lib, document)
        self.assertEqual(before, document)
        return result

    def equivalent(self, a, b):
        aa = {key(ch): i for i,ch in enumerate(a['channels'])}
        bb = {key(ch): i for i,ch in enumerate(b['channels'])}
        self.assertEqual(aa.keys(), bb.keys())
        for channel, i in aa.items():
            j = bb[channel]
            for field in ('incident','outgoing'):
                self.assertLess(abs(number(a['channels'][i][field])-number(b['channels'][j][field])),1e-11)
            for other, k in aa.items():
                l = bb[other]
                for field in ('noise_covariance_w_per_hz','noise_complementary_w_per_hz'):
                    x,y = number(a[field][i][k]),number(b[field][j][l])
                    self.assertLessEqual(abs(x-y),1e-22+1e-9*abs(y))

    def test_native_late_selection_and_mixed_rules(self):
        devices=[dict(ports=[0,1],rules=[rule((1,2),(0,0,.1,.2))]),
                 dict(ports=[0,1],seeds=[(0,13),(0,2),(0,0),(0,1)],
                      rules=[dict(type='copy',input_port=0,output_port=1)])]
        plan=self.lib.plan_conversion_frequencies(1,devices,[((0,0),(1,1))])
        self.assertEqual(plan.channels[0],tuple((0,b) for b in (0,1,2,13))+
                          tuple((1,b) for b in (1,2,3,4,5,6,13)))
        self.assertEqual(plan.seed_channel_indices[1],(3,2,0,1))
        self.assertEqual((plan.total_channels,plan.generated_channels),(19,15))
        # Ordinary and supplemental rules coexist on one device.
        mixed=self.lib.plan_conversion_frequencies(1,[dict(ports=[0,1,2],seeds=[(0,1)],
              rules=[rule(),dict(type='copy',input_port=1,output_port=2)])])
        self.assertEqual(mixed.channels,(((0,1),(1,1),(1,3),(2,1),(2,3)),))

    def test_empty_selection_zero_coefficients_and_eleventh_order(self):
        for selected,coefficients in [((),(0,0,.1)),((1,),(0,))]:
            plan=self.lib.plan_conversion_frequencies(1,[dict(ports=[0,1],seeds=[(0,0),(0,1),(0,13)],
                   rules=[rule(selected,coefficients)])])
            self.assertEqual(plan.channels,(((0,0),(0,1),(0,13),(1,1),(1,13)),))
        plan=self.lib.plan_conversion_frequencies(1,[dict(ports=[0,1],seeds=[(0,2),(0,13)],
             rules=[rule((2,),[0]*11+[.1])])])
        self.assertEqual(tuple(b for p,b in plan.channels[0] if p==1),(2,6,10,13,14,18,22))

    def test_json_feedback_matches_full_manual_grid_and_independent_scalar_reference(self):
        document=self.example()
        result=self.analyze(document)
        self.assertEqual((result['frequency_plan']['total_channels'],result['frequency_plan']['generated_channels']),(8,6))
        self.assertEqual(result['frequency_plan']['generated_boundary_channels'],[])
        manual=copy.deepcopy(document)
        del manual['frequency_plan']
        manual['devices'][0]['channels']=[dict(port=p,bin=b) for p,b in [(1,3),(0,1),(1,1),(0,3)]]
        manual['devices'][1]['bins']=[1,3]
        manual['devices'][1]['output_offset']=[0,.03,0,0]
        self.equivalent(result,self.analyze(manual))
        def gains(power):
            radius=math.sqrt(power); q=10**(-.05); p1=math.sqrt(10**(-2.9))
            head=math.sqrt(10**(-.7))-math.sqrt(.1)
            g=10*(1-(1-q)*power/p1**2) if radius<=p1 else (math.sqrt(.1)+head*math.tanh(10*(3*q-2)*(radius-p1)/head))/radius
            knee,limit=math.sqrt(10**(-3.4)),math.sqrt(10**(-2.8))
            scale=1 if radius<=knee else (knee+(limit-knee)*math.tanh((radius-knee)/(limit-knee)))/radius
            return g,scale
        u,v=.03,0
        for _ in range(200):
            g,scale=gains(u*u+v*v); limited=scale*u
            uu=.03-.02*(g*u+7.5*limited**3); vv=-.02*(g*v+2.5*limited**3)
            done=max(abs(uu-u),abs(vv-v))<1e-15
            u,v=uu,vv
            if done: break
        self.assertTrue(done)
        channels={key(ch):ch for ch in result['channels']}
        self.assertLess(abs(number(channels['highorder',0,1]['incident'])-u),1e-11)
        self.assertLess(abs(number(channels['highorder',0,3]['incident'])-v),1e-11)
        g,scale=gains(u*u+v*v)
        for b in (1,3):
            projected=0j
            for sample in range(128):
                t=2*math.pi*sample/128
                voltage=10*scale*u*math.cos(t)
                wave=math.sqrt(2)*g*(u*math.cos(t)+v*math.cos(3*t))+.1*voltage**3/math.sqrt(50)
                projected+=wave*cmath.exp(-1j*b*t)*math.sqrt(2)/128
            self.assertLess(abs(number(channels['highorder',1,b]['outgoing'])-projected),1e-11)

    def test_seed_statistics_initial_waves_offsets_and_am_pm_remapped(self):
        model=dict(type='highorder_amplifier',power_gain_db=20,output_p1db_dbm=20,
                   output_saturation_dbm=23,nonlinear_voltage_coefficients=[0,.1],generating_bins=[1])
        doc=dict(format='rfmodel.conversion-network',version=1,spacing_hz=1,
                 frequency_plan=dict(new_channel_boundary=dict(reflection=.2)),
                 devices=[dict(id='h',channels=[dict(port=p,bin=b) for p,b in [(1,3),(0,13),(0,1)]],
                  model=model,noise=dict(covariance=[[1e-9,0,0],[0,0,0],[0,0,0]],complementary=[[[1e-10,2e-10],0,0],[0,0,0],[0,0,0]]),
                  source_noise=dict(covariance=[[0,0,0],[0,2e-10,[1e-11,2e-11]],[0,[1e-11,-2e-11],3e-10]],
                                    complementary=[[0,0,0],[0,1e-11,2e-12],[0,2e-12,-1e-11]]),
                  output_offset=[[.001,.002],0,0],am_pm=dict(peak_degrees_per_db=-5,output_reference_dbm=10,output_channels=[0]))],
                 boundaries=[dict(channel=['h',1,3]),dict(channel=['h',0,13],source=[.02,.01]),dict(channel=['h',0,1],source=.01)],
                 additional_source_noise=dict(covariance=[[0,0,0],[0,1e-11,0],[0,0,2e-11]]),
                 operating_point=dict(initial_incident=[0,[.02,.01],.01]),loaded_noise=True)
        actual=self.analyze(doc)
        self.assertEqual(actual['frequency_plan']['devices'][0]['seed_channel_indices'],[3,1,0])
        manual=copy.deepcopy(doc);del manual['frequency_plan']
        entry=manual['devices'][0]
        entry['channels']=[dict(port=p,bin=b) for p,b in [(0,1),(0,13),(1,1),(1,3),(1,13)]]
        # Independent known permutation; do not call production expansion helpers.
        mapping=[3,1,0]
        def matrix(old):
            out=[[0]*5 for _ in range(5)]
            for i,ii in enumerate(mapping):
                for j,jj in enumerate(mapping): out[ii][jj]=old[i][j]
            return out
        for name in ('noise','source_noise'):
            entry[name]={k:matrix(v) for k,v in entry[name].items()}
        manual['additional_source_noise']={k:matrix(v) for k,v in manual['additional_source_noise'].items()}
        entry['output_offset']=[0,0,0,[.001,.002],0]
        entry['am_pm']['output_channels']=[3]
        manual['operating_point']['initial_incident']=[.01,[.02,.01],0,0,0]
        manual['boundaries'].extend([dict(channel=['h',1,b],reflection=.2) for b in (1,13)])
        self.equivalent(actual,self.analyze(manual))

    def test_strict_native_parameters_and_resource_failures(self):
        base=dict(ports=[0,1],seeds=[(0,1)],rules=[rule()])
        for changes in [dict(generating_bins=[True]),dict(generating_bins=[0]),dict(generating_bins=[-1]),
                        dict(generating_bins=[1,1]),dict(generating_bins=[2]),dict(generating_bins=[1.5]),
                        dict(voltage_coefficients=[1,0,.1]),dict(voltage_coefficients=[0,1,.1]),
                        dict(voltage_coefficients=[0,0,float('nan')]),dict(voltage_coefficients=[]),
                        dict(voltage_coefficients=[0]*13),dict(extra=1),dict(generating_bins=[1]*513),
                        dict(input_port=True)]:
            device=copy.deepcopy(base);device['rules'][0].update(changes)
            with self.subTest(changes=changes),self.assertRaises((ValueError,TypeError,RFModelError)):
                self.lib.plan_conversion_frequencies(1,[device])
        for options in [dict(max_bin=2),dict(max_total_channels=2),dict(max_transitions=1)]:
            with self.assertRaises(RFModelError):self.lib.plan_conversion_frequencies(1,[base],**options)
        dense=dict(ports=[0,1],seeds=[(0,b) for b in range(1,129)],rules=[rule(range(1,129),(0,0,1,1))])
        with self.assertRaisesRegex(RFModelError,'internal polynomial'):
            self.lib.plan_conversion_frequencies(1,[dense])

    def test_json_missing_selection_and_invalid_model_rejected(self):
        for update in [dict(generating_bins=[2]),dict(generating_bins=[1,1]),dict(generating_bins=[True]),
                       dict(nonlinear_voltage_coefficients=[True]),dict(power_gain_db=True),dict(unknown=1)]:
            doc=self.example();doc['devices'][0]['model'].update(update)
            with self.subTest(update=update),self.assertRaises((ValueError,TypeError,RFModelError)):
                self.analyze(doc)
        doc=self.example();del doc['operating_point']
        with self.assertRaisesRegex(ValueError,'operating_point'):self.analyze(doc)

    def test_json_remapped_physical_ports_and_explicit_dc(self):
        doc=self.example();entry=doc['devices'][0]
        entry['model'].update(input_port=7,output_port=9)
        doc['connections']=[[['highorder',9],['feedback',0]],[['highorder',7],['feedback',1]]]
        result=self.analyze(doc)
        self.assertEqual({ch['port'] for ch in result['channels'] if ch['device']=='highorder'},{7,9})
        single=dict(format='rfmodel.conversion-network',version=1,spacing_hz=1,
          frequency_plan=dict(new_channel_boundary=dict(reflection=0)),operating_point={},
          devices=[dict(id='h',channels=[dict(port=0,bin=1),dict(port=0,bin=0),dict(port=1,bin=0)],
             model=dict(type='highorder_amplifier',power_gain_db=20,output_p1db_dbm=20,output_saturation_dbm=23,
                        nonlinear_voltage_coefficients=[.1],generating_bins=[1]),noise=dict(noiseless=True))],
          boundaries=[dict(channel=['h',0,1],source=.01),dict(channel=['h',0,0]),dict(channel=['h',1,0])])
        dc=self.analyze(single)
        self.assertEqual(number(next(c for c in dc['channels'] if key(c)==('h',1,0))['outgoing']),0)
        single['boundaries'][1]['source']=.001
        with self.assertRaises(RFModelError):self.analyze(single)

    def test_installed_cli_and_failure_preserves_output(self):
        with tempfile.TemporaryDirectory() as folder:
            source,output=Path(folder)/'input.json',Path(folder)/'output.json'
            doc=self.example();source.write_text(json.dumps(doc),encoding='utf-8')
            env=dict(os.environ)
            if not INSTALLED:env['PYTHONPATH']=str(ROOT/'python')
            args=[sys.executable,'-m','rfmodel',str(source),'--library',LIBRARY_PATH,'--output',str(output)]
            success=subprocess.run(args,capture_output=True,text=True,env=env)
            self.assertEqual(success.returncode,0,success.stderr)
            saved=output.read_bytes();self.assertEqual(json.loads(saved)['frequency_plan']['total_channels'],8)
            doc['devices'][0]['model']['generating_bins']=[2];source.write_text(json.dumps(doc),encoding='utf-8')
            failed=subprocess.run(args,capture_output=True,text=True,env=env)
            self.assertNotEqual(failed.returncode,0);self.assertEqual(output.read_bytes(),saved)


if __name__=='__main__':
    unittest.main()
