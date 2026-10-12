"""Physical-reference stage propagation and owned native reduction snapshots."""
import copy
import itertools
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

LIBRARY_PATH = str(Path(sys.argv.pop(1)).resolve())
if '--installed' in sys.argv:
    sys.argv.remove('--installed')
else:
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'python'))
import rfmodel
from rfmodel.analysis_file import analyze_document
from rfmodel.model_file import _matrix, _encode
ROOT = Path(__file__).resolve().parents[1]


def series(z, a, b):
    d = z + a + b
    t = 2 * math.sqrt(complex(a).real * complex(b).real) / d
    return ((1-2*complex(a).real/d, t), (t, 1-2*complex(b).real/d))


def rc_network():
    return {'devices': [
        {'id': 'r', 'model': {'type': 'resistor', 'connection': 'series', 'resistance_ohms': 25},
         'reference_impedances_ohms': [[31, 12], [73, -21]]},
        {'id': 'c', 'model': {'type': 'capacitor', 'connection': 'shunt', 'capacitance_f': 1e-12},
         'reference_impedances_ohms': [[19, 8], [110, 31]]}],
        'connections': [[['r', 1], ['c', 0]]],
        'external_ports': [['r', 0], ['c', 1]]}


def rc_s(frequency, reference=50):
    # Independent ABCD multiplication: series 25 ohm followed by shunt j*w*C.
    y = 2j * math.pi * frequency * 1e-12
    d = 2 + 25/reference + y*(25+reference)
    return ((25/reference+y*(25-reference))/d, 2/d, (25/reference-y*(25+reference))/d)


def spectrum_document():
    return {'format': 'rfmodel.spectrum-chain', 'version': 1, 'spacing_hz': 1e9,
            'reference_ohms': 50, 'input': [{'bin': 0, 'amplitude': .2},
                                         {'bin': 1, 'amplitude': [.3, .1]},
                                         {'bin': 2, 'amplitude': [.1, -.03]}],
            'stages': [{'id': 'rc', 'type': 'linear_network', 'network': rc_network()}]}


class PowerWaveStageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lib = rfmodel.Library(LIBRARY_PATH)

    def near(self, a, b):
        self.assertLessEqual(abs(a-b), 3e-12 * max(1., abs(a), abs(b)))

    def tree_near(self, a, b):
        if isinstance(a, dict):
            self.assertEqual(set(a), set(b))
            for k in a: self.tree_near(a[k], b[k])
        elif isinstance(a, (tuple, list)):
            self.assertEqual(len(a), len(b))
            for x, y in zip(a, b): self.tree_near(x, y)
        elif isinstance(a, (float, complex)): self.near(a, b)
        else: self.assertEqual(a, b)

    def test_snapshot_lifetime_reference_and_reordering(self):
        with self.lib.network(75) as original:
            original.add_power_waves(series(50, 25, 100), [25, 100])
            before = original.external_s([0, 1])
            reduced = original.reduce_power_waves([1, 0], reference_ohms=75)
            self.assertEqual(original.external_s([0, 1]), before)
        with reduced:
            self.assertEqual(reduced.port_count, 2)
            self.assertEqual(reduced.reference_impedances_ohms, (75, 75))
            self.tree_near(reduced.external_s([0, 1]), ((.25, .75), (.75, .25)))
            self.tree_near(reduced.transmit_spectrum(1e9, {0: .2, 1: .3+.1j}, [0, 1]),
                           {0: .15+0j, 1: .225+.075j})
        with self.assertRaises(RuntimeError): reduced.reduce_power_waves([0, 1])

    def test_snapshot_coherence_and_mixing_identity(self):
        with self.lib.network() as n:
            n.add_power_waves(series(50, 31+12j, 110+31j), [31+12j, 110+31j])
            with self.assertRaises(rfmodel.RFModelError):
                n.transmit_spectrum(1e9, {1: .1}, [0, 1])
            with n.reduce_power_waves([0, 1]) as r:
                component = rfmodel.CoherentComponent(1, rfmodel.SpectrumKind.SOURCE, 1., 7, .3+.1j)
                result = r.transmit_coherent(1e9, [rfmodel.PortCoherentComponent(0, component)], [0, 1], 1)
                self.near(result.components[0].amplitude, (.3+.1j)*2/3)
                terms = [rfmodel.AmplifierMixingTerm(1, 1, (1,), .2),
                         rfmodel.AmplifierMixingTerm(3, 1, (-1, 1, 1), -.2),
                         rfmodel.AmplifierMixingTerm(2, 2, (1, 1), 0)]
                propagated = r.transmit_terms(1e9, terms, [0, 1])
                self.assertEqual(len(propagated), 3)
                for a, b in zip(propagated, terms):
                    self.assertEqual(a[:3], b[:3]); self.near(a.amplitude, b.amplitude*2/3)

    def test_snapshot_bad_inputs_and_bounded_iterables(self):
        with self.lib.network() as n:
            n.add([[0, 1], [1, 0]])
            for ports in ([], [0], [0, 0], [0, 99], [True, 1], [0., 1]):
                with self.subTest(ports=ports), self.assertRaises((ValueError, TypeError, rfmodel.RFModelError)):
                    n.reduce_power_waves(ports)
            for ref in (0, -1, True, float('nan'), float('inf'), 50+1j):
                with self.subTest(ref=ref), self.assertRaises((ValueError, TypeError, rfmodel.RFModelError)):
                    n.reduce_power_waves([0, 1], reference_ohms=ref)
            with self.assertRaises(ValueError): n.reduce_power_waves(itertools.repeat(0))
            self.assertEqual(n.external_s([0, 1]), ((0j, 1+0j), (1+0j, 0j)))

    def test_snapshot_python_adoption_failure_releases_handle(self):
        with self.lib.network() as n:
            n.add([[0]])
            destroy = self.lib._dll.rfmodel_network_destroy
            native = self.lib._dll.rfmodel_network_reduce_power_waves
            with patch.object(self.lib._dll, 'rfmodel_network_destroy', wraps=destroy) as release:
                with patch.object(self.lib._dll, 'rfmodel_network_reduce_power_waves', wraps=native) as create:
                    with patch.object(rfmodel.weakref, 'finalize', side_effect=MemoryError('injected')):
                        with self.assertRaises(MemoryError): n.reduce_power_waves([0])
                    create.assert_not_called()
                release.assert_not_called()
            for exception in (MemoryError, KeyboardInterrupt, SystemExit):
                def allocate_then_interrupt(*args):
                    self.assertEqual(native(*args), 0)
                    raise exception('after native allocation')
                with patch.object(self.lib._dll, 'rfmodel_network_destroy', wraps=destroy) as release:
                    with patch.object(self.lib._dll, 'rfmodel_network_reduce_power_waves', side_effect=allocate_then_interrupt):
                        with self.assertRaises(exception): n.reduce_power_waves([0])
                    # A successful C allocation followed by Python unwinding is
                    # released exactly once, including later object collection.
                    import gc
                    gc.collect()
                    self.assertEqual(release.call_count, 1)
            self.assertEqual(n.external_s([0]), ((0j,),))

    def test_spectrum_rc_matches_independent_abcd_and_preserves_input(self):
        doc = spectrum_document(); before = copy.deepcopy(doc)
        result = analyze_document(self.lib, doc)
        self.assertEqual(doc, before)
        source = {p['bin']: complex(*p['amplitude']) if isinstance(p['amplitude'], list) else p['amplitude'] for p in doc['input']}
        for row in result['stages'][0]['spectrum']:
            self.near(complex(*row['amplitude']), source[row['bin']]*rc_s(row['frequency_hz'])[1])
        self.assertEqual(result['reference_ohms'], 50)

    def test_spectrum_different_stage_reference(self):
        doc = spectrum_document(); doc['reference_ohms'] = 75
        result = analyze_document(self.lib, doc)
        self.near(complex(*result['stages'][0]['spectrum'][1]['amplitude']), (.3+.1j)*rc_s(1e9,75)[1])

    def test_coherent_two_sided_sources_and_external_order(self):
        network = rc_network(); network['external_ports'].reverse()
        doc = {'format': 'rfmodel.coherent-network', 'version': 1, 'spacing_hz': 1e9,
               'network': network, 'output_port': ['c', 1],
               'sources': [{'id': 'a', 'reference_clock': 'shared'}, {'id': 'b', 'reference_clock': 'shared'}],
               'inputs': [{'port': ['r', 0], 'component': {'source': 'a', 'bin': 1, 'bandwidth_hz': 1, 'amplitude': [.3, .1]}},
                          {'port': ['c', 1], 'component': {'source': 'b', 'bin': 1, 'bandwidth_hz': 1, 'amplitude': [.1, -.04]}}]}
        result = analyze_document(self.lib, doc)
        expected = rc_s(1e9)[1]*(.3+.1j)+rc_s(1e9)[2]*(.1-.04j)
        self.assertEqual(len(result['components']), 1)
        self.near(complex(*result['components'][0]['amplitude']), expected)
        self.near(result['total_power_w'], abs(expected)**2)

    def test_explicit_s_is_already_local_and_not_converted_twice(self):
        network = {'devices': [{'id': 'r', 's': _encode(series(50,31+12j,110+31j)),
                                'reference_impedances_ohms': [[31,12],[110,31]]}],
                   'external_ports': [['r',0],['r',1]]}
        doc = spectrum_document(); doc['stages'][0]['network'] = network
        result = analyze_document(self.lib, doc)
        self.near(complex(*result['stages'][0]['spectrum'][1]['amplitude']), (.3+.1j)*2/3)

    def test_multiport_shared_and_independent_sources(self):
        physical = tuple(tuple(2/3 - (r == c) for c in range(3)) for r in range(3))
        refs = [31+12j, 73-21j, 110+31j]
        local = self.lib.renormalize_power_waves(physical, [50]*3, refs).scattering
        with self.lib.network(75) as n:
            n.add_power_waves(local, refs)
            with n.reduce_power_waves([2, 0, 1]) as reduced:
                self.tree_near(reduced.external_s([0, 1, 2]), physical)
        for shared in (False, True):
            sources = [{'id':'a'}, {'id':'b'}]
            if shared:
                for source in sources: source['reference_clock']='same'
            doc = {'format':'rfmodel.coherent-network','version':1,'spacing_hz':1e9,
                   'network':{'devices':[{'id':'j','s':_encode(local),
                                          'reference_impedances_ohms':[[z.real,z.imag] for z in refs]}],
                              'external_ports':[['j',2],['j',0],['j',1]]},
                   'sources':sources, 'output_port':['j',2],
                   'inputs':[{'port':['j',0], 'component':{'source':'a','bin':1,'bandwidth_hz':1,'amplitude':[.3,.1]}},
                             {'port':['j',1], 'component':{'source':'b','bin':1,'bandwidth_hz':1,'amplitude':[.1,-.04]}}]}
            result = analyze_document(self.lib,doc)
            expected = (abs((.3+.1j)+(.1-.04j))**2 if shared else abs(.3+.1j)**2+abs(.1-.04j)**2)*4/9
            self.assertEqual(len(result['components']),1 if shared else 2)
            self.near(result['total_power_w'],expected)

    def test_generated_mixer_frequencies_recompute_network(self):
        doc = spectrum_document(); doc['input'] = [{'bin': 8, 'amplitude': [.02,.01]}]; doc['spacing_hz']=1e8
        doc['stages'].insert(0, {'id':'mix','type':'ideal_mixer','lo_bin':10,'lo_phase_radians':.3})
        result = analyze_document(self.lib, doc)
        input_rows = {row['bin']:complex(*row['amplitude']) for row in result['stages'][0]['spectrum']}
        self.assertEqual(set(input_rows), {2,18})
        for row in result['stages'][1]['spectrum']:
            self.near(complex(*row['amplitude']), input_rows[row['bin']]*rc_s(row['frequency_hz'])[1])

    def test_amplifier_post_stage_preserves_sources(self):
        doc = {'format':'rfmodel.amplifier-components','version':1,'spacing_hz':1e8,
               'input':[{'bin':10,'amplitude':.01},{'bin':11,'amplitude':[.008,.003]}],
               'parameters':{'power_gain_db':10,'output_p1db_dbm':20,'output_saturation_dbm':23,
                             'input_ip2_dbm':40,'input_ip3_dbm':30},'include_terms':True,
               'post_stages':[{'id':'rc','type':'linear_network','network':rc_network()}]}
        result = analyze_document(self.lib, doc)
        source = result['terms']; target = result['post_stages'][0]['terms']
        self.assertEqual(len(source),len(target))
        for a,b in zip(source,target):
            self.assertEqual((a['order'],a['bin'],a['contributors']),(b['order'],b['bin'],b['contributors']))
            self.near(complex(*b['amplitude']),complex(*a['amplitude'])*rc_s(a['frequency_hz'])[1])

    def test_expression_graph_and_highorder_keep_origins(self):
        for filename in ('coherent-recursive-polynomial.json','coherent-mixed-polynomial.json','coherent-highorder-amplifier.json'):
            with self.subTest(filename=filename):
                doc = json.loads((ROOT/'examples'/filename).read_text(encoding='utf-8'))
                previous = doc['outputs'][0]
                stage = {'id':'post','type':'linear_network','network':rc_network(),
                         'inputs':[{'stream':previous,'port':['r',0]}],
                         'outputs':[{'id':'filtered','port':['c',1]}]}
                doc['stages'].append(stage);doc['outputs']=['filtered']
                baseline = copy.deepcopy(doc)
                for device in baseline['stages'][-1]['network']['devices']:device.pop('reference_impedances_ohms')
                actual = analyze_document(self.lib,doc)
                expected = analyze_document(self.lib,baseline)
                self.tree_near(actual,expected)

    def test_empty_input_still_validates_mixed_topology_and_references(self):
        doc=spectrum_document();doc['input']=[]
        self.assertEqual(analyze_document(self.lib,doc)['stages'][0]['spectrum'],[])
        for field,value in [('reference_impedances_ohms',[50,0]),('reference_samples_ohms',[[50,50]])]:
            bad=copy.deepcopy(doc);bad['stages'][0]['network']['devices'][0][field]=value
            with self.assertRaises((ValueError,rfmodel.RFModelError)):analyze_document(self.lib,bad)

    def test_invalid_reference_descriptions(self):
        for value in (None,[],[50],[50,0],[True,50],[[50,float('inf')],50],[50,50,50]):
            doc=spectrum_document();doc['stages'][0]['network']['devices'][0]['reference_impedances_ohms']=value
            with self.subTest(value=value),self.assertRaises((ValueError,rfmodel.RFModelError)):
                analyze_document(self.lib,doc)

    def test_touchstone_parameter_coordinates_and_file_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'series.s2p'
            path.write_text('# Hz S RI R 50\n0 .3333333333333333 0 .6666666666666666 0 .6666666666666666 0 .3333333333333333 0\n2000000000 .3333333333333333 0 .6666666666666666 0 .6666666666666666 0 .3333333333333333 0\n',encoding='utf-8')
            doc=spectrum_document();doc['stages'][0]['network']={'devices':[{'id':'s','model':{'type':'touchstone','path':'series.s2p'},'reference_impedances_ohms':[[31,12],[110,31]]}], 'external_ports':[['s',0],['s',1]]}
            result=analyze_document(self.lib,doc,base_directory=Path(directory))
            self.near(complex(*result['stages'][0]['spectrum'][1]['amplitude']),(.3+.1j)*2/3)

    def test_cli_example_and_failure_preserves_existing_output(self):
        with tempfile.TemporaryDirectory() as directory:
            model,output=Path(directory)/'input.json',Path(directory)/'result.json'
            model.write_text(json.dumps(spectrum_document()),encoding='utf-8')
            env=dict(os.environ,PYTHONPATH=str(Path(rfmodel.__file__).resolve().parents[1]))
            command=[sys.executable,'-m','rfmodel',str(model),'--library',LIBRARY_PATH,'--output',str(output)]
            run=subprocess.run(command,env=env,capture_output=True,text=True)
            self.assertEqual(run.returncode,0,run.stderr);saved=output.read_bytes()
            bad=spectrum_document();bad['stages'][0]['network']['devices'][0]['reference_impedances_ohms']=[0,50]
            model.write_text(json.dumps(bad),encoding='utf-8')
            self.assertNotEqual(subprocess.run(command,env=env,capture_output=True).returncode,0)
            self.assertEqual(output.read_bytes(),saved)


if __name__=='__main__':unittest.main()
