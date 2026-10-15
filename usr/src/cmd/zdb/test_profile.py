"""Pool-free native absence-evidence and supported-profile policy tests."""
import copy,hashlib,json,os,pathlib,subprocess,tempfile,unittest
from test_verified_intervals import sample
from test_pending_intervals import combine,pending_fixture
from test_paired_intervals import F
from paired_intervals import canonical
from profile_intervals import analyze
BASE=pathlib.Path('/build/workspaces/raidz');BUILD=pathlib.Path(os.environ.get('ZDB_INVENTORY_BUILD',str(BASE/'inventory-build-v12')))
ENV=dict(os.environ,LD_LIBRARY_PATH_64=':'.join(map(str,[BUILD/'lib',BASE/'smartos-live/proto/usr/lib/amd64',BASE/'smartos-live/proto/lib/amd64'])))
def combined(evidence,nonempty_pending=False,nonempty_async=False):
 base=sample();bp=base[3];bp['bp_type']=11;bp['bp_words'][6]='000b070200070007';base[4]['bp_words']=bp['bp_words'].copy()
 rows=combine(base,pending_fixture(empty=not nonempty_pending));rows[0]['schema']='zdb-observed-profile-v1'
 next(r for r in rows if r['record']=='allocation_scope')['owner_obligations'][2]='async_destroy:pending_enumeration'
 own=[dict(record='async_scope',object='0',active=False,**F),dict(record='async_end',object='0',entry_index='0',entries='0',owners='0',visits='0',reads='0',read_bytes='0',async_complete=True,error=0,**F)]
 if nonempty_async:
  own[0].update(object='9',active=True);own[-1].update(object='9',entry_index='1',entries='1',reads='4',read_bytes='208')
  own[1:1]=[dict(record='async_queue',object='9',begin='0',end='1',accounted_bytes='0',accounted_comp='0',accounted_uncomp='0',**F),dict(record='async_entry',object='9',entry_index='0',birth_cutoff=str(2**64-1),completed_sentinel=True,resume=['0']*4,root_bp=['0000000000000000']*16,**F)]
 i=next(i for i,r in enumerate(rows) if r['record']=='cut_end');rows[i:i]=own+copy.deepcopy(evidence);return rows

def run(rows,status='0'):
 data=b''.join(canonical(r)+b'\n' for r in rows);return analyze(data,status,hashlib.sha256(data).hexdigest())
