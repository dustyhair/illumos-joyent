#!/usr/bin/env python3
"""Pool-free tests. All executed zdb commands fail before discovery."""
import json, os, pathlib, subprocess, tempfile, unittest, shutil
BASE = pathlib.Path('/build/workspaces/raidz')
BUILD = BASE / 'inventory-build'
SRC = BASE / 'inventory-prototype/usr/src'
ENV = dict(os.environ, LD_LIBRARY_PATH_64=':'.join(map(str, [BUILD/'lib', BASE/'smartos-live/proto/usr/lib/amd64', BASE/'smartos-live/proto/lib/amd64'])))

class InventoryTests(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.tmp = pathlib.Path(tempfile.mkdtemp(prefix='verified-tests-', dir=BUILD))
  cls.normal = cls.tmp/'normal.jsonl'
  q = subprocess.run([str(BUILD/'test_inventory'), 'normal', str(cls.normal)],env=ENV,capture_output=True,text=True)
  assert q.returncode == 0, (q.returncode,q.stderr)
  cls.rows = [json.loads(x) for x in cls.normal.read_text().splitlines()]
  cls.bps = [x for x in cls.rows if x['record']=='bp']
 def fixture(self, mode):
  p=self.tmp/(mode+'.jsonl')
  q=subprocess.run([str(BUILD/'test_inventory'),mode,str(p)],env=ENV,capture_output=True,text=True)
  self.assertEqual(q.returncode,0,q.stderr)
  return [json.loads(x) for x in p.read_text().splitlines()]
 def test_invalid_type_matrix(self):
  rows=[x for x in self.fixture('invalid-types') if x['record']=='bp']
  self.assertEqual(len(rows),24)
  for r in rows:
   self.assertEqual(r['classification'],'type_unsupported')
   self.assertIsNone(r['metadata']);self.assertEqual(r['dvas'],[])
   self.assertTrue(r['unsupported']);self.assertEqual(len(r['bp_words']),16)
 def test_protected_layout_matrix(self):
  rows=[x for x in self.fixture('protected-layouts') if x['record']=='bp']
  self.assertEqual(len(rows),24)
  for r in rows:
   self.assertEqual(r['classification'],'protected_unsupported')
   self.assertEqual(r['dvas'],[]);self.assertTrue(r['children_skipped'])
 def test_mixed_dvas_order_independent(self):
  rows=[x for x in self.fixture('mixed-slots') if x['record']=='bp']
  self.assertEqual([[d['geometry_valid'] for d in r['dvas']] for r in rows],[[False,True],[True,False]])
  for r in rows:
   self.assertTrue(r['unsupported'])
   for d in r['dvas']:self.assertIn('qsize',d)
 def test_allocation_class_policy_and_provenance(self):
  rows=self.fixture('allocation-classes');bps=[x for x in rows if x['record']=='bp'];vs=[x for x in rows if x['record']=='vdev']
  self.assertEqual(len(bps),10);self.assertEqual(len(vs),10)
  for i,(bp,vd) in enumerate(zip(bps,vs)):
   d=bp['dvas'][0];self.assertEqual(d['allocation_class'],vd['allocation_class'])
   self.assertEqual(d['alloc_bias'],vd['alloc_bias']);self.assertEqual(d['is_log'],vd['is_log'])
   self.assertEqual(d['geometry_valid'],i==0);self.assertEqual(bp['unsupported'],i!=0)
 def test_discovery_exported_active_import_stubs(self):
  self.assertTrue(self.fixture('import-policy')[-1]['export_complete'])
 def test_final_flush_failure_returned(self):
  p=self.tmp/'final-flush.jsonl'
  q=subprocess.run([str(BUILD/'test_inventory'),'flush-failure',str(p)],env=ENV)
  self.assertNotEqual(q.returncode,0)
 def test_final_close_failure_returned(self):
  p=self.tmp/'final-close.jsonl'
  q=subprocess.run([str(BUILD/'test_inventory'),'close-failure',str(p)],env=ENV)
  self.assertNotEqual(q.returncode,0)
  end=json.loads(p.read_text().splitlines()[-1])
  self.assertTrue(end['export_complete']) # Must reject because exit is nonzero.
 def launch(self, root, *args, env=None):
  return subprocess.run(['/opt/local/bin/python3.12',str(root/'launch.py'),*args],
                        env=env,capture_output=True,text=True)
 def test_native_launcher_closure(self):
  root=BUILD/'runtime-review-v2'
  q=subprocess.run(['/bin/bash',str(root/'launch.sh'),'--verify-only'],capture_output=True,text=True)
  self.assertEqual(q.returncode,0,q.stderr)
 def test_native_launcher_rejects_tampering(self):
  root=self.tmp/'native-tamper';shutil.copytree(BUILD/'runtime-review-v2',root)
  p=root/'lib/libzpool.so.1';p.write_bytes(p.read_bytes()+b'changed')
  q=subprocess.run(['/bin/bash',str(root/'launch.sh'),'--verify-only'],capture_output=True,text=True)
  self.assertNotEqual(q.returncode,0);self.assertIn('identity mismatch',q.stderr)
 def test_native_launcher_invalid_options_only(self):
  root=BUILD/'runtime-review-v2';p=self.tmp/'native-no-output'
  q=subprocess.run(['/bin/bash',str(root/'launch.sh'),'-J',str(p),'-b'],capture_output=True,text=True)
  self.assertNotEqual(q.returncode,0);self.assertIn('-J requires only',q.stderr);self.assertFalse(p.exists())
 def test_verified_runtime_closure(self):
  q=self.launch(BUILD/'runtime-review-v2','--verify-only')
  self.assertEqual(q.returncode,0,q.stderr)
 def test_verified_runtime_exec_invalid_options_only(self):
  p=self.tmp/'not-created'
  q=self.launch(BUILD/'runtime-review-v2','-J',str(p),'-b',
                env=dict(os.environ,SPA_CONFIG_PATH='/never-use-this',LD_LIBRARY_PATH_64='/nonexistent'))
  self.assertNotEqual(q.returncode,0)
  self.assertIn('-J requires only',q.stderr);self.assertFalse(p.exists())
 def test_marker_compatible_wrong_library_refused(self):
  root=self.tmp/'wrong-marker-runtime'
  shutil.copytree(BUILD/'runtime-review-v2',root)
  old=BUILD/'review-9b2e3f1de8/lib/libzpool.so.1'
  alternate=root/'lib/libzpool.so.1'
  shutil.copy2(old,alternate)
  # Valid ELF with unchanged exports/capability but a different file identity.
  with alternate.open('ab') as f:f.write(b'\nmarker-compatible alternate build fixture\n')
  check=subprocess.run(['/opt/local/bin/python3.12','-c',
       'import ctypes,sys; print(ctypes.CDLL(sys.argv[1]).traverse_no_zil_supported())',str(alternate)],
       env=ENV,capture_output=True,text=True)
  self.assertEqual(check.returncode,0,check.stderr);self.assertEqual(check.stdout.strip(),'1')
  q=self.launch(root,'--verify-only')
  self.assertNotEqual(q.returncode,0);self.assertIn('identity mismatch: lib/libzpool',q.stderr)
 def test_changed_dependency_and_loader_refused(self):
  root=self.tmp/'tampered-runtime';shutil.copytree(BUILD/'runtime-review-v2',root)
  for name in ['lib/libnvpair.so.1','ld.so.1','zdb-inventory']:
   p=root/name;original=p.read_bytes();p.write_bytes(original+b'changed')
   q=self.launch(root,'--verify-only')
   self.assertNotEqual(q.returncode,0);self.assertIn('identity mismatch: '+name,q.stderr)
   p.write_bytes(original)
 def test_manifest_replacement_refused(self):
  root=self.tmp/'manifest-runtime';shutil.copytree(BUILD/'runtime-review-v2',root)
  p=root/'runtime.json';p.write_text(p.read_text()+' ')
  q=self.launch(root,'--verify-only')
  self.assertNotEqual(q.returncode,0);self.assertIn('manifest identity mismatch',q.stderr)
 def test_geometry_256_pinned_oracle_cases(self):
  for r in self.bps[:256]:
   with self.subTest(p=r['psize']):
    p=int(r['psize']);d=r['dvas'][0]
    self.assertTrue(d['geometry_valid']);self.assertEqual(int(d['qsize']),((p+4095)//4096)*4096)
    self.assertEqual(d['asize'],d['expected_asize'])
 def test_exact_max_uint64(self):
  r=self.bps[0];self.assertEqual(r['objset'],str(2**64-1));self.assertEqual(r['physical_birth'],str(2**64-2))
 def test_raw_bp_decode(self):
  for r in self.bps[:256]:
   w=[int(x,16) for x in r['bp_words']];self.assertEqual(len(w),16)
   self.assertTrue(all(len(x)==16 for x in r['bp_words']))
   self.assertEqual(((w[6]>>16)&65535)*512+512,int(r['psize']))
   self.assertEqual((w[0]&0xffffff)*512,int(r['dvas'][0]['asize']))
   self.assertEqual((w[1]&((1<<63)-1))*512,int(r['dvas'][0]['offset']))
 def test_json_escaping(self):
  self.assertEqual(self.rows[1]['text'],'quote" slash\\ newline\n\x01\xff')
 def test_metadata_retained(self):
  self.assertTrue(any(r['metadata'] and r['classification']=='ordinary' for r in self.bps))
 def test_reference_multiplicity(self):
  matches=[r for r in self.bps if r['bp_words']==self.bps[0]['bp_words']]
  self.assertGreaterEqual(len(matches),2);self.assertGreater(len({r['object'] for r in matches}),1)
 def test_gang_has_no_interval(self):
  r=next(r for r in self.bps if r['classification']=='gang_unsupported')
  self.assertTrue(r['unsupported']);self.assertTrue(r['children_skipped'])
  self.assertNotIn('interval',r['dvas'][0]);self.assertNotIn('qsize',r['dvas'][0])
 def test_protected_has_no_dva_decode(self):
  r=next(r for r in self.bps if r['classification']=='protected_unsupported')
  self.assertEqual(r['dvas'],[]);self.assertTrue(r['unsupported'])
 def test_dedup_rejected(self):
  self.assertTrue(next(r for r in self.bps if r['classification']=='dedup_unsupported')['unsupported'])
 def test_embedded_and_hole(self):
  for kind in ['embedded','hole']:
   self.assertEqual(next(r for r in self.bps if r['classification']==kind)['dvas'],[])
  self.assertEqual(next(r for r in self.bps if r['classification']=='embedded')['embedded_payload_bytes'],'16')
 def test_bad_geometry_rejected(self):
  bad=[r for r in self.bps if r['classification']=='ordinary' and r['unsupported']]
  self.assertEqual(len(bad),4)
 def test_no_certificate_flags(self):
  for r in self.rows:
   if r['record']=='escape-test':continue
   for k in ['coverage_complete','payload_validation_performed','evacuation_authorized']:self.assertIs(r[k],False)
 def test_terminal_and_count(self):
  r=self.rows[-1];self.assertEqual(r['record'],'end');self.assertTrue(r['export_complete'])
  self.assertEqual(int(r['observed']),len(self.bps)+1)
  self.assertEqual(len([r for r in self.rows if r['record']=='dnode']),1)
 def test_error_terminal(self):
  p=self.tmp/'terminal-error.jsonl'
  q=subprocess.run([str(BUILD/'test_inventory'),'terminal-error',str(p)],env=ENV)
  self.assertEqual(q.returncode,0);r=json.loads(p.read_text().splitlines()[-1])
  self.assertFalse(r['export_complete']);self.assertNotEqual(r['error'],0)
 def test_injected_traversal_error(self):
  p=self.tmp/'traversal-error.jsonl'
  q=subprocess.run([str(BUILD/'test_inventory'),'traversal-error',str(p)],env=ENV)
  self.assertEqual(q.returncode,0)
  rows=[json.loads(x) for x in p.read_text().splitlines()]
  self.assertFalse(rows[-1]['export_complete']);self.assertNotEqual(rows[-1]['error'],0)
  self.assertTrue(any(x['record']=='pool' for x in rows))
 def test_old_library_refused_before_output(self):
  p=self.tmp/'wrong-library'
  env=dict(ENV,LD_LIBRARY_PATH_64=str(BASE/'smartos-live/proto/usr/lib/amd64')+':'+str(BASE/'smartos-live/proto/lib/amd64'))
  q=subprocess.run([str(BUILD/'zdb-inventory'),'-J',str(p),'-e','-p',str(self.tmp),'no-pool-opened'],env=env,capture_output=True,text=True)
  self.assertNotEqual(q.returncode,0);self.assertFalse(p.exists())
  self.assertIn('traverse_no_zil_supported',q.stderr)
 def test_record_limit_and_output_failure(self):
  p=self.tmp/'errors.jsonl'
  self.assertEqual(subprocess.run([str(BUILD/'test_inventory'),'errors',str(p)],env=ENV).returncode,0)
  self.assertNotEqual(json.loads(p.read_text().splitlines()[-1])['record'],'end')
 def test_exclusive_output_and_symlink(self):
  p=self.tmp/'existing';p.write_text('preserve');link=self.tmp/'link';link.symlink_to(p)
  for target in [p,link]:
   q=subprocess.run([str(BUILD/'zdb-inventory'),'-J',str(target),'-e','-p',str(self.tmp),'no-pool-opened'],env=ENV,capture_output=True)
   self.assertNotEqual(q.returncode,0)
  self.assertEqual(p.read_text(),'preserve')
 def test_incompatible_options_before_open(self):
  for option in ['-b','-k','-F','-X','-A','-R','-c','-U','-o','-t','-v']:
   with self.subTest(option=option):
    p=self.tmp/('reject-'+option[1:]);extra=[option]
    if option in ['-U','-o','-t']:extra+=['invalid']
    q=subprocess.run([str(BUILD/'zdb-inventory'),'-J',str(p),'-e','-p',str(self.tmp),*extra,'no-pool-opened'],env=ENV,capture_output=True)
    self.assertNotEqual(q.returncode,0);self.assertFalse(p.exists())
 def test_missing_required_options(self):
  for args in [[],['-e'],['-e','-p','relative'],['-e','-p',str(self.tmp),'pool/dataset']]:
   p=self.tmp/'missing'
   q=subprocess.run([str(BUILD/'zdb-inventory'),'-J',str(p),*args,'no-pool-opened'],env=ENV,capture_output=True)
   self.assertNotEqual(q.returncode,0);self.assertFalse(p.exists())
 def test_legacy_traversal_unchanged_except_opt_in_gate(self):
  old=(BASE/'smartos-live/projects/illumos/usr/src/uts/common/fs/zfs/dmu_traverse.c').read_text()
  new=(SRC/'uts/common/fs/zfs/dmu_traverse.c').read_text()
  new=new.split('\n/* Userland diagnostics use this symbol')[0]
  new=new.replace('if (!(flags & TRAVERSE_NO_ZIL) && ds != NULL &&\n\t    !ds->ds_is_snapshot && !BP_IS_HOLE(rootbp)) {','if (ds != NULL && !ds->ds_is_snapshot && !BP_IS_HOLE(rootbp)) {')
  self.assertEqual(new,old)
 def test_no_hard_and_explicit_zil_exclusion(self):
  s=(SRC/'cmd/zdb/zdb_inventory.c').read_text()
  self.assertNotIn('TRAVERSE_HARD',s);self.assertIn('TRAVERSE_NO_ZIL, zi_record',s)
  self.assertIn('ZIL',self.rows[0]['excluded'])

if __name__=='__main__':unittest.main(verbosity=2)
