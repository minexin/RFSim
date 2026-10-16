"""Independent long-double Fourier cascade oracle and bounded protocol controls.

The oracle never uses RFModel. Raw stage paths and combined primitive origins
are compared separately; numerical local group identifiers are not SourceN.
"""
import argparse
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

parser = argparse.ArgumentParser(add_help=False)
parser.add_argument('--native-probe', type=Path, required=True)
parser.add_argument('--oracle', type=Path, required=True)
parser.add_argument('--fault-oracle', type=Path)
ARGS, remaining = parser.parse_known_args()
sys.argv[1:] = remaining
sys.path.insert(0, str(Path(__file__).resolve().parent))
from native_cascade_replay import NativeCascadeReplay, MAGIC, MAX_BYTES
from native_replay import NativeReplayError, forbid_product_imports, assert_no_product_imports
from reference_records import CoherentComponent as C, SpectrumKind as K
forbid_product_imports()
PROBE = ARGS.native_probe.resolve()
RID = '0123456789abcdef' * 4
OP = 'cubic_two_stage_v1'
PAYLOAD = '1e8 10 2 10 0 1 17 .003 .004 11 0 1 5 -.002 .001'
COMPONENTS = (C(10,K.SOURCE,1.,17,.003+.004j), C(11,K.SOURCE,1.,5,-.002+.001j))


def request(payload=PAYLOAD, op=OP):
    return f'{MAGIC} 1 {op} {RID}\n{payload}\n'.encode()


def run(data):
    return subprocess.run([str(PROBE)], input=data, capture_output=True, timeout=10)


def call(provider):
    return provider.two_stage(1e8, COMPONENTS, second_gain_db=10.)


class Reader:
    def __init__(self, tokens):
        self.tokens = tokens
        self.pos = 0
    def take(self):
        result = self.tokens[self.pos]
        self.pos += 1
        return result
    def integer(self):
        return int(self.take())
    def number(self):
        return float(self.take())
    def component(self):
        return C(self.integer(), K(self.integer()), self.number(), self.integer(),
                 complex(self.number(), self.number()))
    def stage(self):
        drive, limited = self.number(), self.number()
        inputs = tuple(self.component() for _ in range(self.integer()))
        terms = []
        for _ in range(self.integer()):
            order = self.integer()
            indices = tuple(self.integer() for _ in range(order))
            terms.append((order, indices, self.component()))
        return drive, limited, inputs, terms