class Tests(unittest.TestCase):
 @classmethod
 def setUpClass(cls):cls.tmp=pathlib.Path(tempfile.mkdtemp(prefix='profile-tests-',dir=BUILD))
 def invoke(self,mode):
  p=self.tmp/(mode+'.jsonl');r=subprocess.run([str(BUILD/'test_profile'),mode,str(p)],env=ENV,capture_output=True,text=True)
  self.assertIn('writes=0',r.stdout,r.stderr);return r,[json.loads(s) for s in p.read_text().splitlines()]
 def test_success_and_exact_scan_bounds(self):
  for mode in ('normal','extended-dataset','clone-head','birth-pruned-head','directory-exact','object-exact'):
   with self.subTest(mode=mode):
    r,rows=self.invoke(mode);self.assertEqual(r.returncode,0,r.stderr);result=run(combined(rows))
    self.assertTrue(result['supported_profile_passed']);self.assertTrue(result['ddt_absence_verified']);self.assertTrue(result['zil_empty_heads_verified'])
    self.assertFalse(result['ddt_enumerated']);self.assertFalse(result['zil_enumerated']);self.assertFalse(result['movable_candidate'])
    self.assertTrue(all(result[k] is False for k in F))
    self.assertEqual(result['profile_heads'],2 if mode=='clone-head' else 1)
    self.assertIn('metadata_reads=2' if mode=='clone-head' else 'metadata_reads=1',r.stdout)
 def test_native_rejections(self):
  for mode in ('ddt-stats','ddt-in-core','ddt-slot','ddt-cache-missing','ddt-cache-identity','ddt-directory','directory-error','directory-name','directory-limit','object-limit','orphan-ddt','unknown-type','wrong-dataset-type','ambiguous-dataset','next-error','next-repeat','info-error','bonus-error','bonus-short','inconsistent','unknown-flags','future-dataset','snapshot-state','snapshot-missing-next-zil','snapshot-self-next-zil','head-state','root-type','root-hole','zil-log','zil-claim','zil-padding','arc-error','arc-size','no-heads','writable'):
   with self.subTest(mode=mode):
    r,rows=self.invoke(mode);self.assertEqual(r.returncode,1,r.stderr);self.assertFalse(rows[-1]['profile_metadata_complete'])
    if mode in ('snapshot-missing-next-zil','snapshot-self-next-zil'):self.assertIn('metadata_reads=0 holds=1',r.stdout)
    with self.assertRaises(ValueError):run(combined(rows))
 def test_pre_io_and_exact_budgets(self):
  for mode in ('reads','bytes','datasets'):
   r,_=self.invoke('budget-'+mode);self.assertEqual(r.returncode,1);self.assertIn('metadata_reads=0 holds=0',r.stdout)
  for mode in ('exact','read-exact','dataset-exact'):
   r,_=self.invoke('budget-'+mode);self.assertEqual(r.returncode,0);self.assertIn('metadata_reads=1 holds=1',r.stdout)
 def test_empty_queue_policy(self):
  _,rows=self.invoke('normal')
  for pending,asynchronous in ((True,False),(False,True)):
   with self.subTest(pending=pending,asynchronous=asynchronous),self.assertRaisesRegex(ValueError,'empty pending/async'):run(combined(rows,pending,asynchronous))
 def test_evidence_corruption(self):
  _,original=self.invoke('normal')
  for kind,key,value in [('profile_scope','ddt_enumerated',True),('profile_directory','name','DDT-statistics'),('profile_object','object_type',42),('profile_object','object','0'),('profile_dataset','snapshot',True),('profile_dataset','zil_state','empty'),('profile_dataset','zil_header_words',['0000000000000001']*24),('profile_dataset','dataset_flags','1'),('profile_end','heads','2'),('profile_end','cached_ddt_slots','41'),('profile_end','ddt_absence_checked',False),('profile_end','reads','250001'),('profile_end','read_bytes',str((32<<20)+1)),('profile_end','read_bytes','0')]:
   rows=copy.deepcopy(original);next(r for r in rows if r['record']==kind)[key]=value
   with self.subTest(kind=kind,key=key),self.assertRaises(ValueError):run(combined(rows))
  for i in range(len(original)):
   rows=copy.deepcopy(original);rows.pop(i)
   with self.subTest(missing=i),self.assertRaises(ValueError):run(combined(rows))
 def test_verification_and_cut_binding(self):
  _,evidence=self.invoke('normal')
  for key in ('cut','verification','root','exit','residual','unknown'):
   rows=combined(evidence)
   if key=='cut':next(r for r in rows if r['record']=='cut_end')['txg']='31'
   if key=='verification':next(r for r in rows if r['record']=='verification')['status']='failed'
   if key=='root':next(r for r in rows if r['record']=='profile_dataset')['root_bp'][1]='0000000000000020'
   if key=='residual':
    bp=next(r for r in rows if r['record']=='bp');bp['bp_words'][1]='0000000000000010';bp['dvas'][0]['offset']='8192'
    next(r for r in rows if r['record']=='verification')['bp_words']=bp['bp_words'].copy()
   if key=='unknown':
    bp=next(r for r in rows if r['record']=='bp');bp['unsupported']=True;bp['children_skipped']=True
   with self.subTest(key=key),self.assertRaises(ValueError):run(rows,status='1' if key=='exit' else '0')
 def test_Q_prevalidation_and_schema(self):
  target=self.tmp/'not-created.jsonl'
  for args in (['-Q',str(target),'pool'],['-Q',str(target),'-Y',str(target),'-e','-p','/nonexistent','pool'],['-Q',str(target),'-e','-p','relative','pool'],['-Q',str(target),'-e','-p','/nonexistent','-b','pool']):
   r=subprocess.run([str(BUILD/'zdb-inventory'),*args],env=ENV,capture_output=True,text=True);self.assertEqual(r.returncode,2);self.assertFalse(target.exists())
  r,rows=self.invoke('mode-profile');self.assertEqual(r.returncode,0)
  self.assertEqual(rows[0]['schema'],'zdb-observed-profile-v1')
  self.assertTrue(all(all(row[k] is False for k in F) for row in rows))
 def test_incremental_profile_dependency(self):
  rule=next(s for s in pathlib.Path(__file__).with_name('Makefile').read_text().splitlines() if s.startswith('zdb_inventory.o:'))
  deps=rule.split(':',1)[1].split();self.assertIn('zdb_profile.c',deps)
  work=self.tmp/'make-profile';work.mkdir();(work/'Makefile').write_text(rule+'\n\t@echo rebuild-required\n')
  for name in deps:(work/name).write_text('fixture\n');os.utime(work/name,(100,100))
  (work/'zdb_inventory.o').write_text('fixture object\n');os.utime(work/'zdb_inventory.o',(200,200))
  self.assertEqual(subprocess.run(['/opt/local/bin/gmake','-q','zdb_inventory.o'],cwd=work).returncode,0)
  os.utime(work/'zdb_profile.c',(300,300))
  self.assertEqual(subprocess.run(['/opt/local/bin/gmake','-q','zdb_inventory.o'],cwd=work).returncode,1)
 def test_snapshot_link_evidence(self):
  _,original=self.invoke('normal')
  snapshot=next(r for r in original if r['record']=='profile_dataset' and r['snapshot'])
  self.assertEqual(snapshot['next_snap_object'],'2');self.assertEqual(snapshot['snapnames_object'],'0')
  for key,value,why in [('next_snap_object','0','snapshot next link'),('next_snap_object','3','snapshot next link'),('snapnames_object','7','snapshot name map')]:
   rows=copy.deepcopy(original);next(r for r in rows if r['record']=='profile_dataset' and r['snapshot'])[key]=value
   with self.subTest(key=key,value=value),self.assertRaisesRegex(ValueError,why):run(combined(rows))
  rows=copy.deepcopy(original);next(r for r in rows if r['record']=='profile_dataset' and not r['snapshot'])['next_snap_object']='3'
  with self.assertRaisesRegex(ValueError,'head next link'):run(combined(rows))
  for key in ('next_snap_object','snapnames_object'):
   rows=copy.deepcopy(original);del next(r for r in rows if r['record']=='profile_dataset')[key]
   with self.subTest(missing=key),self.assertRaises(ValueError):run(combined(rows))
if __name__=='__main__':unittest.main(verbosity=2)
