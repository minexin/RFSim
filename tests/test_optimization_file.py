"""Declarative design tasks through real native optimizers and RF analyses."""
import copy
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

LIBRARY_PATH = Path(sys.argv.pop(1)).resolve()
ROOT = Path(__file__).resolve().parents[1]
INSTALLED = '--installed' in sys.argv
if INSTALLED:
    sys.argv.remove('--installed')
else:
    sys.path.insert(0, str(ROOT/'python'))
from rfmodel import Library, RFModelError
from rfmodel.analysis_file import analyze_document, analysis_operation
from rfmodel.model_file import load, referenced_touchstone_paths
from rfmodel.optimization_file import analyze_optimization


def fixture(name='lc-match'):
    return load(ROOT/f'examples/optimize-{name}.json')


def scalar_task():
    return dict(format='rfmodel.optimization', version=1,
        analysis=dict(format='rfmodel.linear-network', version=1, frequencies_hz=[1e9],
            devices=[dict(id='gain',model=dict(type='linear_amplifier',gain_db=0.,gain_phase_degrees=90.))],
            external_ports=[['gain',0],['gain',1]]),
        parameters=[dict(id='phase',lower=90.,upper=90.,initial=90.,
            bindings=[dict(path=['devices',{'id':'gain'},'model','gain_phase_degrees'])])],
        objectives=[dict(id='value',path=['samples',0,'s',1,0],component='power',goal='minimize')])


class OptimizationTaskTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.library = Library(LIBRARY_PATH)

    def evaluate(self, document, **kwargs):
        return analyze_optimization(self.library, document, **kwargs)

    def test_lc_known_minimum_and_complete_trace(self):
        document = fixture()
        before = copy.deepcopy(document)
        result = self.evaluate(document)
        self.assertEqual(document, before)
        native = result['result']
        self.assertEqual(native['stop'], 'target_reached')
        self.assertEqual(native['evaluations'], len(native['history']))
        self.assertLessEqual(native['value'], 1e-14)
        omega = 2*math.pi*1e9
        for actual, reference in zip(native['parameters'], [25/omega,.02/omega]):
            self.assertLess(abs(actual/reference-1), 1e-6)
        for entry in native['history']:
            l,c = entry['parameters']
            z=1/(1/complex(25,omega*l)+complex(0,omega*c))
            self.assertAlmostEqual(entry['value'],abs((z-50)/(z+50))**2,delta=1e-12)
        self.assertEqual(result['best_analysis'], analyze_document(self.library,result['best_model']))
        self.assertEqual(result['best_objectives'][0]['contribution'],native['value'])
        self.assertEqual(result,self.evaluate(document))
        json.dumps(result,allow_nan=False)

    def test_coherent_system_image_phase(self):
        result=self.evaluate(fixture('image-rejection'))
        self.assertEqual(result['result']['stop'],'target_reached')
        self.assertLess(abs(result['result']['parameters'][0]-math.pi/2),1e-6)
        for entry in result['result']['history']:
            phase=entry['parameters'][0]
            image=(1-math.sin(phase))/2
            desired=(1+math.sin(phase))/2
            reference=image+max(.99-desired,0)**2
            self.assertAlmostEqual(entry['value'],reference,delta=1e-12)
        self.assertLess(result['best_objectives'][0]['measured'],1e-14)
        self.assertGreater(result['best_objectives'][1]['measured'],.99)
        # Selectors continue to identify components after unrelated list order changes.
        document=fixture()
        document['analysis']['devices'].reverse()
        reordered, original = self.evaluate(document)['result'], self.evaluate(fixture())['result']
        self.assertEqual(reordered['parameters'],original['parameters'])
        self.assertEqual(reordered['stop'],original['stop'])
        self.assertEqual(reordered['evaluations'],original['evaluations'])
        for a,b in zip(reordered['history'],original['history']):
            self.assertEqual(a['parameters'],b['parameters'])
            self.assertAlmostEqual(a['value'],b['value'],delta=1e-12)

    def test_integer_multiple_bindings_and_units(self):
        component=lambda b,g: dict(bin=b,kind='source',bandwidth_hz=1.,coherence_group=g,amplitude=1.)
        document=dict(format='rfmodel.optimization',version=1,
            analysis=dict(format='rfmodel.coherence',version=1,spacing_hz=1e8,
                          components=[component(2,1),component(5,2)]),
            parameters=[dict(id='bin',lower=1,upper=10,initial=2,kind='integer',unit='bin',
                bindings=[dict(path=['components',0,'bin']),dict(path=['components',1,'bin'],scale=2,offset=1)])],
            objectives=[dict(id='first',path=['power_by_bin',0,'frequency_hz'],goal='target',target=7e8,scale=1e8),
                        dict(id='second',path=['power_by_bin',1,'frequency_hz'],goal='target',target=15e8,scale=1e8)],
            options=dict(target=0.))
        result=self.evaluate(document)
        self.assertEqual(result['result']['parameters'],(7.,))
        self.assertEqual(result['result']['value'],0.)
        self.assertEqual([c['bin'] for c in result['best_model']['components']],[7,15])
        self.assertIs(type(result['best_model']['components'][0]['bin']),int)
        self.assertEqual(result['parameters'][0]['unit'],'bin')

    def test_all_objective_formulas_and_complex_components(self):
        document=scalar_task()
        path=['samples',{'frequency_hz':1000000000},'s',1,0]
        specs=[('real','minimize',None,4.,2.),('imag','maximize',None,2.,.5),
               ('power','target',2.,3.,2.),('magnitude','at_most',.5,2.,1.),
               ('real','at_least',1.,1.,.5),('magnitude_db','target',0.,1.,1.),
               ('phase_radians','target',math.pi/2,1.,1.)]
        document['objectives']=[]
        for i,(component,goal,target,weight,scale) in enumerate(specs):
            entry=dict(id=str(i),path=path,component=component,goal=goal,weight=weight,scale=scale)
            if target is not None: entry['target']=target
            document['objectives'].append(entry)
        result=self.evaluate(document)
        self.assertEqual(result['result']['stop'],'all_parameters_fixed')
        self.assertAlmostEqual(result['result']['value'],1.25)
        self.assertAlmostEqual(sum(v['contribution'] for v in result['best_objectives']),1.25)

    def test_budget_snapshot_without_extra_analysis(self):
        document=fixture()
        document['options']=dict(max_evaluations=5,record_history=False)
        calls=[]
        operation=analysis_operation(document['analysis'])
        def observed(library,candidate,**kwargs):
            calls.append(copy.deepcopy(candidate))
            return operation(library,candidate,**kwargs)
        with patch('rfmodel.optimization_file.analysis_operation',return_value=observed):
            result=self.evaluate(document)
        self.assertEqual(len(calls),5)
        self.assertEqual(result['result']['evaluations'],5)
        self.assertEqual(result['result']['stop'],'evaluation_limit')
        self.assertEqual(result['result']['history'],[])
        self.assertIn(result['best_model'],calls)
        self.assertEqual(result['best_analysis'],analyze_document(self.library,result['best_model']))

    def test_cancellation_and_invalid_candidate_policy(self):
        result=self.evaluate(fixture(),cancelled=lambda:True)
        self.assertEqual(result['result']['stop'],'cancelled')
        self.assertEqual(result['result']['evaluations'],0)
        self.assertIsNone(result['best_model'])
        self.assertIsNone(result['best_analysis'])
        document=fixture()
        document['parameters'][0].update(lower=-1e-8,upper=1e-8,initial=-1e-9,kind='real')
        document['options']=dict(max_evaluations=8)
        failed=self.evaluate(document)
        self.assertEqual(failed['result']['stop'],'objective_failure')
        self.assertEqual(failed['result']['evaluations'],1)
        self.assertIsNone(failed['best_objectives'])
        self.assertEqual(failed['result']['history'][0]['status'],'exception')
        document['options']['invalid_evaluation']='reject'
        rejected=self.evaluate(document)
        self.assertEqual(rejected['result']['evaluations'],8)
        self.assertGreater(rejected['result']['rejected_evaluations'],0)
        self.assertTrue(rejected['result']['has_finite_point'])
        json.dumps(rejected,allow_nan=False)

    def test_schema_rejection_before_analysis(self):
        changes=[lambda d:d.update(version=True),lambda d:d.update(surprise=1),
            lambda d:d['analysis'].update(format='rfmodel.optimization'),
            lambda d:d.update(parameters=[]),lambda d:d.update(objectives=[]),
            lambda d:d['parameters'][0].update(kind='random'),
            lambda d:d['parameters'][0].update(initial=True),
            lambda d:d['parameters'][0]['bindings'][0].update(scale=0),
            lambda d:d['parameters'][0]['bindings'][0].update(path=['devices',True]),
            lambda d:d['parameters'][0]['bindings'][0].update(path=['devices',-1]),
            lambda d:d['parameters'][0]['bindings'][0].update(path=['missing']),
            lambda d:d['parameters'][0]['bindings'][0].update(path=['version']),
            lambda d:d['parameters'][0]['bindings'][0].update(path=['devices',{'id':'missing'},'model','inductance_h']),
            lambda d:d['parameters'][0]['bindings'].append(copy.deepcopy(d['parameters'][0]['bindings'][0])),
            lambda d:(d['parameters'][0].update(lower=1.,upper=2.,initial=1.5,kind='real'),
                      d['parameters'][0]['bindings'][0].update(scale=1e308)),
            lambda d:d['parameters'][1].update(id='L'),
            lambda d:d['objectives'][0].update(weight=-1),
            lambda d:d['objectives'][0].update(scale=0),
            lambda d:d['objectives'][0].update(component='db10'),
            lambda d:d['objectives'][0].update(goal='target'),
            lambda d:d['objectives'][0].update(target=0),
            lambda d:d['options'].update(max_evaluations=False),
            lambda d:d['options'].update(random_seed=1)]
        for i,change in enumerate(changes):
            with self.subTest(case=i):
                document=fixture();change(document);before=copy.deepcopy(document)
                calls=[]
                def lookup(candidate):
                    operation=analysis_operation(candidate)
                    def observed(*args,**kwargs):
                        calls.append(1)
                        return operation(*args,**kwargs)
                    return observed
                with patch('rfmodel.optimization_file.analysis_operation',side_effect=lookup):
                    with self.assertRaises((ValueError,TypeError,RFModelError,OverflowError)):
                        self.evaluate(document)
                self.assertEqual(calls,[])
                self.assertEqual(document,before)

    def test_result_path_and_log_zero_fail_explicitly(self):
        document=scalar_task()
        document['objectives'][0]['path']=['samples',{'frequency_hz':123},'s',1,0]
        result=self.evaluate(document)
        self.assertEqual(result['result']['stop'],'objective_failure')
        self.assertIn('exactly one',result['result']['last_error'])
        document=scalar_task()
        document['objectives'][0].update(path=['samples',0,'s',0,0],component='magnitude_db')
        result=self.evaluate(document)
        self.assertEqual(result['result']['stop'],'objective_failure')
        self.assertIsNone(result['best_analysis'])

    def test_touchstone_fingerprint_and_cli_protection(self):
        with tempfile.TemporaryDirectory() as folder:
            folder=Path(folder);data=folder/'data.s2p';source=folder/'task.json';output=folder/'out.json'
            raw=b'# GHz S RI R 50\n1 0 0 .5 0 .5 0 0 0\n';data.write_bytes(raw)
            document=scalar_task()
            document['analysis']['devices'][0]['model']=dict(type='touchstone',path='data.s2p')
            document['parameters']=[dict(id='frequency',lower=1e9,upper=1e9,initial=1e9,
                                        bindings=[dict(path=['frequencies_hz',0])])]
            result=self.evaluate(document,base_directory=folder)
            self.assertEqual(referenced_touchstone_paths(document,folder),{data.resolve()})
            self.assertEqual(len(result['dependencies']),1)
            self.assertEqual(result['result']['value'],.25)
            source.write_text(json.dumps(document),encoding='utf8')
            env=dict(os.environ) if INSTALLED else dict(os.environ,PYTHONPATH=str(ROOT/'python'))
            command=[sys.executable,'-m','rfmodel',str(source),'--library',str(LIBRARY_PATH),'--output']
            run=subprocess.run(command+[str(output)],env=env,capture_output=True,text=True)
            self.assertEqual(run.returncode,0,run.stderr)
            self.assertEqual(json.loads(output.read_text()),json.loads(json.dumps(result)))
            for protected in (data,source):
                before=protected.read_bytes()
                run=subprocess.run(command+[str(protected)],env=env,capture_output=True,text=True)
                self.assertNotEqual(run.returncode,0)
                self.assertEqual(protected.read_bytes(),before)
            alias=folder/'hard-link-output.json'
            os.link(data,alias)
            run=subprocess.run(command+[str(alias)],env=env,capture_output=True,text=True)
            self.assertNotEqual(run.returncode,0)
            self.assertEqual(data.read_bytes(),raw)
            saved=output.read_bytes();document['parameters'][0]['bindings'][0]['path']=['missing']
            source.write_text(json.dumps(document),encoding='utf8')
            run=subprocess.run(command+[str(output)],env=env,capture_output=True,text=True)
            self.assertNotEqual(run.returncode,0)
            self.assertEqual(output.read_bytes(),saved)

    def test_ambiguous_overlapping_and_integer_range_bindings(self):
        document=fixture()
        document['analysis']['devices'].append(copy.deepcopy(document['analysis']['devices'][0]))
        with self.assertRaisesRegex(ValueError,'exactly one'):
            self.evaluate(document)
        document=fixture()
        document['parameters'][0]['bindings'].append(dict(path=['devices',1,'model','inductance_h']))
        with self.assertRaisesRegex(ValueError,'Overlapping'):
            self.evaluate(document)
        document=scalar_task()
        document['parameters'][0].update(lower=1,upper=2,initial=1,kind='integer')
        document['parameters'][0]['bindings'][0]['scale']=2**53-1
        with self.assertRaisesRegex(ValueError,'exact integer range'):
            self.evaluate(document)
        document=scalar_task()
        document['objectives'][0]['path']=['samples',{'frequency_hz':float('inf')},'s',1,0]
        with self.assertRaises(ValueError):
            self.evaluate(document)

    def test_existing_analysis_formats_as_fixed_tasks(self):
        names=('affine-mixer-network.json','amplifier-chain.json','clocked-coherent-combiner.json',
               'coherence.json','coherent-amplifier-cascade.json','limited-single-tone.json',
               'mixer-image-noise.json','mixer-lo-phase-noise.json','multitone-amplifier.json')
        def first_number(value,path=()):
            if type(value) in (int,float) and math.isfinite(value):
                return list(path),value
            children=value.items() if isinstance(value,dict) else enumerate(value) if isinstance(value,list) else ()
            for key,child in children:
                if key=='version': continue
                found=first_number(child,path+(key,))
                if found is not None: return found
            return None
        for name in names:
            with self.subTest(name=name):
                model=load(ROOT/'examples'/name)
                baseline=analyze_document(self.library,model,base_directory=ROOT/'examples')
                path,value=first_number(model)
                metric,expected=first_number(baseline)
                task=dict(format='rfmodel.optimization',version=1,analysis=model,
                    parameters=[dict(id='fixed',lower=value,upper=value,initial=value,
                        kind='integer' if type(value) is int else 'real',bindings=[dict(path=path)])],
                    objectives=[dict(id='metric',path=metric,goal='minimize')])
                result=self.evaluate(task,base_directory=ROOT/'examples')
                self.assertEqual(result['result']['stop'],'all_parameters_fixed')
                self.assertEqual(result['result']['evaluations'],1)
                self.assertEqual(result['result']['value'],expected)
                self.assertEqual(result['best_analysis'],baseline)
                self.assertEqual(result['best_model'],model)

    def test_changing_dependency_discarded(self):
        with tempfile.TemporaryDirectory() as folder:
            folder=Path(folder);data=folder/'data.s2p'
            raw=b'# GHz S RI R 50\n1 0 0 .5 0 .5 0 0 0\n';data.write_bytes(raw)
            document=scalar_task()
            document['analysis']['devices'][0]['model']=dict(type='touchstone',path='data.s2p')
            document['parameters']=[dict(id='frequency',lower=1e9,upper=1e9,initial=1e9,
                                        bindings=[dict(path=['frequencies_hz',0])])]
            operation=analysis_operation(document['analysis'])
            def changed(library,candidate,**kwargs):
                result=operation(library,candidate,**kwargs)
                data.write_bytes(raw+b'! modified\n')
                return result
            with patch('rfmodel.optimization_file.analysis_operation',return_value=changed):
                with self.assertRaisesRegex(RuntimeError,'changed during optimization'):
                    self.evaluate(document,base_directory=folder)


if __name__=='__main__':
    unittest.main()
