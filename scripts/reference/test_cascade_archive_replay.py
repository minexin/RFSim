"""Cold, immutable eight-capture replay and identity/observation negative controls."""
import argparse
import copy
import hashlib
import json
import math
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

parser=argparse.ArgumentParser(add_help=False)
parser.add_argument('--native-probe',type=Path,required=True)
ARGS,remaining=parser.parse_known_args()
PROBE=ARGS.native_probe.resolve()
sys.argv[1:]=remaining
HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[1]
sys.path.insert(0,str(HERE))
import replay_cascade_archives as facade
from native_cascade_replay import NativeCascadeReplay
from native_replay import NativeReplayError, atomic_write_report


def captures():
    return json.loads((ROOT/'validation'/facade.CAPTURES).read_bytes())


class ArchiveTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.report=facade.replay(PROBE)
        cls.captures=captures()

    def test_exact_joint_results_and_separate_scopes(self):
        s=self.report['summary']
        for key,value in dict(capture_count=8,check_count=230,passed_count=208,failed_count=22,
            raw_path_check_count=158,combined_check_count=56,drive_check_count=16,
            power_only_failed_count=10,unscored_port3_origin_count=43).items():
            self.assertEqual(s[key],value,key)
        self.assertFalse(s['vendor_passed'])
        self.assertFalse(self.report['comparison']['status_differences'])
        rows=self.report['comparison']['rows']
        self.assertEqual([sum(not r['current']['passed'] for r in rows if r['capture_index']==i) for i in range(1,9)], [11,11,0,0,0,0,0,0])
        failed=[r for r in rows if not r['current']['passed']]
        self.assertEqual(sum(r['identity']['stage']==2 for r in failed),14)
        self.assertEqual(sum(r['identity']['stage']=='combined' for r in failed),8)
        self.assertEqual(sum('identity' in r['identity'] and r['identity']['stage']==1 for r in rows),56)
        self.assertEqual(sum('identity' in r['identity'] and r['identity']['stage']==2 for r in rows),102)

    def test_eight_diagnostic_assumptions_and_unscored_origins(self):
        self.assertEqual(self.report['eligibility'],'diagnostic_only')
        self.assertFalse(self.report['eligible_for_full_model_compatibility'])
        for case in self.report['reports']['reports']:
            self.assertEqual(case['spectrum_reduction'],facade.ASSUMPTION)
            self.assertEqual(case['eligibility'],'diagnostic_only')
        excluded=self.report['unscored_port3_origins']
        self.assertEqual(len(excluded),43)
        self.assertNotIn('cascade-02',{r['case_id'] for r in excluded})
        for row in excluded:
            self.assertEqual(row['direction'],'unknown')
            self.assertFalse(row['measured_zero'])
            self.assertTrue(row['identity'].endswith(',RFAmp2,RFAmp2'))
            self.assertTrue(row['boundary_records'])

    def test_provenance_hashes_and_unverified_originals(self):
        claims=self.report['archive_identity']
        raw=(ROOT/'validation'/facade.CAPTURES).read_bytes()
        self.assertEqual(claims['available_capture_bytes_sha256'],hashlib.sha256(raw).hexdigest())
        self.assertEqual(claims['historical_capture_sha256'],hashlib.sha256(raw.replace(b'\n',b'\r\n')).hexdigest())
        self.assertNotEqual(claims['historical_capture_sha256'],claims['available_capture_bytes_sha256'])
        for key in ('original_raw_verified','original_workspace_verified','original_library_verified'):
            self.assertFalse(claims[key])
        for key in ('raw_capture_claims','workspace_claims','historical_library_claims'):
            self.assertTrue(all(r['verification']=='historical_declared_unverified' for r in claims[key]))
        encoded=json.dumps(self.report)
        self.assertIn('canonical_request',encoded)
        self.assertIn('response_sha256',encoded)
        self.assertIn(hashlib.sha256(PROBE.read_bytes()).hexdigest(),encoded)

    def test_full_identity_join_not_row_order_and_actual_differences(self):
        current=copy.deepcopy(self.report['reports'])
        current['reports'].reverse()
        for c in current['reports']: c['checks'].reverse()
        result=facade._comparison(current,self.report['historical_reports'],self.captures)
        self.assertEqual(len(result['rows']),230)
        self.assertFalse(result['status_differences'])
        check=current['reports'][0]['checks'][0]
        check['passed']=not check['passed']
        result=facade._comparison(current,self.report['historical_reports'],self.captures)
        self.assertEqual(len(result['status_differences']),1)

    def test_missing_duplicate_identity_configuration_and_observation_rejected(self):
        for mutation in ('missing','duplicate','identity','width','configuration','observation','tolerance'):
            current=copy.deepcopy(self.report['reports'])
            c=current['reports'][0]
            check=c['checks'][0]
            if mutation=='missing': c['checks'].pop()
            elif mutation=='duplicate': c['checks'][1]=copy.deepcopy(check)
            elif mutation=='identity': check['identity']+='x'
            elif mutation=='width': check['bandwidth_hz']=7
            elif mutation=='configuration': c['configuration']['second_gain_db']=11
            elif mutation=='observation': check['systemvue_wave'][1]*=1.01
            else: current['relative_tolerance']=1e-6
            with self.subTest(mutation=mutation),self.assertRaises(ValueError):
                facade._comparison(current,self.report['historical_reports'],self.captures)

    def test_full_signed_multisets_and_narrow_channel_correction(self):
        origins=self.report['source_identity_mapping'][0]['origins']
        cubic=[o for o in origins if o['signed_source_factors']==[-1,1,1]]
        self.assertTrue(cubic)
        self.assertTrue(all(o['kind']==2 and o['bandwidth_hz']==3 for o in cubic))
        narrow=self.report['source_identity_mapping'][4]['origins']
        self.assertTrue(all(math.isclose(o['measured_channel_fraction'],1/o['bandwidth_hz']) for o in narrow))
        self.assertTrue(all(c['passed'] for c in self.report['reports']['reports'][4]['checks']))


    def test_real_waves_power_only_failures_and_complex_only_wrong_gate(self):
        rows=self.report['comparison']['rows']
        spectral=[r['current'] for r in rows if 'relative_complex_error' in r['current']]
        self.assertEqual(sum(not c['passed'] for c in spectral),22)
        self.assertEqual(sum(c['relative_complex_error']>1e-7 for c in spectral),12)
        power_only=[c for c in spectral if not c['passed'] and c['relative_complex_error']<=1e-7]
        self.assertEqual(len(power_only),10)
        for check in power_only:
            wave=complex(*check['systemvue_wave'])
            self.assertGreater(abs(wave),0)
            perturbed=wave*(1+7e-8)
            scored=facade._joint(perturbed,wave)
            self.assertLess(scored['relative_complex_error'],1e-7)
            self.assertGreater(abs(scored['relative_power_error']),1e-7)
            self.assertFalse(scored['passed'])
            self.assertTrue(facade._joint(wave*(1+4e-8),wave)['passed'])

    def test_numeric_delta_uses_common_fields_not_added_wave_evidence(self):
        comparison=self.report['comparison']
        expected_rows=0
        expected_fields=0
        for row in comparison['rows']:
            a,b=row['current'],row['historical']
            numeric=row['delta']['numeric']
            self.assertTrue(set(numeric)<=set(a)&set(b))
            if row['identity']['stage']=='combined':
                self.assertNotIn('systemvue_wave',numeric)
                self.assertNotIn('rfmodel_wave',numeric)
                self.assertNotIn('signed_source_factors',numeric)
            expected_rows+=bool(numeric)
            expected_fields+=len(numeric)
        self.assertEqual(self.report['summary']['common_numeric_difference_row_count'],expected_rows)
        self.assertEqual(self.report['summary']['common_numeric_difference_field_count'],expected_fields)

