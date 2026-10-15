#!/usr/bin/env python3
"""Pool-free tests. All executed zdb commands fail before discovery."""
import json, os, pathlib, subprocess, tempfile, unittest
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
