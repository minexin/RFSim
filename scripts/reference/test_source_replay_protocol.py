"""Independent no-SDK Fourier oracle plus source protocol corruption controls."""
import argparse
import json
import math
import os
from pathlib import Path
import subprocess
import shutil
import tempfile
import sys
import unittest
from unittest import mock

p = argparse.ArgumentParser(add_help=False)
p.add_argument('--native-probe', type=Path, required=True)
p.add_argument('--oracle', type=Path, required=True)
p.add_argument('--fault-oracle', type=Path, required=True)
ARGS, rest = p.parse_known_args()
sys.argv[1:] = rest
sys.path.insert(0, str(Path(__file__).resolve().parent))
from native_source_replay import NativeSourceReplay, MAGIC, MAX_BYTES
from native_replay import NativeReplayError, forbid_product_imports, assert_no_product_imports
from reference_records import CoherentComponent as C, SourceCoherence as S, SpectrumKind as K
forbid_product_imports()
PROBE = ARGS.native_probe.resolve()
RID = '0123456789abcdef' * 4
COMPONENTS = (C(10,K.SOURCE,1.,1,.003+.004j), C(11,K.SOURCE,1.,2,-.002+.001j))
COMMON = dict(power_gain_db=20., output_p1db_dbm=20., output_saturation_dbm=23.)
PAYLOADS = {
 'source_groups_v1': '2 Source1 - Source2 -',
 'shared_fundamentals_v1': '1e8 20 20 23 2 10 0 1 1 .003 .004 11 0 1 2 -.002 .001',
 'cubic_sources_v1': '1e8 20 20 23 20 10 50 0 2 10 0 1 1 .003 .004 11 0 1 2 -.002 .001',
}

def request(op, payload):
    return f'{MAGIC} 1 {op} {RID}\n{payload}\n'.encode()

def run(data):
    return subprocess.run([str(PROBE)], input=data, capture_output=True, timeout=10)

def groups(provider):
    return provider.assign_source_coherence([S('Source1'),S('Source2')])

def shared(provider):
    return provider.compress_coherent_fundamentals(1e8, COMPONENTS, **COMMON)

def cubic(provider):
    return provider.coherent_amplifier(1e8, COMPONENTS, **COMMON, input_ip2_dbm=20.,input_ip3_dbm=10.)