class MutationTests(unittest.TestCase):
    def test_nonempty_and_unique_direct_scorer_inputs(self):
        provider=NativeCascadeReplay(PROBE)
        original=captures()
        with mock.patch.object(provider,'two_stage',side_effect=AssertionError('empty scorer launched')):
            for invalid in ([],None,()):
                with self.assertRaisesRegex(ValueError,'Nonempty capture list'):
                    facade.compare(provider,invalid)
        with self.assertRaisesRegex(ValueError,'Duplicate configuration'):
            facade.compare(provider,[original[0],original[0]])

    def test_omitted_narrow_channel_wave_correction_fails(self):
        provider=NativeCascadeReplay(PROBE)
        report,_,_=facade.compare(provider,[captures()[4]])
        checked=0
        for check in report['reports'][0]['checks']:
            if 'identity' not in check or check['bandwidth_hz']==1:
                continue
            whole=complex(*check['systemvue_wave'])
            native=complex(*check['rfmodel_wave'])
            uncorrected_boundary=whole/math.sqrt(check['bandwidth_hz'])
            self.assertTrue(facade._joint(native,whole)['passed'])
            self.assertFalse(facade._joint(native,uncorrected_boundary)['passed'])
            checked+=1
        self.assertEqual(checked,9)

    def test_unscored_port3_mutations_never_claim_full_port_verification(self):
        checker=facade._checker()
        for mutation in ('rename','delete'):
            capture=captures()[0]
            ids=checker.vector(capture,'IDNo');names=checker.vector(capture,'IDName')
            pos=next(i for i,n in enumerate(names) if n.endswith(',RFAmp2,RFAmp2'))
            identifier=ids[pos]
            if mutation=='rename':
                names[pos]=names[pos].replace('Source.Source1','Source.UnscoredAltered')
            else:
                active=checker.vector(capture,'ID3')
                keep=[i for i,x in enumerate(active) if x!=identifier]
                for field in ('F3','P3','ID3','V3','Z3'):
                    node=checker.node(capture,checker.SPECTRUM+field)
                    values=node['data']
                    node['data']=([values[2*i+j] for i in keep for j in (0,1)] if field in ('V3','Z3') else [values[i] for i in keep])
                    node['dimensions']=[len(node['data'])]
            report,_,unscored=facade.compare(NativeCascadeReplay(PROBE),[capture])
            self.assertFalse(report['full_port_spectrum_verified'])
            self.assertEqual(report['eligibility'],'diagnostic_only')
            self.assertEqual(report['unscored_port3_origin_count'],len(unscored))
            self.assertEqual(len(unscored),3 if mutation=='rename' else 2)
            self.assertTrue(all(r['direction']=='unknown' for r in unscored))
            self.assertEqual(report['reports'][0]['spectrum_reduction'],facade.ASSUMPTION)
            if mutation=='rename': self.assertTrue(any('UnscoredAltered' in r['identity'] for r in unscored))

    def test_generic_secondary_generation_from_distortion_is_rejected(self):
        provider=NativeCascadeReplay(PROBE)
        original=provider.two_stage
        def secondary(*args,**kwargs):
            result=original(*args,**kwargs)
            index=next(i+1 for i,c in enumerate(result.second.inputs) if c.kind!=facade.SpectrumKind.SOURCE)
            term=result.second.terms[-1]._replace(order=2,input_indices=(index,index))
            return result._replace(second=result.second._replace(terms=result.second.terms+(term,)))
        with mock.patch.object(provider,'two_stage',side_effect=secondary):
            with self.assertRaisesRegex(ValueError,'Secondary distortion generation'):
                facade.compare(provider,captures()[:1])

    def test_joint_power_error_is_not_hidden_by_complex_pass(self):
        check=facade._joint(1+7e-8,1+0j)
        self.assertLess(check['relative_complex_error'],1e-7)
        self.assertGreater(abs(check['relative_power_error']),1e-7)
        self.assertFalse(check['passed'])

    def test_fixed_archive_coverage_and_assumption_cannot_be_upgraded(self):
        original=captures()
        checker=facade._checker()
        for values in ([],original[:-1],original[::-1],original[:7]+original[:1]):
            with self.assertRaises(ValueError): facade._validate_cases(values,checker)
        original[0]['spectrum_reduction']=True
        with self.assertRaises(ValueError): facade._validate_cases(original,checker)

    def test_combined_identity_multiplicity_missing_extra_and_phase(self):
        provider=NativeCascadeReplay(PROBE)
        original=provider.two_stage
        for mutation in ('missing','duplicate','cancel_factors','phase'):
            def corrupt(*args,**kwargs):
                result=original(*args,**kwargs)
                records=list(result.combined)
                if mutation=='missing': records.pop()
                elif mutation=='duplicate': records.append(records[0])
                elif mutation=='cancel_factors':
                    pos=next(i for i,r in enumerate(records) if sorted(r.factors)==[-1,1,1])
                    records[pos]=records[pos]._replace(factors=(1,))
                else: records[0]=records[0]._replace(component=records[0].component._replace(amplitude=-records[0].component.amplitude))
                return result._replace(combined=tuple(records))
            with self.subTest(mutation=mutation),mock.patch.object(provider,'two_stage',side_effect=corrupt):
                if mutation=='phase':
                    report,_,_=facade.compare(provider,captures()[:1])
                    self.assertFalse(report['passed'])
                    self.assertGreater(next(c['relative_complex_error'] for c in report['reports'][0]['checks'] if c['stage']=='combined'),1)
                else:
                    with self.assertRaises(ValueError): facade.compare(provider,captures()[:1])

    def test_archived_observation_units_shapes_and_readbacks_rejected(self):
        checker=facade._checker()
        for mutation in ('power','voltage','impedance','boundary','shape','gain','topology','warning','missing'):
            capture=captures()[0]
            if mutation=='warning': capture['manager_errors']='(WARNING) unexpected failure'
            elif mutation=='topology': checker.node(capture,'Design3/PartList/RFAmp2')['netlist']='wrong'
            elif mutation=='gain': checker.node(capture,'Design3/PartList/RFAmp2/ParamSet/G')['data']=2
            elif mutation=='missing': capture['nodes']=[n for n in capture['nodes'] if not n['path'].endswith('/V2')]
            elif mutation=='shape': checker.node(capture,checker.SPECTRUM+'V2')['dimensions']=[1]
            else:
                field={'power':'P2','voltage':'V2','impedance':'Z2','boundary':'F2'}[mutation]
                ids=checker.vector(capture,'IDNo');names=checker.vector(capture,'IDName')
                identity=next(i for i,n in zip(ids,names) if n.endswith('D[(Source.Source1)],Source,RFAmp1,RFAmp2'))
                row=checker.vector(capture,'ID2').index(identity)
                checker.vector(capture,field)[row*2+1 if field=='V2' else row*2 if field=='Z2' else row]*=1.01
            with self.subTest(mutation=mutation),self.assertRaises((ValueError,KeyError)):
                facade.compare(NativeCascadeReplay(PROBE),[capture])


