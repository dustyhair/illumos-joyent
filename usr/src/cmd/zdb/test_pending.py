"""Pool-free native checked walker tests; no real pool initialization."""
import json,os,pathlib,subprocess,tempfile,unittest
BASE=pathlib.Path('/build/workspaces/raidz')
BUILD=pathlib.Path(os.environ.get('ZDB_INVENTORY_BUILD',str(BASE/'inventory-build-v9')))
ENV=dict(os.environ,LD_LIBRARY_PATH_64=':'.join(map(str,[BUILD/'lib',BASE/'smartos-live/proto/usr/lib/amd64',BASE/'smartos-live/proto/lib/amd64'])))
class Tests(unittest.TestCase):
 @classmethod
 def setUpClass(cls):cls.tmp=pathlib.Path(tempfile.mkdtemp(prefix='pending-tests-',dir=BUILD))
 def invoke(self,mode):
  p=self.tmp/(mode+'.jsonl');r=subprocess.run([str(BUILD/'test_pending'),mode,str(p)],env=ENV,capture_output=True,text=True)
  rows=[json.loads(s) for s in p.read_text().splitlines()]
  self.assertIn('writes=0 payload_reads=0',r.stdout,r.stderr);return r,rows
 def test_success_provenance(self):
  for mode,count in [('normal',1),('both',2),('empty',0),('nested',2),('duplicate-extents',2),('embedded',1),('legacy',1),('legacy-v0',1),('multi-bp',8),('multi-list',0)]:
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
    self.assertIn('deferred-free payload validation',rows[0]['excluded'])
    self.assertNotIn('deferred-free ownership and payload validation',rows[0]['excluded'])
    if mode=='multi-bp':self.assertIn('reads=2 ',r.stdout)
    if mode=='multi-list':self.assertEqual(end['links'],'65');self.assertEqual(end['objects'],'67');self.assertIn('reads=2 ',r.stdout)
    self.assertTrue(rows[-1]['export_complete']);self.assertTrue(all(x['coverage_complete'] is x['payload_validation_performed'] is x['evacuation_authorized'] is False for x in rows))
 def test_failures_terminal_and_release(self):
  for mode in ('cycle','cross-root-alias','list-alias','duplicate-child','zero-child','missing-child','bad-list-type','depth-limit','missing-root','missing-dsl-root','bad-type','bad-bonus-type','bad-bonus-size','short-bonus','bad-block-size','huge-block','truncated','entries-limit','subobjects-limit','empty-accounting','read-error','bonus-error','info-error','sparse','writable','invalid-type','protected','dedup','ddt','gang','hole','future-birth','bad-geometry','outside-vdev'):
   with self.subTest(mode=mode):
    r,rows=self.invoke(mode);self.assertEqual(r.returncode,1,r.stderr);self.assertFalse(rows[-1]['export_complete'])
    end=next(x for x in rows if x['record']=='pending_end');self.assertFalse(end['pending_complete']);self.assertNotEqual(end['error'],0)
 def test_second_block_failures(self):
  for mode,entries,links,active in [('second-bp-error',4,0,'1'),('second-list-error',0,64,'3')]:
   with self.subTest(mode=mode):
    r,rows=self.invoke(mode);self.assertEqual(r.returncode,1);self.assertIn('reads=2 ',r.stdout)
    end=next(x for x in rows if x['record']=='pending_end')
    self.assertFalse(end['pending_complete']);self.assertEqual(int(end['entries']),entries);self.assertEqual(int(end['links']),links)
    self.assertEqual(end['active_object'],active);self.assertFalse(rows[-1]['export_complete'])
 def test_cumulative_production_boundaries(self):
  # Exercise real 100000-entry and 4096-ID constants across both roots.
  # Stream large outputs to keep the test process memory bounded.
  for mode,expected_exit,expected_entries,expected_objects,expected_links in [
   ('cumulative-entries-boundary',0,100000,2,0),
   ('cumulative-entries-excess',1,50000,1,0),
   ('cumulative-ids-boundary',0,0,4094,4092),
   ('cumulative-ids-excess',1,0,2,4092)]:
   with self.subTest(mode=mode):
    path=self.tmp/(mode+'.jsonl');r=subprocess.run([str(BUILD/'test_pending'),mode,str(path)],env=ENV,capture_output=True,text=True)
    self.assertEqual(r.returncode,expected_exit,r.stderr);self.assertIn('writes=0 payload_reads=0',r.stdout)
    count=0;last_index={};end=None;terminal=None
    with path.open() as f:
     for line in f:
      row=json.loads(line)
      if row['record']=='pending_owner':
       key=(row['queue'],row['object']);self.assertEqual(int(row['entry_index']),last_index.get(key,-1)+1);last_index[key]=int(row['entry_index']);count+=1
      elif row['record']=='pending_end':end=row
      elif row['record']=='end':terminal=row
    self.assertEqual(count,expected_entries);self.assertEqual(int(end['entries']),expected_entries)
    self.assertEqual(int(end['objects']),expected_objects);self.assertEqual(int(end['links']),expected_links)
    self.assertEqual(end['pending_complete'],expected_exit==0);self.assertEqual(terminal['export_complete'],expected_exit==0)
 def test_output_failures(self):
  for mode in ('flush-error','close-error'):
   r,rows=self.invoke(mode);self.assertEqual(r.returncode,1)
if __name__=='__main__':unittest.main(verbosity=2)
