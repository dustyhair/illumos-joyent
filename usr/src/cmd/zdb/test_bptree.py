"""Pool-free production destroyed-tree walker fixtures and parser rejection tests."""
import copy,json,os,pathlib,subprocess,tempfile,unittest
from test_pending_intervals import combine,run,pending_fixture
from test_paired_intervals import fixture
from test_verified_intervals import sample
from verified_intervals import analyze as verified
BASE=pathlib.Path('/build/workspaces/raidz');BUILD=pathlib.Path(os.environ.get('ZDB_INVENTORY_BUILD',str(BASE/'inventory-build-v10')))
ENV=dict(os.environ,LD_LIBRARY_PATH_64=':'.join(map(str,[BUILD/'lib',BASE/'smartos-live/proto/usr/lib/amd64',BASE/'smartos-live/proto/lib/amd64'])))
def combined(own,verified_mode=False):
 rows=combine(sample() if verified_mode else fixture(),pending_fixture(empty=True));rows[0]['schema']=rows[0]['schema'].replace('-v2','-v3')
 next(r for r in rows if r['record']=='allocation_scope')['owner_obligations'][2]='async_destroy:pending_enumeration'
 i=next(i for i,r in enumerate(rows) if r['record']=='cut_end');rows[i:i]=copy.deepcopy(own);return rows