class ColdTests(unittest.TestCase):
    def run_cli(self,script,*args):
        return subprocess.run([sys.executable,'-I','-S',str(script),*map(str,args)],capture_output=True,text=True,timeout=90)

    def test_cold_closure_mutations_rejected_before_report(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);scripts=root/'scripts/reference';scripts.mkdir(parents=True)
            (root/'validation').mkdir()
            for p in HERE.glob('*.py'): shutil.copyfile(p,scripts/p.name)
            for n in facade.ARCHIVE_SHA256: shutil.copyfile(ROOT/'validation'/n,root/'validation'/n)
            for n in facade.FROZEN_SHA256:
                path=scripts/n; original=path.read_bytes();path.write_bytes(original+b'\n# cold mutation\n')
                output=root/(n+'.json')
                result=self.run_cli(scripts/'replay_cascade_archives.py','--native-probe',PROBE,'--report',output)
                with self.subTest(dependency=n):
                    self.assertNotEqual(result.returncode,0)
                    self.assertIn('Frozen cascade checker/support bytes changed',result.stderr)
                    self.assertFalse(output.exists())
                path.write_bytes(original);shutil.rmtree(scripts/'__pycache__',ignore_errors=True)

    def test_new_path_cli_and_frozen_archives(self):
        before={n:(ROOT/'validation'/n).read_bytes() for n in facade.ARCHIVE_SHA256}
        with tempfile.TemporaryDirectory() as tmp:
            output=Path(tmp)/'replay.json'
            result=self.run_cli(HERE/'replay_cascade_archives.py','--native-probe',PROBE,'--report',output)
            self.assertEqual(result.returncode,0,result.stderr)
            saved=output.read_bytes()
            result=self.run_cli(HERE/'replay_cascade_archives.py','--native-probe',PROBE,'--report',output)
            self.assertNotEqual(result.returncode,0)
            self.assertEqual(saved,output.read_bytes())
            report=json.loads(saved);self.assertFalse(report['summary']['vendor_passed'])
        self.assertEqual(before,{n:(ROOT/'validation'/n).read_bytes() for n in before})

    def test_archive_byte_drift_and_manifest_drift_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);(root/'validation').mkdir()
            for n in facade.ARCHIVE_SHA256: shutil.copyfile(ROOT/'validation'/n,root/'validation'/n)
            path=root/'validation'/facade.CAPTURES;path.write_bytes(path.read_bytes()+b' ')
            with self.assertRaisesRegex(ValueError,'Frozen cascade archive bytes'): facade.replay(PROBE,root=root)
        original=NativeCascadeReplay.manifest
        def corrupt(provider,*args,**kwargs):
            result=original(provider,*args,**kwargs)
            hashes=result['available_archive_bytes_sha256'];hashes[next(iter(hashes))]='f'*64
            return result
        with mock.patch.object(NativeCascadeReplay,'manifest',corrupt),self.assertRaises(NativeReplayError): facade.replay(PROBE)

    def test_cold_no_product_or_ctypes_import(self):
        driver='''import importlib.abc,sys,pathlib
class Ban(importlib.abc.MetaPathFinder):
 def find_spec(self,fullname,path=None,target=None):
  if fullname=='rfmodel' or fullname.startswith('rfmodel.') or fullname in ('ctypes','ctypes.util','_ctypes'): raise AssertionError(fullname)
sys.meta_path.insert(0,Ban())
sys.path.insert(0,sys.argv[1])
import replay_cascade_archives as f
r=f.replay(pathlib.Path(sys.argv[2]))
assert r['summary']['check_count']==230
'''
        result=subprocess.run([sys.executable,'-I','-S','-c',driver,str(HERE),str(PROBE)],capture_output=True,text=True,timeout=90)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)


if __name__=='__main__': unittest.main()
