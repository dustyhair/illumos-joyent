"""Pool-free native verifier and CLI tests; real checksum tests are separate."""
import json,os,pathlib,subprocess,tempfile,unittest
BASE=pathlib.Path('/build/workspaces/raidz')
BUILD=pathlib.Path(os.environ.get('ZDB_INVENTORY_BUILD',str(BASE/'inventory-build-v6')))
ENV=dict(os.environ,LD_LIBRARY_PATH_64=':'.join(map(str,[BUILD/'lib',BASE/'smartos-live/proto/usr/lib/amd64',BASE/'smartos-live/proto/lib/amd64'])))
class Tests(unittest.TestCase):
 @classmethod
 def setUpClass(cls):cls.tmp=pathlib.Path(tempfile.mkdtemp(prefix='raw-verify-tests-',dir=BUILD))
 def invoke(self,mode):
  p=self.tmp/(mode+'.jsonl');r=subprocess.run([str(BUILD/'test_verify'),mode,str(p)],env=ENV,capture_output=True,text=True)
  rows=[json.loads(s) for s in p.read_text().splitlines()]
  self.assertIn('writes=0',r.stdout);return r,rows
 def test_successes_original_bp_p_and_duplicates(self):
  for mode in ('normal','sha256','compressed','duplicate'):
   with self.subTest(mode=mode):
    r,rows=self.invoke(mode);self.assertEqual(r.returncode,0,r.stderr)
    vs=[x for x in rows if x['record']=='verification'];self.assertEqual(len(vs),2 if mode=='duplicate' else 1)
    for v in vs:self.assertEqual(v['status'],'verified');self.assertTrue(v['measured']);self.assertEqual(v['completed_p'],'1024');self.assertEqual(v['requested_p'],'1024')
    end=next(x for x in rows if x['record']=='verification_end');self.assertTrue(end['verification_complete']);self.assertEqual(end['submitted'],end['completed']);self.assertEqual(end['outstanding'],'0')
    self.assertTrue(rows[-1]['export_complete']);self.assertTrue(all(x['coverage_complete'] is x['payload_validation_performed'] is x['evacuation_authorized'] is False for x in rows))
 def test_nonapplicable_no_reads(self):
  for mode in ('hole','embedded'):
   r,rows=self.invoke(mode);self.assertEqual(r.returncode,0);self.assertIn('reads=0',r.stdout)
   v=next(x for x in rows if x['record']=='verification');self.assertEqual(v['status'],'not_applicable');self.assertFalse(v['measured'])
 def test_unsupported_never_submitted(self):
  for mode in ('checksum-off','noparity','invalid-type','protected','gang','dedup','ddt','bad-geometry'):
   with self.subTest(mode=mode):
    r,rows=self.invoke(mode);self.assertNotEqual(r.returncode,0);self.assertIn('reads=0',r.stdout)
    v=next(x for x in rows if x['record']=='verification');self.assertEqual(v['status'],'unsupported');self.assertFalse(rows[-1]['export_complete'])
 def test_readonly_gate_before_submission(self):
  for mode in ('writable','no-read-mode'):
   r,rows=self.invoke(mode);self.assertNotEqual(r.returncode,0);self.assertIn('reads=0',r.stdout);self.assertFalse(rows[-1]['export_complete'])
 def test_completed_errors_and_corrupt_completions(self):
  for mode in ('checksum-error','read-error','short-completion','changed-bp','changed-bookmark','writable-during-io','wrong-type','missing-completion','double-completion','wait-disagrees'):
   with self.subTest(mode=mode):
    r,rows=self.invoke(mode);self.assertNotEqual(r.returncode,0);self.assertFalse(rows[-1]['export_complete'])
    self.assertFalse(next(x for x in rows if x['record']=='verification_end')['verification_complete'])
    self.assertEqual(next(x for x in rows if x['record']=='verification')['status'],'failed')
 def test_byte_limit(self):
  r,rows=self.invoke('byte-limit');self.assertNotEqual(r.returncode,0);self.assertIn('reads=0',r.stdout)
 def test_final_output_failures(self):
  for mode in ('close-error','flush-error'):
   r,rows=self.invoke(mode);self.assertNotEqual(r.returncode,0)
 def test_cli_no_discovery(self):
  for args in ([],['-b'],['-K','/unused'],['-J','/unused']):
   p=self.tmp/'no-create';r=subprocess.run([str(BUILD/'zdb-inventory'),'-Y',str(p)]+args,env=ENV,capture_output=True,text=True)
   self.assertNotEqual(r.returncode,0);self.assertIn('requires only',r.stderr);self.assertFalse(p.exists())
 def test_cli_existing_output(self):
  p=self.tmp/'existing';p.write_text('preserve');r=subprocess.run([str(BUILD/'zdb-inventory'),'-Y',str(p),'-e','-p','/fixture-only','fixture'],env=ENV,capture_output=True,text=True)
  self.assertNotEqual(r.returncode,0);self.assertIn('inventory output',r.stderr);self.assertEqual(p.read_text(),'preserve')
if __name__=='__main__':unittest.main(verbosity=2)