class WireTests(unittest.TestCase):
    def reject(self, data):
        result = run(data)
        self.assertIn(result.returncode, (2,3,4,5), result)
        self.assertEqual(result.stderr, b'')
        tokens = result.stdout.split()
        self.assertEqual(tokens[:3], [MAGIC.encode(), b'1', b'error'])
        self.assertEqual(len(tokens), 6)
        self.assertEqual(result.returncode, {b'invalid_input':2,b'resource_limit':3,
                                            b'numeric_range':4,b'internal_error':5}[tokens[5]])

    def test_binary_wire_boundaries(self):
        data = request()
        result = run(data)
        self.assertEqual(result.returncode, 0, result)
        self.assertEqual(result.stderr, b'')
        self.assertNotIn(b'\r', result.stdout)
        self.assertEqual(result.stdout, run(data).stdout)
        self.assertEqual(result.stdout, run(data.replace(b'\n',b'\r\n')).stdout)
        self.reject(data+b'\x1a hidden')
        padded = data+b' '*(MAX_BYTES-len(data))
        self.assertEqual(run(padded).returncode, 0)
        self.reject(padded+b' ')
        self.reject(data+b'\r\n'*((MAX_BYTES-len(data))//2+1))

    def test_headers_truncation_extra_and_legacy_operations(self):
        data = request()
        for bad in (data+b'extra',data+b'\x00',data+b'\xff',
                    data.replace(MAGIC.encode(),b'RFMODEL_NATIVE_REPLAY'),
                    data.replace(MAGIC.encode(),b'RFMODEL_SOURCE_ARCHIVE_REPLAY'),
                    data.replace(b' 1 ',b' 2 ',1),data.replace(RID.encode(),b'wrong')):
            self.reject(bad)
        for op in ('source_groups_v1','shared_fundamentals_v1','cubic_sources_v1',
                   'polynomial','highorder','am_pm'):
            self.reject(request(op=op))
        words = data.split()
        for n in range(len(words)):
            self.reject(b' '.join(words[:n]))

    def test_all_numeric_grammar_and_range(self):
        tokens = PAYLOAD.split()
        for index in range(len(tokens)):
            for value in ('nan','inf','-inf','1e999','1e-999','true','0x1p0','1'*129):
                changed=tokens[:];changed[index]=value
                with self.subTest(index=index,value=value):
                    self.reject(request(' '.join(changed)))
        for index,value in ((0,'1'),(1,'-1'),(1,'21'),(2,'0'),(2,'3'),(2,'18446744073709551616'),
                            (3,'9'),(4,'1'),(5,'2'),(6,'0'),(6,'18446744073709551616'),
                            (7,'1e308'),(9,'10'),(12,'17')):
            changed=tokens[:];changed[index]=value
            self.reject(request(' '.join(changed)))
        for wave in ('0 0','1e-200 0','1e-150 0'):
            self.reject(request('1e8 10 1 10 0 1 1 '+wave))


class OracleTests(unittest.TestCase):
    def assert_scalar(self, actual, expected, label):
        self.assertTrue(math.isclose(actual, expected, rel_tol=4e-12, abs_tol=1e-300),
                        (label, actual, expected))

    def assert_component(self, actual, expected, label, groups=True):
        self.assertEqual((actual.bin,actual.kind,actual.bandwidth_hz),
                         (expected.bin,expected.kind,expected.bandwidth_hz),label)
        if groups:
            self.assertEqual(actual.coherence_group,expected.coherence_group,label)
        self.assertLessEqual(abs(actual.amplitude-expected.amplitude),
                             4e-12*max(abs(expected.amplitude),1e-300),label)

    def test_independent_fourier_all_raw_paths_and_combined(self):
        result=subprocess.run([str(ARGS.oracle.resolve())],capture_output=True,timeout=20)
        self.assertEqual(result.returncode,0,result.stderr)
        lines=result.stdout.decode().splitlines();header=lines.pop(0).split()
        self.assertEqual(header[:2],['RFMODEL_CASCADE_ARCHIVE_ORACLE','1'])
        self.assertEqual(len(lines),int(header[2]));self.assertGreaterEqual(len(lines),16)
        names=set();provider=NativeCascadeReplay(PROBE)
        for line in lines:
            words=line.split();name,op=words[:2];count=int(words[2]);payload=words[3:3+count]
            self.assertEqual(op,OP);self.assertNotIn(name,names);names.add(name)
            expected_count=int(words[3+count]);expected=words[4+count:]
            self.assertEqual(len(expected),expected_count)
            reader=Reader(payload);spacing,gain=reader.number(),reader.number()
            components=tuple(reader.component() for _ in range(reader.integer()))
            actual=provider.two_stage(spacing,components,second_gain_db=gain)
            reader=Reader(expected)
            stages=[]
            for stage in (actual.first,actual.second):
                drive,limited,inputs,terms=reader.stage();stages.append((drive,limited,inputs,terms))
                self.assert_scalar(stage.total_input_power_w,drive,name)
                self.assert_scalar(stage.limited_input_power_w,limited,name)
                self.assertEqual(len(stage.inputs),len(inputs),name)
                for a,b in zip(stage.inputs,inputs):self.assert_component(a,b,name)
                self.assertEqual(len(stage.terms),len(terms),name)
                for a,(order,indices,c) in zip(stage.terms,terms):
                    self.assertEqual(a.order,order,name)
                    self.assertEqual(tuple(a.input_indices)[:order],indices,name)
                    self.assert_component(a.component,c,name)
            combined={}
            for _ in range(reader.integer()):
                factors=tuple(reader.integer() for _ in range(reader.integer()))
                c=reader.component();combined[(c.bin,c.kind,c.bandwidth_hz,factors)]=c
            self.assertEqual(reader.pos,len(expected),name)
            observed={(r.component.bin,r.component.kind,r.component.bandwidth_hz,tuple(r.factors)):r.component
                      for r in actual.combined}
            self.assertEqual(len(observed),len(actual.combined),name)
            self.assertEqual(observed.keys(),combined.keys(),name)
            for key,c in observed.items():self.assert_component(c,combined[key],name,groups=False)
            # Root identity is checked against original input ordinal, not sorted
            # reduced-input order, group magnitude, or implementation root order.
            self.assertEqual(len(actual.roots),len(components),name)
            for root in actual.roots:
                source=components[root.source_ordinal-1]
                self.assertEqual((root.group,root.bin,root.bandwidth_hz,root.role),
                                 (source.coherence_group,source.bin,source.bandwidth_hz,0),name)
            self.assertEqual(len(actual.stage_stats),2,name)
            for index,stats in enumerate(actual.stage_stats):
                drive,limited,inputs,terms=stages[index]
                self.assert_scalar(stats.input_power_w,drive,name)
                self.assert_scalar(stats.limited_input_power_w,limited,name)
                self.assertEqual(stats.physical_input_count,sum(c.kind==K.SOURCE for c in inputs),name)
                self.assertEqual(stats.generated_term_count,sum(t[0]>1 for t in terms),name)
                self.assertEqual(stats.direct_contribution_count,len(inputs),name)
                self.assertEqual(stats.output_record_count,len(combined),name)
        self.assertTrue(any('reversed' in n for n in names))
        self.assertTrue(any('saturation' in n for n in names))


class ProviderTests(unittest.TestCase):
    def corrupt(self, mutate, code=None, stderr=None):
        native=NativeCascadeReplay(PROBE);original=native._run
        def altered(data):
            rc,out,err=original(data)
            return rc if code is None else code,mutate(out),err if stderr is None else stderr
        with mock.patch.object(native,'_run',side_effect=altered),self.assertRaises(NativeReplayError):
            call(native)
        self.assertEqual(native.exchanges,[])

    @staticmethod
    def field(index,value):
        def mutate(data):
            words=data.split();words[index]=value;return b' '.join(words)
        return mutate

    def test_response_envelope_transport_and_numeric(self):
        for i,value in ((0,b'wrong'),(1,b'2'),(2,b'wrong'),(3,b'0'*64),(4,b'0'*64),(5,b'invalid_input')):
            self.corrupt(self.field(i,value))
        for mutate in (lambda b:b+b' extra',lambda b:b.rsplit(None,1)[0],lambda b:b'',
                       lambda b:b+b'\xff',lambda b:b+b'\x00',lambda b:b' '*(MAX_BYTES+1)):
            self.corrupt(mutate)
        self.corrupt(lambda b:b,code=1);self.corrupt(lambda b:b,stderr=b'warning')
        for index in (6,7):
            for value in (b'-1',b'nan',b'inf',b'1e999',b'1e-999',b'true',b'1'*129):
                self.corrupt(self.field(index,value))
        for i,value in ((6,b'.01'),(7,b'.01'),(8,b'0'),(8,b'3'),(9,b'9'),(10,b'1'),
                        (11,b'2'),(12,b'0'),(13,b'.02')):
            self.corrupt(self.field(i,value))

    def test_stage_root_combined_and_stats_corruption(self):
        def positions(data):
            w=data.split();p=6;stages=[]
            for _ in range(2):
                start=p;p+=2;n=int(w[p]);p+=1+6*n
                count_at=p;n=int(w[p]);p+=1;terms=[]
                for _ in range(n):
                    begin=p;p+=1+int(w[p])+6;terms.append((begin,p))
                stages.append((start,count_at,terms))
            roots=p;p+=1+6*int(w[p]);combined=p;p+=1;records=[]
            for _ in range(int(w[combined])):
                begin=p;p+=1+int(w[p])+6;records.append((begin,p))
            return w,stages,roots,combined,records,p
        def mutate(kind):
            def change(data):
                w,stages,roots,combined,records,stats=positions(data)
                if kind=='missing-second':return b' '.join(w[:stages[1][0]])
                if kind=='missing-term':
                    _,count,terms=stages[1];a,b=terms[-1];w[count]=str(int(w[count])-1).encode();del w[a:b]
                elif kind=='duplicate-term':
                    terms=stages[1][2];a,b=terms[-1];c,d=terms[-2];w[a:b]=w[c:d]
                elif kind=='bad-parent':w[stages[1][2][-1][0]+1]=b'99'
                elif kind=='distortion-parent':w[stages[1][2][-1][0]+1]=b'1'
                elif kind=='root-count':w[roots]=b'0'
                elif kind=='combined-count':w[combined]=b'17'
                elif kind=='factor-count':w[records[-1][0]]=b'4'
                elif kind=='distortion-source':
                    start=stages[1][0];w[start+4]=b'0' # first second-stage input is difference-frequency intermod
                elif kind=='duplicate-root':w[roots+7:roots+13]=w[roots+1:roots+7]
                elif kind=='unknown-root':w[roots+1]=b'99'
                elif kind=='wrong-source-ordinal':w[roots+2]=b'2'
                elif kind=='unknown-factor':w[records[-1][0]+1]=b'99'
                elif kind=='cancel-pair':
                    target=next((a,b) for a,b in records if int(w[a])==3 and any(int(x)<0 for x in w[a+1:a+4]))
                    a,b=target;w[a:a+4]=[b'1',b'1']
                elif kind=='duplicate-combined':
                    a,b=records[-1];c,d=records[-2];w[a:b]=w[c:d]
                elif kind=='missing-combined':
                    a,b=records[-1];w[combined]=str(int(w[combined])-1).encode();del w[a:b]
                elif kind=='bad-stats-count':w[stats+8]=b'99'
                elif kind=='bad-stats-drive':w[stats+6]=b'.01'
                return b' '.join(w)
            return change
        for kind in ('missing-second','missing-term','duplicate-term','bad-parent','distortion-source',
                     'distortion-parent','root-count','combined-count','factor-count',
                     'duplicate-root','unknown-root','wrong-source-ordinal','unknown-factor','cancel-pair',
                     'duplicate-combined','missing-combined','bad-stats-count','bad-stats-drive'):
            with self.subTest(kind=kind):self.corrupt(mutate(kind))

    def test_invalid_python_requests_do_not_launch(self):
        native=NativeCascadeReplay(PROBE)
        bad_components=[[],COMPONENTS*2,[COMPONENTS[0]._replace(bin=True)],
                        [COMPONENTS[0]._replace(kind=K.HARMONIC)],
                        [COMPONENTS[0]._replace(amplitude=True)],
                        [COMPONENTS[0]._replace(amplitude=0)],
                        [COMPONENTS[0]._replace(amplitude=1e-200)],
                        [COMPONENTS[0]._replace(amplitude=complex(math.nan))],
                        [COMPONENTS[0]._replace(bandwidth_hz=2)],
                        [COMPONENTS[0]._replace(coherence_group=0)],
                        [COMPONENTS[0],COMPONENTS[1]._replace(coherence_group=17)]]
        with mock.patch.object(native,'_run',side_effect=AssertionError('invalid request launched')):
            for components in bad_components:
                with self.assertRaises((ValueError,TypeError)):
                    native.two_stage(1e8,components,second_gain_db=10.)
            for gain in (True,-1,21,math.nan):
                with self.assertRaises((ValueError,TypeError)):
                    native.two_stage(1e8,COMPONENTS,second_gain_db=gain)

    def test_error_envelopes_and_contradictory_exit(self):
        for status,code in (('invalid_input',2),('resource_limit',3),('numeric_range',4),('internal_error',5)):
            native=NativeCascadeReplay(PROBE)
            def envelope(data):
                rid=data.split()[3].decode();build=native.execution_identity['build_id']
                return code,f'{MAGIC} 1 error {rid} {build} {status}\n'.encode(),b''
            with mock.patch.object(native,'_run',side_effect=envelope),self.assertRaises(NativeReplayError) as caught:
                call(native)
            self.assertEqual(caught.exception.status,status)
            self.assertEqual(native.exchanges,[])
            def contradiction(data):
                _,out,err=envelope(data);return 0,out,err
            with mock.patch.object(native,'_run',side_effect=contradiction),self.assertRaises(NativeReplayError):call(native)

    def test_execution_identity_change(self):
        with tempfile.TemporaryDirectory() as directory:
            probe=Path(directory)/PROBE.name;shutil.copy2(PROBE,probe)
            manifest=Path(str(probe)+'.manifest.json');shutil.copy2(str(PROBE)+'.manifest.json',manifest)
            native=NativeCascadeReplay(probe);original=native._run
            def modified(data):
                answer=original(data);manifest.write_bytes(manifest.read_bytes()+b' ');return answer
            with mock.patch.object(native,'_run',side_effect=modified),self.assertRaises(NativeReplayError):call(native)
            self.assertEqual(native.exchanges,[])
            with self.assertRaises(NativeReplayError):call(native)
            with self.assertRaises(NativeReplayError):NativeCascadeReplay(probe)

    def test_actual_process_faults(self):
        if ARGS.fault_oracle is None:self.skipTest('fault executable not provided')
        for mode,status in (('timeout','timeout'),('stdout_limit','resource_limit'),
                            ('stderr_limit','resource_limit'),('nonzero','execution_failure')):
            with mock.patch.dict(os.environ,{'RFMODEL_REFERENCE_ORACLE_FAULT':mode}):
                native=NativeCascadeReplay(ARGS.fault_oracle.resolve(),manifest_path=str(PROBE)+'.manifest.json',
                                          timeout=.05 if mode=='timeout' else 10.)
                with self.assertRaises(NativeReplayError) as caught:call(native)
                self.assertEqual(caught.exception.status,status);self.assertEqual(native.exchanges,[])

    def test_old_domains_stay_closed(self):
        native=NativeCascadeReplay(PROBE)
        for op in ('polynomial','highorder','am_pm','source_groups_v1','shared_fundamentals_v1','cubic_sources_v1'):
            with self.assertRaises(ValueError):native._invoke(op,[])
        assert_no_product_imports()


if __name__=='__main__':unittest.main()