class WireTests(unittest.TestCase):
    def reject(self, data):
        result = run(data)
        self.assertIn(result.returncode,(2,3,4,5), result)
        self.assertEqual(result.stderr,b'')
        words=result.stdout.split()
        self.assertEqual(words[:3],[MAGIC.encode(),b'1',b'error'])
        self.assertEqual(len(words),6)
        self.assertEqual(result.returncode, {b'invalid_input':2,b'resource_limit':3,b'numeric_range':4,b'internal_error':5}[words[5]])

    def test_all_operations_deterministic_binary_wire(self):
        for op,payload in PAYLOADS.items():
            data=request(op,payload)
            first=run(data)
            self.assertEqual(first.returncode,0,first)
            self.assertEqual(first.stderr,b'')
            self.assertNotIn(b'\r',first.stdout)
            self.assertEqual(first.stdout,run(data).stdout)
            self.assertEqual(first.stdout,run(data.replace(b'\n',b'\r\n')).stdout)
            self.reject(data+b'\x1a hidden')
            # Exactly64KiB accepted, one byte over rejected before token parsing.
            padded=data+b' '*(MAX_BYTES-len(data))
            self.assertEqual(run(padded).returncode,0)
            self.reject(padded+b' ')
            # On Windows text stdin would shrink this below the wire budget.
            self.reject(data+b'\r\n'*((MAX_BYTES-len(data))//2+1))

    def test_request_header_and_termination(self):
        for op,payload in PAYLOADS.items():
            data=request(op,payload)
            for mutation in (data+b'extra', data.replace(MAGIC.encode(),b'RFMODEL_NATIVE_REPLAY'),
                             data.replace(b' 1 ',b' 2 ',1),data.replace(op.encode(),b'cubic_two_stage_v1'),
                             data.replace(RID.encode(),b'wrong'),data+b'\x00',data+b'\xff'):
                self.reject(mutation)
            words=data.split()
            for n in range(len(words)):
                self.reject(b' '.join(words[:n]))

    def test_numeric_fields_bad_grammar_range_and_counts(self):
        for op,payload in PAYLOADS.items():
            if op=='source_groups_v1':
                continue
            words=payload.split()
            for at in range(len(words)):
                for bad in ('nan','inf','-inf','1e999','1e-999','true','0x1p0','1'*129):
                    changed=words[:];changed[at]=bad
                    with self.subTest(op=op,at=at,bad=bad):
                        self.reject(request(op,' '.join(changed)))
        for payload in ('0','3 A - B - C -','2 A - A clock','1 - -','1 A bad/clock',
                        '1 '+ 'a'*65 +' -','1 A', '18446744073709551616'):
            self.reject(request('source_groups_v1',payload))

    def test_source_only_and_support_guards(self):
        for op in ('shared_fundamentals_v1','cubic_sources_v1'):
            words=PAYLOADS[op].split(); start=5 if op.startswith('shared') else 9
            for at,bad in ((start,'0'),(start,'715827883'),(start+1,'1'),(start+2,'0'),
                           (start+2,'1e20'),(start+3,'0'),(start+3,'18446744073709551616'),
                           (start+4,'1e308')):
                altered=words[:];altered[at]=bad
                self.reject(request(op,' '.join(altered)))

    def test_derived_product_underflow_is_explicit_numeric_failure(self):
        result=run(request('cubic_sources_v1','1e8 20 20 23 20 10 50 0 1 10 0 1 1 1e-150 0'))
        self.assertEqual(result.returncode,4)
        self.assertEqual(result.stdout.split()[-1],b'numeric_range')
        self.assertEqual(result.stderr,b'')

    def test_shared_nonzero_wave_power_underflow_rejected(self):
        for amplitude in ('1e-162','1e-200'):
            result=run(request('shared_fundamentals_v1',f'1e8 20 20 23 1 10 0 1 1 {amplitude} 0'))
            self.assertEqual(result.returncode,4)
            self.assertEqual(result.stdout.split()[-1],b'numeric_range')

class OracleTests(unittest.TestCase):
    def test_independent_long_double_fourier_oracle(self):
        result=subprocess.run([str(ARGS.oracle.resolve())],capture_output=True,timeout=20)
        self.assertEqual(result.returncode,0,result.stderr)
        lines=result.stdout.decode().splitlines(); header=lines.pop(0).split()
        self.assertEqual(header[:2],['RFMODEL_SOURCE_ARCHIVE_ORACLE','1'])
        self.assertEqual(len(lines),int(header[2]));self.assertGreaterEqual(len(lines),46)
        names=set(); operations=set()
        for line in lines:
            words=line.split(); name,op=words[:2]; count=int(words[2])
            payload=words[3:3+count]; expected_count=int(words[3+count]);expected=words[4+count:]
            self.assertNotIn(name,names);names.add(name);operations.add(op)
            self.assertEqual(len(expected),expected_count)
            actual=run(request(op,' '.join(payload)))
            self.assertEqual(actual.returncode,0,(name,actual))
            received=actual.stdout.decode().split()[6:]
            self.assertEqual(len(received),len(expected),name)
            cursor = 0
            def exact():
                nonlocal cursor
                self.assertEqual(received[cursor], expected[cursor], (name,cursor))
                value = received[cursor]; cursor += 1
                return value
            def scalar():
                nonlocal cursor
                a,b=float(received[cursor]),float(expected[cursor]);cursor+=1
                self.assertTrue(math.isclose(a,b,rel_tol=3e-12,abs_tol=1e-300),(name,cursor,a,b))
            def component():
                nonlocal cursor
                exact(); exact(); scalar(); exact()
                a=complex(*map(float,received[cursor:cursor+2]))
                b=complex(*map(float,expected[cursor:cursor+2]));cursor+=2
                self.assertLessEqual(abs(a-b),3e-12*max(abs(b),1e-300),(name,cursor,a,b))
            if op == 'source_groups_v1':
                for _ in range(1+3*int(received[0])): exact()
            elif op == 'shared_fundamentals_v1':
                scalar()
                for _ in range(int(exact())): component()
                for _ in range(int(exact())): exact(); scalar()
                scalar()
            else:
                scalar();scalar()
                for _ in range(int(exact())): component()
                for _ in range(int(exact())):
                    for _ in range(int(exact())): exact()
                    component()
            self.assertEqual(cursor,len(received),name)
        self.assertEqual(operations,set(PAYLOADS))

class ProviderTests(unittest.TestCase):
    def corrupt(self, call, change, code=None, stderr=None):
        native=NativeSourceReplay(PROBE); original=native._run
        def altered(data):
            rc,out,err=original(data)
            return rc if code is None else code,change(out),err if stderr is None else stderr
        with mock.patch.object(native,'_run',side_effect=altered),self.assertRaises(NativeReplayError):
            call(native)
        self.assertEqual(native.exchanges,[])

    @staticmethod
    def field(index,value):
        def mutate(data):
            words=data.split();words[index]=value;return b' '.join(words)
        return mutate

    def test_all_response_headers_and_byte_guards(self):
        for call in (groups,shared,cubic):
            for i,value in ((0,b'wrong'),(1,b'2'),(2,b'wrong'),(3,b'0'*64),(4,b'0'*64),(5,b'invalid_input')):
                self.corrupt(call,self.field(i,value))
            for change in (lambda b:b+b' extra',lambda b:b.rsplit(None,1)[0],lambda b:b'',
                           lambda b:b+b'\xff',lambda b:b+b'\x00',lambda b:b' '*(MAX_BYTES+1)):
                self.corrupt(call,change)
            self.corrupt(call,lambda b:b,code=1)
            self.corrupt(call,lambda b:b,stderr=b'warning')

    def test_response_numeric_and_structural_fields(self):
        for call,index in ((shared,6),(cubic,6)):
            for value in (b'-1',b'nan',b'inf',b'1e999',b'1e-999',b'true',b'1'*129):
                self.corrupt(call,self.field(index,value))
        # group response: count; id/clock/group identity, partition completeness.
        for i,value in ((6,b'1'),(7,b'Wrong'),(8,b'Clock'),(9,b'0'),(12,b'1')):
            self.corrupt(groups,self.field(i,value))
        # shared: missing/duplicate output, invalid kind, bandwidth, group/bin power.
        for i,value in ((7,b'0'),(7,b'3'),(8,b'9'),(9,b'1'),(10,b'2'),(11,b'0'),
                        (14,b'10'),(17,b'1'),(20,b'0'),(22,b'0'),(25,b'0')):
            self.corrupt(shared,self.field(i,value))
        # cubic: reduced inputs, term arity/order/indices/kind/group/support.
        for i,value in ((8,b'0'),(8,b'3'),(9,b'12'),(10,b'1'),(11,b'2'),(12,b'0'),
                        (21,b'0'),(21,b'17'),(22,b'0'),(23,b'0'),(23,b'-1'),
                        (24,b'11'),(25,b'1'),(26,b'2'),(27,b'2')):
            self.corrupt(cubic,self.field(i,value))

    def test_positive_drive_corruption_and_limiter_expansion_rejected(self):
        self.corrupt(shared,self.field(6,b'.01'))
        self.corrupt(cubic,self.field(6,b'.01'))
        self.corrupt(cubic,self.field(7,b'.01'))

    def test_missing_and_duplicate_full_term_identity(self):
        def rows(data):
            words=data.split(); offset=9+6*int(words[8]); count=int(words[offset]); start=offset+1
            terms=[]
            for _ in range(count):
                size=1+int(words[start])+6;terms.append(words[start:start+size]);start+=size
            return words,offset,terms
        def missing(data):
            words,offset,terms=rows(data);terms.pop();return b' '.join(words[:offset]+[str(len(terms)).encode()]+[v for t in terms for v in t])
        def duplicate(data):
            words,offset,terms=rows(data);terms[-1]=terms[0];return b' '.join(words[:offset+1]+[v for t in terms for v in t])
        def reversed_terms(data):
            words,offset,terms=rows(data)
            return b' '.join(words[:offset+1]+[v for t in reversed(terms) for v in t])
        def duplicate_generated_group(data):
            words,offset,terms=rows(data)
            generated=[t for t in terms if int(t[0])>1]
            generated[-1][int(generated[-1][0])+4]=generated[0][int(generated[0][0])+4]
            return b' '.join(words[:offset+1]+[v for t in terms for v in t])
        self.corrupt(cubic,missing);self.corrupt(cubic,duplicate)
        self.corrupt(cubic,reversed_terms);self.corrupt(cubic,duplicate_generated_group)
        self.corrupt(cubic,self.field(13,b'.02'))

    def test_invalid_python_inputs_never_launch(self):
        native=NativeSourceReplay(PROBE)
        calls=[lambda:native.assign_source_coherence([]),lambda:native.assign_source_coherence([S('A'),S('A')]),
               lambda:native.assign_source_coherence([S('A/B')]),lambda:native.assign_source_coherence([S('A','-')])]
        for c in (COMPONENTS[0]._replace(bin=True),COMPONENTS[0]._replace(kind=K.HARMONIC),
                  COMPONENTS[0]._replace(amplitude=True),COMPONENTS[0]._replace(amplitude=1e-200),COMPONENTS[0]._replace(amplitude=complex(math.nan)),
                  COMPONENTS[0]._replace(bandwidth_hz=0),COMPONENTS[0]._replace(coherence_group=0)):
            calls.append(lambda c=c:native.compress_coherent_fundamentals(1e8,[c],**COMMON))
        calls.append(lambda:native.coherent_amplifier(1e8,COMPONENTS,**COMMON,input_ip2_dbm=20.,input_ip3_dbm=10.,propagate_distortion=True))
        with mock.patch.object(native,'_run',side_effect=AssertionError('launched invalid request')):
            for call in calls:
                with self.assertRaises((ValueError,TypeError)):call()

    def test_actual_process_faults(self):
        for mode,status in (('timeout','timeout'),('stdout_limit','resource_limit'),('stderr_limit','resource_limit'),('nonzero','execution_failure')):
            with mock.patch.dict(os.environ,{'RFMODEL_REFERENCE_ORACLE_FAULT':mode}):
                native=NativeSourceReplay(ARGS.fault_oracle.resolve(),manifest_path=str(PROBE)+'.manifest.json',timeout=.05 if mode=='timeout' else 10.)
                with self.assertRaises(NativeReplayError) as caught:shared(native)
                self.assertEqual(caught.exception.status,status)
                self.assertEqual(native.exchanges,[])

    def test_frozen_execution_identity_before_and_after_call(self):
        with tempfile.TemporaryDirectory() as directory:
            probe=Path(directory)/PROBE.name
            shutil.copy2(PROBE,probe)
            manifest=Path(str(probe)+'.manifest.json')
            shutil.copy2(str(PROBE)+'.manifest.json',manifest)
            native=NativeSourceReplay(probe)
            original=native._run
            def modified(data):
                answer=original(data)
                manifest.write_bytes(manifest.read_bytes()+b' ')
                return answer
            with mock.patch.object(native,'_run',side_effect=modified),self.assertRaises(NativeReplayError):
                shared(native)
            self.assertEqual(native.exchanges,[])
            with self.assertRaises(NativeReplayError):shared(native)
            with self.assertRaises(NativeReplayError):NativeSourceReplay(probe)

    def test_error_envelopes_cannot_be_numerical_failures(self):
        for status,code in (('invalid_input',2),('resource_limit',3),('numeric_range',4),('internal_error',5)):
            native=NativeSourceReplay(PROBE)
            def envelope(data):
                rid=data.split()[3].decode();build=native.execution_identity['build_id']
                return code,f'{MAGIC} 1 error {rid} {build} {status}\n'.encode(),b''
            with mock.patch.object(native,'_run',side_effect=envelope),self.assertRaises(NativeReplayError) as caught:
                cubic(native)
            self.assertEqual(caught.exception.status,status)
            self.assertEqual(native.exchanges,[])
            def contradiction(data):
                _,out,err=envelope(data);return 0,out,err
            with mock.patch.object(native,'_run',side_effect=contradiction),self.assertRaises(NativeReplayError):cubic(native)

    def test_no_product_imports_and_old_domain_stays_closed(self):
        native=NativeSourceReplay(PROBE)
        for op in ('polynomial','highorder','am_pm','cubic_two_stage_v1'):
            with self.assertRaises(ValueError):native._invoke(op,[])
        assert_no_product_imports()

if __name__=='__main__':unittest.main()
