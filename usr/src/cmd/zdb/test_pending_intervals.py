"""Synthetic pending-owner provenance and interval reconciliation tests."""
import copy,hashlib,unittest
from test_paired_intervals import fixture,F
from paired_intervals import analyze,canonical
from verified_intervals import analyze as verified
from test_verified_intervals import sample

def pending_fixture(offset=4096,duplicate=False,empty=False):
 b=fixture()[3]['bp_words'].copy();b[1]=f'{offset//512:016x}';b[12:]=['0000000000000000']*4
 rows=[dict(record='pending_scope',spa_version='26',spa_deferred_root='10',dsl_free_root='11',dsl_free_applicable=True,**F)]
 for q,obj,count in [('spa_deferred','10',0 if empty else 2 if duplicate else 1),('dsl_free','11',0)]:
  rows.append(dict(record='pending_object',queue=q,root_object=obj,object=obj,parent_object='0',parent_index='0',depth='0',entries=str(count),subobject_list='0',subobjects='0',bonus_bytes='48',accounted_bytes=str(8192*count),accounted_comp='0',accounted_uncomp='0',**F))
  for i in range(count):rows.append(dict(record='pending_owner',queue=q,root_object=obj,object=obj,entry_index=str(i),status='ordinary',error=0,payload_verifiable=False,movable_candidate=False,bp_words=b.copy(),**F))
 count=0 if empty else 2 if duplicate else 1
 for q,obj,n in [('spa_deferred','10',count),('dsl_free','11',0)]:rows.append(dict(record='pending_queue_end',queue=q,root_object=obj,status='enumerated' if n else 'empty',error=0,objects='1',entries=str(n),links='0',**F))
 rows.append(dict(record='pending_end',pending_complete=True,error=0,objects='2',entries=str(count),links='0',read_bytes=str(96+count*128),active_object='11',active_index='0',**F))
 return rows

def combine(base,pending):
 rows=copy.deepcopy(base);rows[0]['schema']=rows[0]['schema'].replace('-v1','-v2')
 next(r for r in rows if r['record']=='allocation_scope')['owner_obligations'][1]='deferred_free:pending_enumeration'
 i=next(i for i,r in enumerate(rows) if r['record']=='cut_end');rows[i:i]=copy.deepcopy(pending)
 return rows

def run(rows,fn=analyze,status='0'):
 data=b''.join(canonical(r)+b'\n' for r in rows);return fn(data,status,hashlib.sha256(data).hexdigest())
