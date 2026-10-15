"""Pool-free native checked walker tests; no real pool initialization."""
import json,os,pathlib,subprocess,tempfile,unittest
BASE=pathlib.Path('/build/workspaces/raidz')
BUILD=pathlib.Path(os.environ.get('ZDB_INVENTORY_BUILD',str(BASE/'inventory-build-v8')))
ENV=dict(os.environ,LD_LIBRARY_PATH_64=':'.join(map(str,[BUILD/'lib',BASE/'smartos-live/proto/usr/lib/amd64',BASE/'smartos-live/proto/lib/amd64'])))
class Tests(unittest.TestCase):
 @classmethod
 def setUpClass(cls):cls.tmp=pathlib.Path(tempfile.mkdtemp(prefix='pending-tests-',dir=BUILD))
 def invoke(self,mode):
  p=self.tmp/(mode+'.jsonl');r=subprocess.run([str(BUILD/'test_pending'),mode,str(p)],env=ENV,capture_output=True,text=True)
  rows=[json.loads(s) for s in p.read_text().splitlines()]
  self.assertIn('writes=0 payload_reads=0',r.stdout,r.stderr);return r,rows
 def test_success_provenance(self):
  for mode,count in [('normal',1),('both',2),('empty',0),('nested',2),('duplicate-extents',2),('embedded',1),('legacy',1),('legacy-v0',1)]:
   with self.subTest(mode=mode):
    r,rows=self.invoke(mode);self.assertEqual(r.returncode,0,r.stderr)
    owners=[x for x in rows if x['record']=='pending_owner'];self.assertEqual(len(owners),count)
    self.assertTrue(all(x['payload_verifiable'] is x['movable_candidate'] is False for x in owners))
    end=next(x for x in rows if x['record']=='pending_end');self.assertTrue(end['pending_complete']);self.assertEqual(int(end['entries']),count)
    self.assertEqual([x['queue'] for x in rows if x['record']=='pending_queue_end'],['spa_deferred','dsl_free'])
    if mode=='nested':
     child=next(x for x in rows if x['record']=='pending_object' and x['object']=='4');self.assertEqual((child['root_object'],child['parent_object'],child['parent_index'],child['depth']),('1','1','0','1'))
    # Feed actual C-produced owner records into the v2 analyzer, alongside
    # explicitly synthetic paired allocation fixtures (never pool evidence).
    from test_pending_intervals import combine,run
    from test_paired_intervals import fixture
    pending=[x for x in rows if x['record'].startswith('pending_')]
    result=run(combine(fixture(),pending))
    self.assertEqual(len(result['pending_owner_records']),count)
    if mode=='nested':
     import copy
     for key,value in [('child_object','1'),('subobject_list','99'),('entry_index','1')]:
      changed=copy.deepcopy(pending);next(x for x in changed if x['record']=='pending_link')[key]=value
      with self.assertRaises(ValueError):run(combine(fixture(),changed))
    self.assertTrue(rows[-1]['export_complete']);self.assertTrue(all(x['coverage_complete'] is x['payload_validation_performed'] is x['evacuation_authorized'] is False for x in rows))
 def test_failures_terminal_and_release(self):
  for mode in ('cycle','cross-root-alias','list-alias','duplicate-child','zero-child','missing-child','bad-list-type','depth-limit','missing-root','missing-dsl-root','bad-type','bad-bonus-type','bad-bonus-size','short-bonus','bad-block-size','huge-block','truncated','entries-limit','subobjects-limit','empty-accounting','read-error','bonus-error','info-error','sparse','writable','invalid-type','protected','dedup','ddt','gang','hole','future-birth','bad-geometry','outside-vdev'):
   with self.subTest(mode=mode):
    r,rows=self.invoke(mode);self.assertEqual(r.returncode,1,r.stderr);self.assertFalse(rows[-1]['export_complete'])
    end=next(x for x in rows if x['record']=='pending_end');self.assertFalse(end['pending_complete']);self.assertNotEqual(end['error'],0)
 def test_output_failures(self):
  for mode in ('flush-error','close-error'):
   r,rows=self.invoke(mode);self.assertEqual(r.returncode,1)
if __name__=='__main__':unittest.main(verbosity=2)
