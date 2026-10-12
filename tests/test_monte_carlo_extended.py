"""Extended C-backed Monte Carlo snapshots and versioned correlated RF tasks."""
import copy
import itertools
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
import rfmodel
from rfmodel.monte_carlo import ALGORITHM, COPULA_ALGORITHM
from rfmodel.monte_carlo_file import analyze_monte_carlo
from rfmodel.model_file import load, referenced_touchstone_paths
from rfmodel.analysis_file import analyze_document, analysis_operation
P = rfmodel.MonteCarloParameter


def task(samples=16):
    d=load(ROOT/'examples/monte-carlo-correlated-resistors.json')
    d['options']['samples']=samples
    return d


class MonteCarloExtendedTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lib=rfmodel.Library(LIBRARY_PATH)

    def run_extended(self, parameters=None, metric_count=2, evaluate=lambda x:x, **options):
        return self.lib.run_monte_carlo_extended(parameters or [P('normal',0,1)]*2,
            metric_count,evaluate,**dict(dict(samples=16,joint_statistics=True),**options))

    def check_joint(self, joint, rows):
        self.assertEqual(joint.count,len(rows))
        columns=list(zip(*rows))
        for i,x in enumerate(columns):
            for j,y in enumerate(columns):
                mx,my=statistics.mean(x),statistics.mean(y)
                cov=math.fsum((a-mx)*(b-my) for a,b in zip(x,y))/(len(rows)-1)
                k=i*len(columns)+j
                self.assertAlmostEqual(joint.sample_covariance[k],cov,delta=1e-12*max(1,abs(cov)))
                sx,sy=statistics.stdev(x),statistics.stdev(y)
                if sx and sy:self.assertAlmostEqual(joint.sample_correlation[k],cov/sx/sy,delta=1e-12)
                else:self.assertIsNone(joint.sample_correlation[k])

    def test_legacy_result_and_independent_stream(self):
        parameters=[P('uniform',-2,3),P('normal',3,2)]
        old=self.lib.run_monte_carlo(parameters,2,lambda x:x,samples=32,seed=44)
        new=self.run_extended(parameters,samples=32,seed=44)
        self.assertIs(type(old),rfmodel.MonteCarloResult)
        self.assertEqual(len(old),19)
        self.assertEqual(new.result,old)
        self.assertEqual((new.latent_rank,new.effective_latent_correlation),(0,()))
        self.check_joint(new.parameter_joint,[e.parameters for e in old.history])
        off=self.run_extended(parameters,samples=32,seed=44,joint_statistics=False)
        self.assertEqual(off.result,old)
        self.assertEqual(off.metric_joint,rfmodel.MonteCarloJointStatistics(0,0,(),()))

    def test_singular_correlation_and_replay(self):
        options=dict(latent_correlation=[1,-1,-1,1],seed=20261012)
        r=self.run_extended(**options)
        self.assertEqual((r.result.algorithm,r.latent_rank,r.result.random_words),(COPULA_ALGORITHM,1,64))
        self.assertEqual(r.effective_latent_correlation,(1,-1,-1,1))
        for e in r.result.history:self.assertEqual(e.parameters[0],-e.parameters[1])
        self.assertEqual(r,self.run_extended(**options))
        nohistory=self.run_extended(**options,record_history=False)
        self.assertEqual(nohistory.parameter_joint,r.parameter_joint)
        self.assertEqual(nohistory.metric_joint,r.metric_joint)
        self.assertEqual(nohistory.result,r.result._replace(history=()))

    def test_joint_pairing_before_quantile_sort(self):
        rows=[(1,9,7),(3,2,7),(2,4,7),(5,-1,7),(4,6,7)]
        stream=iter(rows)
        r=self.run_extended([P('fixed',1)],3,lambda _:next(stream),samples=5)
        self.check_joint(r.metric_joint,rows)
        self.assertAlmostEqual(r.metric_joint.sample_covariance[1],-4.5)
        self.assertEqual(r.parameter_joint.sample_covariance,(0,))
        self.assertEqual(r.parameter_joint.sample_correlation,(None,))
        for i,s in enumerate(r.result.statistics):self.assertEqual(s.sample_variance,r.metric_joint.sample_covariance[3*i+i])

    def test_extreme_covariance_and_unavailable_values(self):
        rows=iter([(-sys.float_info.max,1e-308),(sys.float_info.max,-1e-308)])
        r=self.run_extended([P('fixed',1)],2,lambda _:next(rows),samples=2)
        self.assertIsNone(r.metric_joint.sample_covariance[0])
        self.assertAlmostEqual(r.metric_joint.sample_covariance[1]/(-2*(sys.float_info.max*1e-308)),1,delta=1e-14)
        for actual,expected in zip(r.metric_joint.sample_correlation,(1,-1,-1,1)):
            self.assertAlmostEqual(actual,expected,delta=1e-14)
        self.assertEqual(r.metric_joint.sample_covariance[3],0)
        json.dumps(r._asdict(),allow_nan=False)
        one=self.run_extended(samples=1)
        self.assertEqual(one.parameter_joint.sample_covariance,(None,)*4)

    def test_rejected_rows_are_paired(self):
        seen=[]
        def evaluate(x):
            seen.append(x)
            if len(seen)%2:raise ValueError('invalid')
            return x
        r=self.run_extended(evaluate=evaluate,invalid_sample='reject',latent_correlation=[1,.5,.5,1])
        accepted=[e.parameters for e in r.result.history if e.status=='accepted']
        self.assertEqual((r.result.rejected_samples,r.parameter_joint.count,r.metric_joint.count),(8,8,8))
        self.check_joint(r.parameter_joint,accepted)
        self.assertEqual(r.parameter_joint,r.metric_joint)
        failed=self.run_extended(evaluate=lambda _:[],invalid_sample='reject')
        self.assertEqual(failed.metric_joint,rfmodel.MonteCarloJointStatistics(2,0,(None,)*4,(None,)*4))

    def test_sampling_failure_and_cancellation(self):
        for mean,expected in [(10000,None),(-10000,0)]:
            r=self.run_extended([P('log_normal',mean,.1),P('normal',0,1)],
                latent_correlation=[1,.5,.5,1],samples=3,invalid_sample='reject')
            self.assertEqual((r.result.random_words,r.result.evaluations,r.result.rejected_samples),(12,0,3))
            self.assertEqual(r.result.history[0].parameters[0],expected)
            self.assertEqual(r.parameter_joint.count,0)
            json.dumps(r._asdict(),allow_nan=False)
        r=self.run_extended(latent_correlation=[1,-1,-1,1],cancelled=lambda:True)
        self.assertEqual((r.result.stop,r.result.random_words,r.latent_rank),('cancelled',0,1))
        self.assertEqual(r.parameter_joint.count,0)
        seen=[]
        r=self.run_extended(evaluate=lambda x:seen.append(x) or x,cancelled=lambda:len(seen)==3)
        self.assertEqual(r.parameter_joint.count,3)

    def test_cleanup_on_fatal_callback_or_new_getter_failure(self):
        real_destroy=self.lib._dll.rfmodel_monte_carlo_destroy
        destroyed=[]
        def destroy(handle):destroyed.append(handle.value);real_destroy(handle)
        with patch.object(self.lib._dll,'rfmodel_monte_carlo_destroy',destroy):
            for exception in (MemoryError,KeyboardInterrupt,SystemExit):
                seen=[]
                def evaluate(x):seen.append(x);raise exception('fatal')
                before=len(destroyed)
                with self.assertRaises(exception):self.run_extended(evaluate=evaluate,invalid_sample='reject')
                self.assertEqual((len(seen),len(destroyed)-before),(1,1))
            for name in ('rfmodel_monte_carlo_get_dependence','rfmodel_monte_carlo_get_joint_statistics'):
                before=len(destroyed)
                with patch.object(self.lib._dll,name,side_effect=RuntimeError('read failed')):
                    with self.assertRaisesRegex(RuntimeError,'read failed'):self.run_extended()
                self.assertEqual(len(destroyed),before+1)
        with self.assertRaisesRegex(TypeError,'bool'):self.run_extended(cancelled=lambda:1)

    def test_frozen_inputs_and_immutable_snapshots(self):
        matrix=[1,-1,-1,1];parameters=[dict(distribution='normal',first=0,second=1)]*2
        def evaluate(x):matrix[:]=[1,0,0,1];parameters[0]['first']=500;return x
        r=self.run_extended(parameters,evaluate=evaluate,latent_correlation=matrix)
        for e in r.result.history:self.assertEqual(e.parameters[0],-e.parameters[1])
        self.assertEqual(r.effective_latent_correlation,(1,-1,-1,1))
        with self.assertRaises(AttributeError):r.latent_rank=0
        with self.assertRaises(AttributeError):r.metric_joint.count=0
        with self.assertRaises(TypeError):r.parameter_joint.sample_covariance[0]=0
        before=r
        self.run_extended(seed=1)
        self.assertEqual(r,before)

    def test_invalid_matrix_requests_before_callbacks(self):
        invalid=[([1],True),([1,.5,.6,1],True),([2,0,0,1],True),([1,math.nan,math.nan,1],True),
            ([1,math.inf,math.inf,1],True),([1,1.1,1.1,1],True),([1,0,0,1],1),
            ([[1,0],[0,1]],True),('1001',True),(b'1001',True),([True,0,0,1],True)]
        for matrix,joint in invalid:
            with self.subTest(matrix=matrix,joint=joint):
                with self.assertRaises((ValueError,TypeError,rfmodel.RFModelError)):
                    self.run_extended(latent_correlation=matrix,joint_statistics=joint,
                        evaluate=lambda _:self.fail('callback'),cancelled=lambda:self.fail('cancel'))
        with self.assertRaises(ValueError):self.run_extended(latent_correlation=itertools.repeat(1))
        r=self.run_extended(latent_correlation=iter([1,0,0,1]))
        self.assertEqual(r.latent_rank,2)

    def test_all_six_correlated_marginals(self):
        parameters=[P('normal',0,1),P('uniform',-2,3),P('log_uniform',1,10),P('log_normal',.1,.2),P('fixed',7),P('discrete_uniform',-3,7)]
        r=self.run_extended(parameters,6,latent_correlation=[1]*36,samples=128)
        for e in r.result.history:
            x=e.parameters;u=(1+math.erf(x[0]/math.sqrt(2)))/2
            expected=(x[0],-2+5*u,10**u,math.exp(.1+.2*x[0]),7,-3+min(10,math.floor(u*11)))
            for a,b in zip(x,expected):self.assertAlmostEqual(a,b,delta=1e-12)
        self.assertEqual(r.result.random_words,128*12)
        self.assertEqual(r.latent_rank,1)

    def test_rf_correlated_resistors(self):
        def evaluate(x):
            with self.lib.network() as network:
                for value in x:network.add(self.lib.ideal_rlc(1e9,element='resistor',connection='series',value=value))
                network.connect(1,2);s=network.external_s([0,3])
            return [abs(s[1][0])**2,abs(s[0][0])**2]
        r=self.run_extended([P('normal',50,2)]*2,evaluate=evaluate,samples=4096,seed=20261012,
            latent_correlation=[1,.8,.8,1],criteria=[rfmodel.MonteCarloLimits(lower=.25),rfmodel.MonteCarloLimits(upper=.25)])
        self.assertEqual((r.result.accepted_samples,r.result.passed_samples,r.result.random_words),(4096,2066,16384))
        for e in r.result.history:
            total=sum(e.parameters)
            for a,b in zip(e.values,[(100/(100+total))**2,(total/(100+total))**2]):self.assertAlmostEqual(a,b,delta=1e-12)
        self.check_joint(r.parameter_joint,[e.parameters for e in r.result.history])
        self.check_joint(r.metric_joint,[e.values for e in r.result.history])

    def test_version_two_task_matches_direct_api(self):
        d=task(128);before=copy.deepcopy(d)
        r=analyze_monte_carlo(self.lib,d)
        self.assertEqual(d,before)
        self.assertEqual((r['version'],r['result']['latent_rank'],r['analysis_evaluations']),(2,2,128))
        direct=self.run_extended([P('normal',50,2)]*2,samples=128,seed=20261012,latent_correlation=[1,.8,.8,1])
        self.assertEqual(r['result']['parameter_joint'],direct.parameter_joint._asdict())
        self.assertEqual(r['result']['effective_latent_correlation'],direct.effective_latent_correlation)
        for e,native in zip(r['result']['history'],direct.result.history):self.assertEqual(e['parameters'],native.parameters)
        self.assertEqual(r,analyze_monte_carlo(self.lib,d))
        json.dumps(r,allow_nan=False)

    def test_version_one_shape_and_version_two_defaults(self):
        d=load(ROOT/'examples/monte-carlo-resistor-tolerance.json');d['options']['samples']=8
        v1=analyze_monte_carlo(self.lib,d)
        self.assertEqual(len(v1['result']),19)
        self.assertNotIn('latent_correlation',v1['options'])
        d['version']=2
        v2=analyze_monte_carlo(self.lib,d)
        for key,value in v1['result'].items():self.assertEqual(value,v2['result'][key])
        self.assertEqual(v2['result']['metric_joint'],rfmodel.MonteCarloJointStatistics(0,0,(),())._asdict())
        d['version']=1;d['options']['joint_statistics']=True
        with self.assertRaises(ValueError):analyze_monte_carlo(self.lib,d)
        for version in (True,0,3,1.0):
            d=task();d['version']=version
            with self.assertRaises(ValueError):analyze_monte_carlo(self.lib,d)

    def test_task_freezes_version_and_dependence(self):
        d=task(4);before=copy.deepcopy(d);operation=analysis_operation(d['analysis'])
        def changed(*args,**kwargs):
            d['version']=1;d['options']['latent_correlation'][:]=[1,0,0,1]
            d['options']['joint_statistics']=False
            return operation(*args,**kwargs)
        with patch('rfmodel.monte_carlo_file.analysis_operation',return_value=changed):r=analyze_monte_carlo(self.lib,d)
        self.assertEqual(r,analyze_monte_carlo(self.lib,before))

    def test_task_invalid_matrix_and_no_extra_analyses(self):
        d=task(3);operation=analysis_operation(d['analysis']);calls=[]
        def counted(*args,**kwargs):calls.append(1);return operation(*args,**kwargs)
        with patch('rfmodel.monte_carlo_file.analysis_operation',return_value=counted):
            r=analyze_monte_carlo(self.lib,d)
            self.assertEqual(len(calls),3)
            for bad in ([1],[[1,0],[0,1]],(1,0,0,1),[1,.2,.3,1]):
                d['options']['latent_correlation']=bad
                with self.assertRaises((ValueError,TypeError,rfmodel.RFModelError)):analyze_monte_carlo(self.lib,d)
                self.assertEqual(len(calls),3)
        self.assertEqual(r['result']['metric_joint']['count'],3)

    def test_version_two_all_nine_analyses(self):
        names=('affine-mixer-network.json','amplifier-chain.json','clocked-coherent-combiner.json',
               'coherence.json','coherent-amplifier-cascade.json','limited-single-tone.json',
               'mixer-image-noise.json','mixer-lo-phase-noise.json','multitone-amplifier.json')
        def first(value,path=()):
            if type(value) in (int,float) and math.isfinite(value):return list(path),value
            children=value.items() if isinstance(value,dict) else enumerate(value) if isinstance(value,list) else ()
            for key,child in children:
                if key=='version':continue
                found=first(child,path+(key,))
                if found is not None:return found
        formats=set()
        for name in names:
            model=load(ROOT/'examples'/name);formats.add(model['format'])
            path,value=first(model);metric,expected=first(analyze_document(self.lib,model,base_directory=ROOT/'examples'))
            distribution=dict(kind='discrete_uniform',lower=value,upper=value) if type(value) is int else dict(kind='fixed',value=value)
            d=dict(format='rfmodel.monte-carlo',version=2,analysis=model,
                parameters=[dict(id='fixed',distribution=distribution,bindings=[dict(path=path)])],
                metrics=[dict(id='metric',path=metric)],options=dict(samples=2,latent_correlation=[1],joint_statistics=True))
            r=analyze_monte_carlo(self.lib,d,base_directory=ROOT/'examples')
            self.assertEqual((r['analysis_evaluations'],r['result']['random_words']),(2,4))
            self.assertEqual(r['result']['statistics'][0]['mean'],expected)
            self.assertEqual(r['result']['metric_joint']['sample_covariance'],(0,))
            self.assertEqual(r['result']['metric_joint']['sample_correlation'],(None,))
        self.assertEqual(len(formats),9)

    def test_cli_v2_roundtrip_protection_and_failed_output(self):
        with tempfile.TemporaryDirectory() as folder:
            folder=Path(folder);source=folder/'task.json';output=folder/'result.json';alias=folder/'alias.json'
            d=task(8);source.write_text(json.dumps(d),encoding='utf-8')
            env=dict(os.environ) if INSTALLED else dict(os.environ,PYTHONPATH=str(ROOT/'python'))
            cmd=[sys.executable,'-m','rfmodel',str(source),'--library',str(LIBRARY_PATH),'--output']
            run=subprocess.run(cmd+[str(output)],env=env,capture_output=True,text=True)
            self.assertEqual(run.returncode,0,run.stderr)
            self.assertEqual(json.loads(output.read_text()),json.loads(json.dumps(analyze_monte_carlo(self.lib,d))))
            alias.hardlink_to(source);original=source.read_bytes()
            self.assertNotEqual(subprocess.run(cmd+[str(alias)],env=env,capture_output=True).returncode,0)
            self.assertEqual(source.read_bytes(),original)
            d['options']['latent_correlation']=[1,1.1,1.1,1];source.write_text(json.dumps(d),encoding='utf-8')
            saved=output.read_bytes()
            self.assertNotEqual(subprocess.run(cmd+[str(output)],env=env,capture_output=True).returncode,0)
            self.assertEqual(output.read_bytes(),saved)
            d=task(2);d['metrics'][0]['path']=['missing'];source.write_text(json.dumps(d),encoding='utf-8')
            run=subprocess.run(cmd+[str(output)],env=env,capture_output=True,text=True)
            self.assertEqual(run.returncode,0,run.stderr)
            failed=json.loads(output.read_text());self.assertEqual(failed['result']['stop'],'evaluation_failure')
            self.assertEqual(failed['result']['metric_joint']['sample_covariance'],[None]*4)

    def test_v2_touchstone_dependency_change_and_output_protection(self):
        with tempfile.TemporaryDirectory() as folder:
            folder=Path(folder);data=folder/'data.s2p';source=folder/'task.json';alias=folder/'alias.s2p'
            raw=b'# GHz S RI R 50\n1 0 0 .5 0 .5 0 0 0\n';data.write_bytes(raw)
            d=dict(format='rfmodel.monte-carlo',version=2,
                analysis=dict(format='rfmodel.linear-network',version=1,frequencies_hz=[1e9],
                    devices=[dict(id='s',model=dict(type='touchstone',path='data.s2p'))],external_ports=[['s',0],['s',1]]),
                parameters=[dict(id='f',distribution=dict(kind='fixed',value=1e9),bindings=[dict(path=['frequencies_hz',0])])],
                metrics=[dict(id='power',path=['samples',0,'s',1,0],component='power')],
                options=dict(samples=2,latent_correlation=[1],joint_statistics=True))
            self.assertEqual(referenced_touchstone_paths(d,folder),{data.resolve()})
            r=analyze_monte_carlo(self.lib,d,base_directory=folder)
            self.assertEqual(len(r['dependencies']),1)
            self.assertEqual(r['result']['statistics'][0]['mean'],.25)
            def changed():data.write_bytes(raw+b'!changed\n');return False
            with self.assertRaisesRegex(RuntimeError,'Touchstone input changed'):analyze_monte_carlo(self.lib,d,base_directory=folder,cancelled=changed)
            data.write_bytes(raw);source.write_text(json.dumps(d),encoding='utf-8');alias.hardlink_to(data)
            env=dict(os.environ) if INSTALLED else dict(os.environ,PYTHONPATH=str(ROOT/'python'))
            run=subprocess.run([sys.executable,'-m','rfmodel',str(source),'--library',str(LIBRARY_PATH),'--output',str(alias)],env=env,capture_output=True)
            self.assertNotEqual(run.returncode,0);self.assertEqual(data.read_bytes(),raw)


if __name__=='__main__':unittest.main()
