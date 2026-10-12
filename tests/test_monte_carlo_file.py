"""Declarative Monte Carlo tasks, physical RF references and installed CLI checks."""
import copy
import json
import math
import os
from pathlib import Path
import statistics
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
from rfmodel import Library, MonteCarloParameter, RFModelError
from rfmodel.analysis_file import analyze_document, analysis_operation
from rfmodel.model_file import load, referenced_touchstone_paths
from rfmodel.monte_carlo_file import analyze_monte_carlo


def fixture(name='resistor-tolerance', samples=None):
    document = load(ROOT/f'examples/monte-carlo-{name}.json')
    if samples is not None:
        document['options']['samples'] = samples
    return document


def scalar_task():
    return dict(format='rfmodel.monte-carlo', version=1,
        analysis=dict(format='rfmodel.linear-network', version=1, frequencies_hz=[1e9],
            devices=[dict(id='gain', model=dict(type='linear_amplifier', gain_db=0., gain_phase_degrees=90.))],
            external_ports=[['gain', 0], ['gain', 1]]),
        parameters=[dict(id='phase', distribution=dict(kind='fixed', value=90.),
            bindings=[dict(path=['devices', {'id': 'gain'}, 'model', 'gain_phase_degrees'])])],
        metrics=[dict(id='power', path=['samples', 0, 's', 1, 0], component='power')],
        options=dict(samples=2))


class MonteCarloTaskTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.library = Library(LIBRARY_PATH)

    def evaluate(self, document, **kwargs):
        return analyze_monte_carlo(self.library, document, **kwargs)

    def test_resistor_complete_trace_and_statistics(self):
        document = fixture()
        before = copy.deepcopy(document)
        report = self.evaluate(document)
        self.assertEqual(document, before)
        self.assertEqual(report['format'], 'rfmodel.monte-carlo-results')
        self.assertEqual(report['parameter_order'], ['R1', 'R2'])
        r = report['result']
        self.assertEqual((report['analysis_evaluations'], r['evaluations'], r['samples_attempted']), (4096,)*3)
        self.assertEqual((r['stop'], r['passed_samples'], r['passed_fraction']), ('completed', 2060, 2060/4096))
        columns = [[], []]
        for entry in r['history']:
            a, b = entry['parameters']
            expected = [(100/(100+a+b))**2, ((a+b)/(100+a+b))**2]
            self.assertEqual(entry['passed'], a <= 50)
            for j in range(2):
                self.assertAlmostEqual(entry['values'][j], expected[j], delta=1e-12)
                columns[j].append(expected[j])
        for j, col in enumerate(columns):
            s = r['statistics'][j]
            self.assertAlmostEqual(s['mean'], statistics.mean(col), delta=1e-12)
            self.assertAlmostEqual(s['sample_variance'], statistics.variance(col), delta=1e-12)
            self.assertAlmostEqual(s['standard_error'], statistics.stdev(col)/64, delta=1e-12)
            col.sort()
            for p, value in zip(r['quantile_levels'], s['quantiles']):
                h=(len(col)-1)*p; i=int(h)
                self.assertAlmostEqual(value, col[i]+(h-i)*(col[min(i+1,len(col)-1)]-col[i]), delta=1e-12)
        self.assertEqual(report, self.evaluate(document))
        json.dumps(report, allow_nan=False)

    def test_image_phase_analytic_population(self):
        report = self.evaluate(fixture('image-phase-tolerance'))
        r = report['result']
        self.assertEqual(r['accepted_samples'], 1024)
        for entry in r['history']:
            delta = entry['parameters'][0]
            expected = [(1-math.cos(delta))/2, (1+math.cos(delta))/2]
            for actual, ref in zip(entry['values'], expected):
                self.assertAlmostEqual(actual, ref, delta=1e-12)
            self.assertEqual(entry['passed'], expected[0] <= .005)
        b=.2
        mean=(1-math.sin(b)/b)/2
        variance=(1+math.sin(2*b)/(2*b))/8-(math.sin(b)/b)**2/4
        probability=math.acos(.99)/b
        self.assertLessEqual(abs(r['statistics'][0]['mean']-mean),6*math.sqrt(variance/1024))
        self.assertLessEqual(abs(r['passed_fraction']-probability),6*math.sqrt(probability*(1-probability)/1024))
        self.assertEqual(report['parameters'][0]['bindings'][0]['offset'], math.pi/2)

    def test_all_distribution_contracts_use_native_stream(self):
        cases=[(dict(kind='fixed',value=50), MonteCarloParameter('fixed',50)),
               (dict(kind='uniform',lower=45,upper=55), MonteCarloParameter('uniform',45,55)),
               (dict(kind='log_uniform',lower=45,upper=55), MonteCarloParameter('log_uniform',45,55)),
               (dict(kind='normal',mean=50,standard_deviation=.1), MonteCarloParameter('normal',50,.1)),
               (dict(kind='log_normal',log_mean=math.log(50),log_standard_deviation=.01), MonteCarloParameter('log_normal',math.log(50),.01)),
               (dict(kind='discrete_uniform',lower=45,upper=55), MonteCarloParameter('discrete_uniform',45,55))]
        for distribution, parameter in cases:
            with self.subTest(distribution=distribution):
                d=fixture(samples=32);d['parameters'][0]['distribution']=distribution
                r=self.evaluate(d)['result']
                native=self.library.run_monte_carlo([parameter],1,lambda x:x,samples=32,seed=20261012)
                self.assertEqual(r['random_words'],native.random_words)
                self.assertEqual([e['parameters'][0] for e in r['history']], [e.parameters[0] for e in native.history])
                self.assertEqual(r['accepted_samples'],32)

    def test_shared_parameter_multiple_bindings(self):
        d=fixture(samples=32)
        d['parameters']=[dict(id='delta',distribution=dict(kind='uniform',lower=-5,upper=5),unit='ohm',
            bindings=[dict(path=['devices',{'id':'R1'},'model','resistance_ohms'],offset=50),
                      dict(path=['devices',{'id':'R2'},'model','resistance_ohms'],scale=-1,offset=50)])]
        calls=[];operation=analysis_operation(d['analysis'])
        def observed(library,candidate,**kwargs):
            calls.append(copy.deepcopy(candidate))
            return operation(library,candidate,**kwargs)
        with patch('rfmodel.monte_carlo_file.analysis_operation',return_value=observed):
            report=self.evaluate(d)
        self.assertEqual(len(calls),32)
        for entry,candidate in zip(report['result']['history'],calls):
            a,b=[device['model']['resistance_ohms'] for device in candidate['devices']]
            self.assertEqual((a,b),(50+entry['parameters'][0],50-entry['parameters'][0]))
            self.assertAlmostEqual(entry['values'][0],.25,delta=1e-12)
        self.assertLess(report['result']['statistics'][0]['sample_standard_deviation'],1e-14)
        self.assertEqual(report['input_analysis'],d['analysis'])

    def test_discrete_integer_model_fields(self):
        component=lambda b,g:dict(bin=b,kind='source',bandwidth_hz=1.,coherence_group=g,amplitude=1.)
        d=dict(format='rfmodel.monte-carlo',version=1,
            analysis=dict(format='rfmodel.coherence',version=1,spacing_hz=1e8,components=[component(2,1),component(5,2)]),
            parameters=[dict(id='bin',distribution=dict(kind='discrete_uniform',lower=1,upper=10),unit='bin',
                bindings=[dict(path=['components',0,'bin']),dict(path=['components',1,'bin'],scale=2,offset=1)])],
            metrics=[dict(id='first',path=['power_by_bin',0,'frequency_hz']),dict(id='second',path=['power_by_bin',1,'frequency_hz'])],
            options=dict(samples=32,seed=7))
        calls=[];operation=analysis_operation(d['analysis'])
        def observed(library,candidate,**kwargs):
            calls.append(candidate)
            self.assertIs(type(candidate['components'][0]['bin']),int)
            return operation(library,candidate,**kwargs)
        with patch('rfmodel.monte_carlo_file.analysis_operation',return_value=observed):
            r=self.evaluate(d)['result']
        self.assertEqual(r['accepted_samples'],32)
        for e in r['history']:
            n=e['parameters'][0]
            self.assertEqual(e['values'],(n*1e8,(2*n+1)*1e8))
        self.assertFalse(r['criteria_enabled'])
        self.assertTrue(all(e['passed'] is None for e in r['history']))

    def test_all_measurement_components(self):
        d=scalar_task()
        components=('real','imag','magnitude','power','magnitude_db','phase_radians')
        d['metrics']=[dict(id=c,path=['samples',0,'s',1,0],component=c) for c in components]
        d['metrics'].append(dict(id='frequency',path=['samples',{'frequency_hz':1e9},'frequency_hz']))
        r=self.evaluate(d)['result']
        for actual,expected in zip(r['history'][0]['values'],(0,1,1,1,0,math.pi/2,1e9)):
            self.assertAlmostEqual(actual,expected,delta=1e-12)

    def test_history_switch_and_no_export_reanalysis(self):
        d=fixture(samples=12)
        original=self.evaluate(d)
        d['options']['record_history']=False
        calls=[];operation=analysis_operation(d['analysis'])
        def observed(*args,**kwargs):
            calls.append(1);return operation(*args,**kwargs)
        with patch('rfmodel.monte_carlo_file.analysis_operation',return_value=observed):
            report=self.evaluate(d)
            json.dumps(report,allow_nan=False)
        expected=dict(original['result'],history=[])
        self.assertEqual(report['result'],expected)
        self.assertEqual(report['analysis_evaluations'],len(calls))
        self.assertEqual(len(calls),12)
        self.assertNotIn('best_analysis',report)

    def test_failure_policy_counts_attempts_without_resampling(self):
        d=fixture(samples=32)
        d['parameters'][0]['distribution']=dict(kind='uniform',lower=-5,upper=5)
        stopped=self.evaluate(d)['result']
        self.assertEqual(stopped['stop'],'evaluation_failure')
        self.assertEqual(stopped['rejected_samples'],1)
        self.assertLess(stopped['samples_attempted'],32)
        d['options']['invalid_sample']='reject'
        report=self.evaluate(d);r=report['result']
        self.assertEqual((r['stop'],r['samples_attempted'],r['evaluations'],report['analysis_evaluations']),('completed',32,32,32))
        valid=sum(e['parameters'][0]>=0 for e in r['history'])
        self.assertEqual((r['accepted_samples'],r['rejected_samples']),(valid,32-valid))
        self.assertGreater(r['rejected_samples'],0)
        self.assertEqual(r['passed_fraction'],r['passed_samples']/32)
        self.assertEqual(r['valid_pass_fraction'],r['passed_samples']/valid)
        json.dumps(report,allow_nan=False)

    def test_sampling_binding_and_measurement_failures(self):
        d=fixture(samples=3);d['options']['invalid_sample']='reject'
        d['parameters'][0]['distribution']=dict(kind='log_normal',log_mean=10000,log_standard_deviation=.01)
        report=self.evaluate(d);r=report['result']
        self.assertEqual((r['samples_attempted'],r['evaluations'],report['analysis_evaluations'],r['random_words']),(3,0,0,6))
        self.assertEqual(r['history'][0]['status'],'sampling_failure')
        self.assertIsNone(r['last_failed_parameters'][0])
        json.dumps(report,allow_nan=False)
        d['parameters'][0]['distribution']=dict(kind='normal',mean=1e308,standard_deviation=0)
        d['parameters'][0]['bindings'][0]['scale']=2
        report=self.evaluate(d)
        self.assertEqual((report['result']['evaluations'],report['analysis_evaluations']),(3,0))
        self.assertEqual(report['result']['history'][0]['status'],'exception')
        d=fixture(samples=3);d['options']['invalid_sample']='reject';d['metrics'][0]['path']=['missing']
        report=self.evaluate(d)
        self.assertEqual((report['result']['evaluations'],report['analysis_evaluations']),(3,3))
        self.assertEqual(report['result']['accepted_samples'],0)
        self.assertIsNone(report['result']['statistics'][0]['mean'])
        d=scalar_task();d['metrics'][0].update(path=['samples',0,'s',0,0],component='magnitude_db')
        self.assertEqual(self.evaluate(d)['result']['stop'],'evaluation_failure')

    def test_cancellation_and_fatal_exception(self):
        report=self.evaluate(fixture(samples=3),cancelled=lambda:True)
        self.assertEqual((report['analysis_evaluations'],report['result']['samples_attempted']),(0,0))
        self.assertEqual(report['result']['stop'],'cancelled')
        calls=[];d=fixture(samples=8);operation=analysis_operation(d['analysis'])
        def observed(*args,**kwargs):
            calls.append(1);return operation(*args,**kwargs)
        with patch('rfmodel.monte_carlo_file.analysis_operation',return_value=observed):
            r=self.evaluate(d,cancelled=lambda:len(calls)==2)['result']
        self.assertEqual((r['stop'],r['samples_attempted']),('cancelled',2))
        def fatal(*args,**kwargs):
            raise MemoryError('analysis allocation')
        d['options']['invalid_sample']='reject'
        with patch('rfmodel.monte_carlo_file.analysis_operation',return_value=fatal):
            with self.assertRaises(MemoryError):self.evaluate(d)

    def test_schema_validation_precedes_analysis(self):
        changes=[lambda d:d.update(version=True),lambda d:d.update(extra=1),
            lambda d:d['analysis'].update(format='rfmodel.monte-carlo'),
            lambda d:d['analysis'].update(format='rfmodel.optimization'),
            lambda d:d.update(parameters=[]),lambda d:d.update(metrics=[]),
            lambda d:d['parameters'][0]['distribution'].update(kind='unknown'),
            lambda d:d['parameters'][0]['distribution'].update(lower=True),
            lambda d:d['parameters'][0]['distribution'].update(upper=40),
            lambda d:d['parameters'][0]['distribution'].update(extra=1),
            lambda d:d['parameters'][0].update(distribution=dict(kind='normal',mean=50,standard_deviation=-1)),
            lambda d:d['parameters'][0]['bindings'][0].update(scale=0),
            lambda d:d['parameters'][0]['bindings'][0].update(path=['version']),
            lambda d:d['parameters'][0]['bindings'][0].update(path=['devices',True]),
            lambda d:d['parameters'][0]['bindings'][0].update(path=['missing']),
            lambda d:d['parameters'][0]['bindings'][0].update(scale=1e308),
            lambda d:d['parameters'][1].update(id='R1'),
            lambda d:d['metrics'][1].update(id=d['metrics'][0]['id']),
            lambda d:d['metrics'][0].update(component='db10'),
            lambda d:d['metrics'][0].update(limits={}),
            lambda d:d['metrics'][0].update(limits=dict(lower=2,upper=1)),
            lambda d:d['metrics'][0].update(limits=dict(upper=math.inf)),
            lambda d:d['options'].update(samples=True),
            lambda d:d['options'].update(seed=2**64),
            lambda d:d['options'].update(quantile_levels='none'),
            lambda d:d['options'].update(quantile_levels=[.5,.5]),
            lambda d:d['options'].update(criteria=[]),
            lambda d:d['analysis']['devices'][1]['model'].update(resistance_ohms=math.nan)]
        for index,change in enumerate(changes):
            with self.subTest(index=index):
                d=fixture(samples=2);change(d);calls=[]
                def lookup(candidate):
                    operation=analysis_operation(candidate)
                    def observed(*args,**kwargs):calls.append(1);return operation(*args,**kwargs)
                    return observed
                with patch('rfmodel.monte_carlo_file.analysis_operation',side_effect=lookup):
                    with self.assertRaises((ValueError,TypeError,RFModelError,OverflowError)):
                        self.evaluate(d)
                self.assertEqual(calls,[])

    def test_unique_selectors_overlaps_and_integer_range(self):
        d=fixture(samples=1);d['analysis']['devices'].append(copy.deepcopy(d['analysis']['devices'][0]))
        with self.assertRaisesRegex(ValueError,'exactly one'):self.evaluate(d)
        d=fixture(samples=1);d['parameters'][0]['bindings'].append(dict(path=['devices',0,'model','resistance_ohms']))
        with self.assertRaisesRegex(ValueError,'Overlapping'):self.evaluate(d)
        d=fixture(samples=1);d['parameters'][0]['distribution']=dict(kind='discrete_uniform',lower=1,upper=2)
        d['parameters'][0]['bindings'][0]['scale']=2**53-1
        with self.assertRaisesRegex(ValueError,'exact integer range'):self.evaluate(d)
        d=fixture(samples=1);d['metrics'][0]['path']=['samples',{'frequency_hz':123},'s',1,0]
        self.assertEqual(self.evaluate(d)['result']['stop'],'evaluation_failure')
        d=fixture(samples=1);d['analysis']['devices'].reverse()
        changed,original=self.evaluate(d)['result'],self.evaluate(fixture(samples=1))['result']
        self.assertEqual(changed['history'][0]['parameters'],original['history'][0]['parameters'])
        for a,b in zip(changed['history'][0]['values'],original['history'][0]['values']):self.assertAlmostEqual(a,b,delta=1e-12)

    def test_frozen_document_during_callback(self):
        d=fixture(samples=3);before=copy.deepcopy(d);operation=analysis_operation(d['analysis'])
        def changed(*args,**kwargs):
            d['options']['samples']=1;d['parameters'][0]['distribution']['lower']=1000
            d['analysis']['devices'][0]['model']['resistance_ohms']=1000
            d['metrics'][0]['limits']['lower']=1000
            return operation(*args,**kwargs)
        with patch('rfmodel.monte_carlo_file.analysis_operation',return_value=changed):
            r=self.evaluate(d)
        expected=self.evaluate(before)
        self.assertEqual(r,expected)

    def test_existing_nine_analysis_formats(self):
        names=('affine-mixer-network.json','amplifier-chain.json','clocked-coherent-combiner.json',
               'coherence.json','coherent-amplifier-cascade.json','limited-single-tone.json',
               'mixer-image-noise.json','mixer-lo-phase-noise.json','multitone-amplifier.json')
        def first_number(value,path=()):
            if type(value) in (int,float) and math.isfinite(value):return list(path),value
            children=value.items() if isinstance(value,dict) else enumerate(value) if isinstance(value,list) else ()
            for key,child in children:
                if key=='version':continue
                found=first_number(child,path+(key,))
                if found is not None:return found
        formats=set()
        for name in names:
            model=load(ROOT/'examples'/name);formats.add(model['format'])
            baseline=analyze_document(self.library,model,base_directory=ROOT/'examples')
            path,value=first_number(model);metric,expected=first_number(baseline)
            distribution=(dict(kind='discrete_uniform',lower=value,upper=value) if type(value) is int
                          else dict(kind='fixed',value=value))
            d=dict(format='rfmodel.monte-carlo',version=1,analysis=model,
                parameters=[dict(id='fixed',distribution=distribution,bindings=[dict(path=path)])],
                metrics=[dict(id='metric',path=metric)],options=dict(samples=2))
            r=self.evaluate(d,base_directory=ROOT/'examples')
            self.assertEqual(r['result']['stop'],'completed')
            self.assertEqual(r['analysis_evaluations'],2)
            self.assertEqual(r['result']['statistics'][0]['mean'],expected)
            self.assertEqual(r['result']['statistics'][0]['sample_variance'],0.)
            self.assertEqual(r['input_analysis'],model)
        self.assertEqual(len(formats),9)

    def test_touchstone_fingerprints_cli_and_hardlinks(self):
        with tempfile.TemporaryDirectory() as folder:
            folder=Path(folder);data=folder/'data.s2p';source=folder/'task.json';output=folder/'out.json'
            raw=b'# GHz S RI R 50\n1 0 0 .5 0 .5 0 0 0\n';data.write_bytes(raw)
            d=scalar_task();d['analysis']['devices'][0]['model']=dict(type='touchstone',path='data.s2p')
            d['parameters']=[dict(id='f',distribution=dict(kind='fixed',value=1e9),bindings=[dict(path=['frequencies_hz',0])])]
            r=self.evaluate(d,base_directory=folder)
            self.assertEqual(referenced_touchstone_paths(d,folder),{data.resolve()})
            self.assertEqual(len(r['dependencies']),1)
            self.assertEqual(r['result']['statistics'][0]['mean'],.25)
            source.write_text(json.dumps(d),encoding='utf-8')
            env=dict(os.environ) if INSTALLED else dict(os.environ,PYTHONPATH=str(ROOT/'python'))
            command=[sys.executable,'-m','rfmodel',str(source),'--library',str(LIBRARY_PATH),'--output']
            run=subprocess.run(command+[str(output)],env=env,capture_output=True,text=True)
            self.assertEqual(run.returncode,0,run.stderr)
            self.assertEqual(json.loads(output.read_text()),json.loads(json.dumps(r)))
            for protected in (data,source):
                before=protected.read_bytes();run=subprocess.run(command+[str(protected)],env=env,capture_output=True,text=True)
                self.assertNotEqual(run.returncode,0);self.assertEqual(protected.read_bytes(),before)
            alias=folder/'alias.json';os.link(data,alias)
            run=subprocess.run(command+[str(alias)],env=env,capture_output=True,text=True)
            self.assertNotEqual(run.returncode,0);self.assertEqual(data.read_bytes(),raw)
            saved=output.read_bytes();d['parameters'][0]['bindings'][0]['path']=['missing']
            source.write_text(json.dumps(d),encoding='utf-8')
            run=subprocess.run(command+[str(output)],env=env,capture_output=True,text=True)
            self.assertNotEqual(run.returncode,0);self.assertEqual(output.read_bytes(),saved)

    def test_dependency_change_discards_task(self):
        with tempfile.TemporaryDirectory() as folder:
            folder=Path(folder);data=folder/'data.s2p';raw=b'# GHz S RI R 50\n1 0 0 .5 0 .5 0 0 0\n';data.write_bytes(raw)
            d=scalar_task();d['analysis']['devices'][0]['model']=dict(type='touchstone',path='data.s2p')
            d['parameters']=[dict(id='f',distribution=dict(kind='fixed',value=1e9),bindings=[dict(path=['frequencies_hz',0])])]
            operation=analysis_operation(d['analysis'])
            def changed(*args,**kwargs):
                r=operation(*args,**kwargs);data.write_bytes(raw+b'! changed\n');return r
            with patch('rfmodel.monte_carlo_file.analysis_operation',return_value=changed):
                with self.assertRaisesRegex(RuntimeError,'changed during Monte Carlo'):
                    self.evaluate(d,base_directory=folder)

    def test_full_seed_defaults_and_cli_sample_failure(self):
        d=fixture(samples=2);d['options']=dict(samples=2,seed=2**64-1)
        r=self.evaluate(d)
        self.assertEqual(r['result']['seed'],2**64-1)
        self.assertEqual(r['options']['quantile_levels'],[.05,.5,.95])
        self.assertTrue(r['options']['record_history'])
        with tempfile.TemporaryDirectory() as folder:
            source=Path(folder)/'task.json';output=Path(folder)/'out.json'
            d['parameters'][0]['distribution']=dict(kind='fixed',value=-1)
            source.write_text(json.dumps(d),encoding='utf-8')
            env=dict(os.environ) if INSTALLED else dict(os.environ,PYTHONPATH=str(ROOT/'python'))
            run=subprocess.run([sys.executable,'-m','rfmodel',str(source),'--library',str(LIBRARY_PATH),'--output',str(output)],env=env,capture_output=True,text=True)
            self.assertEqual(run.returncode,0,run.stderr)
            result=json.loads(output.read_text())['result']
            self.assertEqual(result['stop'],'evaluation_failure')
            self.assertEqual(result['seed'],2**64-1)
            self.assertIsNone(result['statistics'][0]['mean'])


if __name__=='__main__':
    unittest.main()
