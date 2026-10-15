"""Pool-free production parent-walker and decoded-membership tests."""
import copy,hashlib,json,os,pathlib,subprocess,tempfile,unittest
from parent_edges import metadata,analyze
BASE=pathlib.Path('/build/workspaces/raidz');BUILD=pathlib.Path(os.environ.get('ZDB_INVENTORY_BUILD',str(BASE/'inventory-edges-v1')))
ENV=dict(os.environ,LD_LIBRARY_PATH_64=':'.join(map(str,[BUILD/'lib',BASE/'smartos-live/proto/usr/lib/amd64',BASE/'smartos-live/proto/lib/amd64'])))
def anchors(rows):
 edges=[r for r in rows if r['record']=='parent_edge']
 return edges[0]['child_bp'],{r['bookmark'][0]:r['child_bp'] for r in edges if r['kind']=='dataset_bonus'}
def full_fixture(edges):
 from test_relocation_demand import fixture
 from paired_intervals import union
 from observed_intervals import FLAGS as F
 base=fixture();root,ds=anchors(edges)
 btemplate=copy.deepcopy(next(r for r in base if r['record']=='bp'));vtemplate=copy.deepcopy(next(r for r in base if r['record']=='verification'))
 observations=[];seen=set();spans=[];totalp=0
 for e in edges:
  if e['record']!='parent_edge' or e['disposition'] in ('hole','embedded') or tuple(e['child_bp']) in seen:continue
  seen.add(tuple(e['child_bp']));w=[int(x,16) for x in e['child_bp']];prop=w[6];p=((prop>>16&65535)+1)*512;n=len(seen)
  b=copy.deepcopy(btemplate);b.update(sequence=str(n),objset=e['bookmark'][0],object=e['bookmark'][1],level=e['bookmark'][2],blkid=e['bookmark'][3],bp_words=e['child_bp'],bp_type=prop>>48&255,bp_level=prop>>56&31,lsize=str((prop&65535)+1 and ((prop&65535)+1)*512),psize=str(p),metadata=bool((prop>>56&31) or (prop>>48&255) in (10,11)))
  b['dvas'][0].update(offset=str(w[1]*512),asize=str((w[0]&0xffffff)*512),qsize=str(((p+4095)//4096)*4096),expected_asize=str((w[0]&0xffffff)*512))
  v=copy.deepcopy(vtemplate)
  for k in ('sequence','objset','object','level','blkid','bp_words'):v[k]=copy.deepcopy(b[k])
  v.update(requested_p=str(p),completed_p=str(p));observations.extend([b,v]);totalp+=p;spans.append([w[1]*512,w[1]*512+(w[0]&0xffffff)*512])
 base=[r for r in base if r['record'] not in ('bp','verification','allocation_op','allocation','profile_object','profile_dataset')]
 base[0]['schema']='zdb-parent-edges-v1';next(r for r in base if r['record']=='pool')['root_bp']=root;next(r for r in base if r['record']=='cut_end')['root_bp']=root
 i=next(i for i,r in enumerate(base) if r['record']=='allocation_scope');base[i:i]=observations
 mp=next(r for r in base if r['record']=='allocation_map');mp['length']=str(len(spans)*8);i=base.index(mp)+1
 ops=[dict(record='allocation_op',source='base',map_object='50',log_txg='0',ordinal=str(j+1),vdev_id='0',metaslab='0',start=str(a),length=str(b-a),op='alloc',applied=True,**F) for j,(a,b) in enumerate(sorted(spans))];base[i:i]=ops
 merged=union(spans);i=next(i for i,r in enumerate(base) if r['record']=='pending_scope')
 base[i:i]=[dict(record='allocation',vdev_id='0',vdev_guid='7',metaslab='0',start=str(a),length=str(b-a),**F) for a,b in merged]
 dsrows=[]
 for obj in ('2','3'):
  dsrows.append(dict(record='profile_object',object=obj,object_type=16,bonus_type=16,**F))
  dsrows.append(dict(record='profile_dataset',object=obj,snapshot=obj=='3',children='1' if obj=='3' else '0',dataset_flags='0',next_snap_object='2' if obj=='3' else '0',snapnames_object='0',zil_state='snapshot_not_applicable' if obj=='3' else 'canonical_zero_header',root_bp=ds[obj],dir_object='10',prev_snap_object='0',prev_snap_txg='25',creation_txg='0',space_bytes=['0']*4,next_clones_object='0',userrefs_object='0',props_object='0',zil_header_words=None if obj=='3' else ['0000000000000000']*24,**F))
 pe=next(r for r in base if r['record']=='profile_end');pe.update(objects='2',datasets='2',snapshots='1',reads='10',active_object='3');base[base.index(pe):base.index(pe)]=dsrows
 ve=next(r for r in base if r['record']=='verification_end')
 for k in ('seen','submitted','completed','passed'):ve[k]=str(len(seen))
 ve.update(requested_p=str(totalp),verified_p=str(totalp))
 ae=next(r for r in base if r['record']=='allocation_end');ae.update(records=str(sum(r['record'] in ('allocation_scope','metaslab','allocation_map','allocation_op','allocation') for r in base)),intervals=str(len(merged)),bytes=str(sum(b-a for a,b in merged)),map_bytes=str(len(spans)*8),map_words=str(len(spans)))
 next(r for r in base if r['record']=='end')['observed']=str(len(seen))
 i=next(i for i,r in enumerate(base) if r['record']=='cut_end');base[i:i]=copy.deepcopy(edges)
 return base

class Tests(unittest.TestCase):
 @classmethod
 def setUpClass(cls):cls.tmp=pathlib.Path(tempfile.mkdtemp(prefix='edge-tests-',dir=BUILD))
 def invoke(self,mode):
  path=self.tmp/(mode+'.jsonl');r=subprocess.run([str(BUILD/'test_edges'),mode,str(path)],env=ENV,capture_output=True,text=True)
  self.assertIn('writes=0',r.stdout,r.stderr);return r,[json.loads(s) for s in path.read_text().splitlines()]
 def test_aliases_and_birth_pruning(self):
  r,rows=self.invoke('normal');self.assertEqual(r.returncode,0,r.stdout)
  root,ds=anchors(rows);self.assertEqual(set(ds),{'2','3'});self.assertEqual(ds['2'],ds['3'])
  result=metadata(rows,root,ds);self.assertEqual(result['containers'],8)
  # Both logical aliases reach the same leaf via distinct parent observation IDs.
  leaves=[r for r in rows if r['record']=='parent_edge' and r['disposition']=='leaf']
  self.assertEqual({r['bookmark'][0] for r in leaves},{'2','3'})
  a=[r for r in leaves if r['kind']=='indirect'];self.assertEqual(len(a),2)
  self.assertEqual(a[0]['child_bp'],a[1]['child_bp']);self.assertNotEqual(a[0]['parent_node'],a[1]['parent_node'])
  self.assertEqual({r['kind'] for r in rows if r['record']=='parent_edge'},{'selected_uberblock','dnode','dataset_bonus','indirect','spill'})
  self.assertFalse(result['pointer_rewrite_ready']);self.assertFalse(result['coverage_complete'])
 def test_hole_dataset_root(self):
  r,rows=self.invoke('root-hole');self.assertEqual(r.returncode,0,r.stdout)
  root,ds=anchors(rows);self.assertEqual(ds['3'],['0000000000000000']*16)
  self.assertEqual(metadata(rows,root,ds)['containers'],5)
 def test_layout_and_read_refusals(self):
  for mode in ('large-dnode','bonus-bounds','dataset-bonus','dataset-type','indirect-size','level','checksum','arc-error','arc-size','writable','cycle','embedded-parent','future-birth','unknown-flags'):
   with self.subTest(mode=mode):
    r,rows=self.invoke(mode);self.assertEqual(r.returncode,1,r.stdout);self.assertFalse(rows[-1]['parent_capture_complete'])
    with self.assertRaises(ValueError):metadata(rows,['0000000000000000']*16,{})
 def test_pre_read_budgets(self):
  for mode in ('visits','reads','bytes','output','depth'):
   r,_=self.invoke('budget-'+mode);self.assertEqual(r.returncode,1);self.assertIn('reads=0',r.stdout)
 def test_membership_and_completeness_mutations(self):
  _,original=self.invoke('normal');root,ds=anchors(original)
  cases=[('parent_edge','decoded_offset','8'),('parent_edge','parent_bp',['0000000000000000']*16),('parent_edge','parent_bookmark',['99','0','0','0']),('parent_edge','slot','99'),('parent_edge','disposition','leaf'),('parent_container','authentication','unchecked'),('parent_container','decoded_size','1'),('parent_end','nodes','1'),('parent_end','output_charge','0'),('parent_end','parent_capture_complete',False)]
  for kind,key,value in cases:
   rows=copy.deepcopy(original)
   target=next(r for r in rows if r['record']==kind and (kind!='parent_edge' or r['parent_node']!='0'))
   target[key]=value
   with self.subTest(kind=kind,key=key),self.assertRaises(ValueError):metadata(rows,root,ds)
  for kind in ('parent_edge','parent_container'):
   rows=copy.deepcopy(original);i=next(i for i,r in enumerate(rows) if r['record']==kind and (kind!='parent_edge' or r['parent_node']!='0'));rows.pop(i)
   with self.subTest(missing=kind),self.assertRaises(ValueError):metadata(rows,root,ds)
  rows=copy.deepcopy(original);container=next(r for r in rows if r['record']=='parent_container');container['decoded_hex']='ff'*int(container['decoded_size'])
  with self.assertRaises(ValueError):metadata(rows,root,ds)
  with self.assertRaises(ValueError):metadata(original,root,{'2':ds['2']})
 def test_full_profile_projection_and_binding(self):
  from paired_intervals import canonical
  _,edges=self.invoke('normal');rows=full_fixture(edges)
  def check(rows):
   data=b''.join(canonical(r)+b'\n' for r in rows);return analyze(data,'0',hashlib.sha256(data).hexdigest())
  r=check(rows);self.assertTrue(r['parent_evidence_validated']);self.assertEqual(r['dataset_roots'],2)
  (self.tmp/'full-synthetic.jsonl').write_bytes(b''.join(canonical(r)+b'\n' for r in rows))
  for kind,key,value in [('profile_end','profile_metadata_complete',False),('cut_end','txg','31'),('parent_scope','snapshot_birth_pruning',True),('profile_dataset','dir_object','99')]:
   bad=copy.deepcopy(rows);next(r for r in bad if r['record']==kind)[key]=value
   with self.subTest(kind=kind),self.assertRaises(ValueError):check(bad)
 def test_W_prevalidation(self):
  target=self.tmp/'not-created'
  for args in (['-W',str(target),'pool'],['-W',str(target),'-Q',str(target),'-e','-p','/nonexistent','pool'],['-W',str(target),'-e','-p','relative','pool']):
   r=subprocess.run([str(BUILD/'zdb-inventory'),*args],env=ENV,capture_output=True,text=True)
   self.assertEqual(r.returncode,2,r.stderr);self.assertFalse(target.exists())
  with self.assertRaises(ValueError):analyze(b'{}\n','1','0'*64)
if __name__=='__main__':unittest.main(verbosity=2)