class Tests(unittest.TestCase):
 @classmethod
 def setUpClass(cls):cls.tmp=pathlib.Path(tempfile.mkdtemp(prefix='bptree-tests-',dir=BUILD))
 def invoke(self,mode):
  path=self.tmp/(mode+'.jsonl');r=subprocess.run([str(BUILD/'test_bptree'),mode,str(path)],env=ENV,capture_output=True,text=True)
  self.assertIn('no_mutation=1',r.stdout,r.stderr);return r,[json.loads(s) for s in path.read_text().splitlines()]
 def test_success(self):
  for mode,count in [('normal',4),('resumed',4),('resume-later',4),('resume-parent',3),('cutoff-equal',3),('root-cutoff-equal',0),('sentinel',0),('empty',0),('queue-exact',0),('inactive',0)]:
   with self.subTest(mode=mode):
    r,rows=self.invoke(mode);self.assertEqual(r.returncode,0,r.stderr)
    result=run(combined(rows));self.assertEqual(len(result['async_owner_records']),count)
    self.assertTrue(result['async_destroy_enumeration_complete']);self.assertFalse(result['coverage_complete']);self.assertFalse(result['evacuation_authorized'])
    self.assertTrue(all(x['payload_verifiable'] is x['movable_candidate'] is False for x in result['async_owner_records']))
    if mode=='normal':self.assertIn('queue_reads=2 metadata_reads=3',r.stdout)
    if mode=='resume-later':self.assertNotIn('1',[o['bookmark'][1] for o in result['async_owner_records']])
    if mode=='resume-parent':self.assertIn('metadata_reads=2',r.stdout)
    if mode=='sentinel':self.assertIn('metadata_reads=0',r.stdout)
 def test_failures(self):
  for mode in ['unresolved','reversed','overflow','queue-limit','read-first','read-second','arc-error','arc-size','sparse','tiny-bonus','wrong-type','extent','future-cutoff','future-birth','bad-resume','large-dnode','bad-level','bad-indirect','cycle','writable','inactive-object','ancestor-overlap','compression','zero-birth','compression-inherit','checksum-inherit']:
   with self.subTest(mode=mode):
    r,rows=self.invoke(mode);self.assertEqual(r.returncode,1,r.stderr);self.assertFalse(rows[-1]['async_complete'])
    with self.assertRaises((ValueError,KeyError)):run(combined(rows))
 def test_pre_io_budgets(self):
  for mode in ('reads','bytes','visits','depth','cycle','visits-exact'):
   r,_=self.invoke('budget-'+mode);self.assertEqual(r.returncode,1);self.assertIn('metadata_reads=0',r.stdout)
  for mode in ('exact','read-exact','depth-exact'):
   r,_=self.invoke('budget-'+mode);self.assertEqual(r.returncode,1);self.assertIn('metadata_reads=1',r.stdout)
 def test_verified_mode(self):
  _,rows=self.invoke('normal');r=run(combined(rows,True),verified)
  self.assertEqual(r['measured_read_summary']['verified_bp_references'],1);self.assertEqual(len(r['async_owner_records']),4)
 def test_corrupt_provenance(self):
  _,rows=self.invoke('normal')
  for kind,key,value in [('async_scope','active',False),('async_queue','begin','4'),('async_entry','entry_index','2'),('async_entry','completed_sentinel',True),('async_entry','birth_cutoff','31'),('async_owner','entry_index','0'),('async_owner','movable_candidate',True),('async_owner','depth','64'),('async_end','owners','0'),('async_end','async_complete',False),('async_end','read_bytes','1'),('async_end','visits','100001')]:
   own=copy.deepcopy(rows);next(r for r in own if r['record']==kind)[key]=value
   with self.subTest(kind=kind,key=key),self.assertRaises(ValueError):run(combined(own))
  for i in range(len(rows)):
   own=copy.deepcopy(rows);own.pop(i)
   with self.subTest(missing=i),self.assertRaises((ValueError,KeyError)):run(combined(own))
 def test_malformed_root_before_birth_pruning(self):
  for mode in ('normal','root-cutoff-equal'):
   _,original=self.invoke(mode)
   for bad in ('zero','type','level'):
    rows=copy.deepcopy(original);entry=next(r for r in rows if r['record']=='async_entry')
    w=entry['root_bp']
    if bad=='zero':w[:]=['0000000000000000']*16
    elif bad=='type':w[6]=f'{(int(w[6],16)&~(255<<48))|(19<<48):016x}'
    else:w[6]=f'{int(w[6],16)|(1<<56):016x}'
    # Corrupt the matching owner too, so root identity alone cannot catch it.
    for owner in rows:
     if owner['record']=='async_owner' and owner['bookmark'][2]=='-1':owner['bp_words']=w.copy()
    with self.subTest(mode=mode,bad=bad),self.assertRaises(ValueError):run(combined(rows))
 def test_missing_root_owner_even_with_adjusted_counts(self):
  for mode in ('normal','resumed','resume-later','resume-parent','cutoff-equal'):
   _,original=self.invoke(mode)
   for drop_all in (False,True):
    rows=copy.deepcopy(original)
    removed=[r for r in rows if r['record']=='async_owner' and (drop_all or r['bookmark'][2]=='-1')]
    rows=[r for r in rows if r not in removed]
    rows[-1]['owners']=str(int(rows[-1]['owners'])-len(removed))
    with self.subTest(mode=mode,drop_all=drop_all),self.assertRaises(ValueError):run(combined(rows))
 def test_root_disposition_and_postorder(self):
  _,rows=self.invoke('root-cutoff-equal');run(combined(rows))
  entry=next(r for r in rows if r['record']=='async_entry');entry['root_bp'][10]='0000000000000014'
  with self.assertRaises(ValueError):run(combined(rows))
  _,rows=self.invoke('normal');root=next(r for r in rows if r['record']=='async_owner' and r['bookmark'][2]=='-1')
  rows.remove(root);rows.insert(3,root)
  with self.assertRaises(ValueError):run(combined(rows))
  _,rows=self.invoke('sentinel');next(r for r in rows if r['record']=='async_entry')['root_bp']=['0000000000000000']*16
  run(combined(rows)) # Native completed sentinels deliberately skip root checks.
 def test_incremental_bptree_dependency(self):
  source=pathlib.Path(__file__).parent/'Makefile'
  rule=next(s for s in source.read_text().splitlines() if s.startswith('zdb_inventory.o:'))
  deps=rule.split(':',1)[1].split();self.assertIn('zdb_bptree.c',deps)
  work=self.tmp/'make-dependency';work.mkdir()
  (work/'Makefile').write_text(rule+'\n\t@echo rebuild-required\n')
  for name in deps:(work/name).write_text('fixture\n');os.utime(work/name,(100,100))
  (work/'zdb_inventory.o').write_text('fixture object\n');os.utime(work/'zdb_inventory.o',(200,200))
  r=subprocess.run(['/opt/local/bin/gmake','-q','zdb_inventory.o'],cwd=work);self.assertEqual(r.returncode,0)
  os.utime(work/'zdb_bptree.c',(300,300))
  r=subprocess.run(['/opt/local/bin/gmake','-q','zdb_inventory.o'],cwd=work);self.assertEqual(r.returncode,1)
if __name__=='__main__':unittest.main(verbosity=2)