class Tests(unittest.TestCase):
 def test_preserves_original_and_live_overlap(self):
  old=run(fixture());r=run(combine(fixture(),pending_fixture()))
  for key in ('A_minus_observed','observed_minus_A','address_sets_equal','reconciliation_passed'):self.assertEqual(r[key],old[key])
  self.assertEqual(r['pending_vs_observed_overlaps'][0]['length'],'8192');self.assertEqual(r['pending_outside_A'],[])
  self.assertTrue(r['pending_free_enumeration_complete']);self.assertFalse(r['coverage_complete']);self.assertFalse(r['evacuation_authorized'])
 def test_explained_and_outside(self):
  base=fixture(extra=True,omit=True);old=run(base);r=run(combine(base,pending_fixture(16384)))
  self.assertEqual(r['A_minus_observed'],old['A_minus_observed']);self.assertFalse(r['address_sets_equal']);self.assertFalse(r['reconciliation_passed'])
  self.assertEqual(r['pending_explained_A_minus_observed'][0]['length'],'4096');self.assertEqual(r['pending_residual_A_minus_observed'],[])
  self.assertEqual(r['pending_outside_A'][0]['start'],'20480')
 def test_residual(self):
  r=run(combine(fixture(extra=True,omit=True),pending_fixture(32768)))
  self.assertEqual(r['pending_explained_A_minus_observed'],[]);self.assertEqual(r['pending_residual_A_minus_observed'][0]['start'],'16384')
 def test_duplicate_owners_retained(self):
  r=run(combine(fixture(),pending_fixture(duplicate=True)))
  self.assertEqual(len(r['pending_owner_records']),2);self.assertEqual(r['pending_owner_overlaps'][0]['references'],2)
 def test_partial_owner_overlap(self):
  pending=pending_fixture(duplicate=True)
  owners=[r for r in pending if r['record']=='pending_owner'];owners[1]['bp_words'][1]='0000000000000010'
  r=run(combine(fixture(),pending))
  self.assertEqual(r['pending_owner_overlaps'][0]['start'],'8192');self.assertEqual(r['pending_owner_overlaps'][0]['length'],'4096')
  self.assertEqual(r['pending_owner_overlaps'][0]['references'],2)
 def test_empty_is_explicit(self):
  r=run(combine(fixture(),pending_fixture(empty=True)))
  self.assertEqual(r['pending_owner_records'],[]);self.assertTrue(r['pending_free_enumeration_complete'])
 def test_verified_mode_keeps_live_read_counters(self):
  r=run(combine(sample(),pending_fixture()),verified)
  self.assertEqual(r['measured_read_summary']['verified_bp_references'],1);self.assertEqual(len(r['pending_owner_records']),1)
  self.assertTrue(all(x['payload_verifiable'] is x['movable_candidate'] is False for x in r['pending_owner_records']))
 def test_missing_section_terminal_and_extra(self):
  rows=fixture();rows[0]['schema']='zdb-observed-allocation-v2'
  with self.assertRaises(ValueError):run(rows)
  for index in (0,1,2,-3,-2,-1):
   pending=pending_fixture();pending.pop(index)
   with self.subTest(index=index),self.assertRaises((ValueError,KeyError)):run(combine(fixture(),pending))
  pending=pending_fixture();pending.insert(3,copy.deepcopy(pending[2]))
  with self.assertRaises(ValueError):run(combine(fixture(),pending))
 def test_provenance_and_counts(self):
  for kind,key,value in [('pending_scope','dsl_free_applicable',False),('pending_scope','dsl_free_root','10'),('pending_object','parent_object','9'),('pending_object','bonus_bytes','40'),('pending_object','entries','100001'),('pending_object','depth','65'),('pending_object','subobject_list','11'),('pending_owner','entry_index','1'),('pending_owner','queue','dsl_free'),('pending_owner','root_object','99'),('pending_owner','object','11'),('pending_owner','payload_verifiable',True),('pending_owner','movable_candidate',True),('pending_owner','error',5),('pending_queue_end','status','empty'),('pending_queue_end','entries','99'),('pending_end','pending_complete',False),('pending_end','error',5),('pending_end','entries','99'),('pending_end','read_bytes','1')]:
   pending=pending_fixture();next(r for r in pending if r['record']==kind)[key]=value
   with self.subTest(kind=kind,key=key),self.assertRaises(ValueError):run(combine(fixture(),pending))
 def test_raw_unsupported(self):
  for mutate in (lambda w:w.__setitem__(6,f'{int(w[6],16)|(1<<62):016x}'),lambda w:w.__setitem__(6,f'{int(w[6],16)|(1<<61):016x}'),lambda w:w.__setitem__(1,'8000000000000008'),lambda w:w.__setitem__(0,'0000000000000000'),lambda w:w.__setitem__(0,'0000000100000010'),lambda w:w.__setitem__(1,'0000000000000001'),lambda w:w.__setitem__(10,'000000000000001f')):
   pending=pending_fixture();mutate(pending[2]['bp_words'])
   with self.assertRaises(ValueError):run(combine(fixture(),pending))
 def test_exit_and_cut_failures(self):
  rows=combine(fixture(),pending_fixture())
  with self.assertRaises(ValueError):run(rows,status='1')
  next(r for r in rows if r['record']=='cut_end')['txg']='31'
  with self.assertRaises(ValueError):run(rows)
if __name__=='__main__':unittest.main(verbosity=2)
