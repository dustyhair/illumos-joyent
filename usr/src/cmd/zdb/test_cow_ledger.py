"""Portable complete captures and adversarial symbolic COW proposals."""
import copy
import hashlib
import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest

from paired_intervals import canonical, digest
from parent_edges import SAFETY as PARENT_FLAGS, raw
from reference_graph import analyze as graph_analyze
from test_edges import full_fixture
from test_semantic_references import bp, dnode, embed, zap, fletcher
import semantic_references as semantic
import cow_ledger as c


def fixture():
    def pointer(typ,offset,size,checksum=None):
        w=bp(typ,offset,size,checksum);w[0]='0000000000000010';w[10]='0000000000000014'
        return w
    root=pointer(11,65536,4096);mos=pointer(10,81920,2048)
    dsroot=pointer(11,131072,4096);dn=pointer(10,163840,1024)
    data=pointer(19,196608,512);extra=pointer(10,245760,2048)
    stored=b''.join(raw(list(semantic.normalize(data))) for _ in range(7)).ljust(1024,b'\0')
    owner=pointer(5,262144,1024,fletcher(stored))
    rootdata=bytearray(4096);rd=dnode(10,mos,blocksize=2048,count=3)
    rd[192:320]=raw(mos);rd[320:448]=raw(extra);rootdata[:512]=rd
    deadlist=embed(zap([('1',10)]),50);deadlist[10]='0000000000000014'
    mosdata=bytearray(2048);mosdata[512:1024]=dnode(50,deadlist,51,bytes(320),count=1)
    for obj in (2,3):
        bonus=bytearray(320)
        for index,value in [(0,10),(2,25),(3,2 if obj==3 else 0),(5,1 if obj==3 else 0),(8,1)]:
            struct.pack_into('<Q',bonus,index*8,value)
        bonus[128:256]=raw(dsroot)
        mosdata[obj*512:(obj+1)*512]=dnode(16,['0'*16]*16,16,bonus,count=1)
    extradata=bytearray(2048)
    extradata[1024:1536]=dnode(5,owner,6,struct.pack('<6Q',7,57344,3584,3584,0,0),1024)
    dsdata=bytearray(4096);dsdata[:512]=dnode(10,dn,blocksize=1024,count=1)
    dndata=bytearray(1024);dndata[512:]=dnode(19,data,count=1)
    rows=[dict(record='parent_scope',encoding='decoded-amd64-little-endian',snapshot_birth_pruning=False,
        scope='current_MOS_and_dataset_tree_pointer_slots',semantic_BP_payloads_enumerated=False,
        historical_roots_enumerated=False,anchor='selected_import_uberblock_not_independent_label_proof',**PARENT_FLAGS)]
    edges={}
    def edge(parent,kind,bm,pointer,offset=0,base=0,slot=0,decoded=None):
        node=str(len(edges)+1);w=[int(v,16) for v in pointer]
        disposition='container' if decoded is not None else 'embedded' if w[6]&(1<<39) else 'leaf' if w[0]&0xffffff else 'hole'
        r=dict(record='parent_edge',node=node,parent_node=parent,kind=kind,bookmark=bm,
            decoded_offset=str(offset),dnode_offset=str(base),slot=str(slot),child_bp=pointer,
            parent_bp=edges[parent]['child_bp'] if parent!='0' else None,
            parent_bookmark=edges[parent]['bookmark'] if parent!='0' else None,
            disposition=disposition,**PARENT_FLAGS)
        edges[node]=r;rows.append(r)
        if decoded is not None:
            rows.append(dict(record='parent_container',node=node,decoded_size=str(len(decoded)),
                authentication='original_bp_arc_checksum',decoded_hex=bytes(decoded).hex(),**PARENT_FLAGS))
        return node
    edge('0','selected_uberblock',['0','0','-1','0'],root,decoded=rootdata)
    for blkid in (0,1):
        parent=edge('1','dnode',['0','0','0',str(blkid)],mos,64+128*blkid,slot=blkid,decoded=mosdata)
        first=4*blkid
        edge(parent,'dnode',['0',str(first+1),'0','0'],deadlist,576,512)
        for obj in (first+2,first+3):
            base=(obj-first)*512;edge(parent,'dnode',['0',str(obj),'0','0'],['0'*16]*16,base+64,base)
        for obj in (first+2,first+3):
            base=(obj-first)*512
            anchor=edge(parent,'dataset_bonus',[str(obj),'0','-1','0'],dsroot,base+320,decoded=dsdata)
            node=edge(anchor,'dnode',[str(obj),'0','0','0'],dn,64,decoded=dndata)
            edge(node,'dnode',[str(obj),'1','0','0'],data,576,512)
    node=edge('1','dnode',['0','0','0','2'],extra,320,slot=2,decoded=extradata)
    for slot,pointer in enumerate([owner,['0'*16]*16,['0'*16]*16]):
        edge(node,'dnode',['0','10','0',str(slot)],pointer,1088+slot*128,1024,slot)
    containers=[r for r in rows if r['record']=='parent_container'];total=sum(int(r['decoded_size']) for r in containers)
    rows.append(dict(record='parent_end',parent_capture_complete=True,error=0,nodes=str(len(edges)),
        containers=str(len(containers)),decoded_bytes=str(total),dataset_roots='4',
        output_charge=str(4096*len(edges)+2048*len(containers)+2*total),**PARENT_FLAGS))
    full=full_fixture(rows);end=next(r for r in full if r['record']=='profile_end');additions=[]
    for old,new in [('2','6'),('3','7')]:
        for kind in ('profile_object','profile_dataset'):
            r=copy.deepcopy(next(r for r in full if r['record']==kind and r['object']==old));r['object']=new;additions.append(r)
    full[full.index(end):full.index(end)]=additions
    end.update(objects='4',datasets='4',heads='2',snapshots='2',active_object='7',reads='17',read_bytes='8704')
    encoded=b''.join(canonical(r)+b'\n' for r in full);sha=hashlib.sha256(encoded).hexdigest()
    cut=dict(guid=next(r for r in full if r['record']=='pool')['guid'],txg='30',root_bp=root,
             topology=sorted((r for r in full if r['record']=='vdev'),key=lambda r:int(r['id'])))
    cutsha=digest(cut);target=dict(vdev_id='0',vdev_guid='7',offset='196608',asize='8192',dva_slot='0')
    leaf=canonical(dict(schema='semantic-leaf-evidence-v1',capture_sha256=sha,cut_sha256=cutsha,
        leaves=[dict(bp_words=owner,source_replica=dict(slot=0,vdev_id='0',vdev_guid='7',offset='262144',asize='8192'),physical_hex=stored.hex())]))
    return encoded,sha,cutsha,target,leaf,hashlib.sha256(leaf).hexdigest()


class Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.inputs=fixture();data,sha,cut,target,leaf,leafsha=cls.inputs
        cls.selection=dict(dataset_object='2',object='1',blkid='0')
        cls.graph=graph_analyze(data,'0',sha,cut,target);cls.records=[parse for parse in map(c.parse,data.splitlines())]
        sb=semantic.Budget();cls.payloads=semantic.leaf_payloads(c.parse(leaf),cls.graph,sb)
        cls.sem=semantic._build(cls.graph,cls.records,cls.payloads,sb)
        cls.context=c._context(cls.graph,cls.sem,cls.records,cls.payloads,cls.selection,leafsha,c.Budget())

    def plan(self):return c.draft(self.context)
    def public(self,proposal=None,**kwargs):
        data,sha,cut,target,leaf,leafsha=self.inputs
        args=dict(data=data,status='0',sha=sha,cut_sha=cut,target=target,selection=self.selection,leaf_data=leaf,leaf_sha=leafsha)
        if proposal is not None:
            b=canonical(proposal);args.update(proposal_data=b,proposal_sha=hashlib.sha256(b).hexdigest())
        args.update(kwargs);return c.analyze(**args)

    def test_complete_public_revalidation_and_counts(self):
        result=self.public();p=result['plan'];counts=result['counts']
        self.assertEqual(counts['selected_route_replacement_blocks'],5)
        self.assertEqual(counts['preserved_stored_references'],7)
        self.assertEqual(len({r['id'] for r in p['replacements']}),5)
        self.assertEqual({r['dataset_object'] for r in p['preserved_dataset_roots']},{'3','6','7'})
        self.assertTrue(all(result[k] is False for k in c.SAFETY))
        self.assertTrue(all(r['new_logical_birth'] is r['new_physical_birth'] is None for r in p['replacements']))
        self.assertTrue(all(r['destination_vdev'] is r['physical_offset'] is None for r in p['new_allocations']))
        self.assertEqual(result,self.public(p))

    def test_selected_route_is_not_a_frozen_header_stock_transaction(self):
        result=self.public();p=result['plan'];counts=result['counts']
        self.assertFalse(result['full_stock_transaction_enumerated'])
        self.assertFalse(result['stock_kill_side_effects_simulated'])
        self.assertIsNone(counts['full_stock_transaction_replacement_count'])
        self.assertEqual((counts['stock_dataset_deadlist_path_blocks'],counts['stock_dataset_free_path_blocks'],
                          counts['stock_MOS_free_path_blocks']),(3,0,2))
        self.assertTrue(all(r['actual_stock_after_header'] is None and r['actual_stock_new_entry_count'] is None
                            for r in p['preserved_deadlists']))
        self.assertIn('stock_COW_deadlist_BPOBJ_counter_and_MOS_expansion',{r['kind'] for r in p['obligations']})
        for mode in ('count','stock-header','scope'):
            changed=self.plan()
            if mode=='count':changed['full_stock_transaction_replacement_count']=5
            elif mode=='stock-header':changed['preserved_deadlists'][0]['actual_stock_after_header']=changed['preserved_deadlists'][0]['header_u64']
            else:changed['model_scope']='ordinary_stock_transaction'
            with self.subTest(mode=mode),self.assertRaises(ValueError):c.validate_plan(self.context,changed)

    def test_shared_physical_slots_keep_distinct_logical_references(self):
        p=self.plan();seed=next(r for r in p['replacements'] if r['original_block']==self.graph['target']['block'])
        self.assertEqual(len(seed['original_logical_references']),4)
        self.assertEqual(sum(r['selected_for_new_copy'] for r in seed['original_logical_references']),1)
        self.assertEqual({r['bookmark'][0] for r in seed['original_logical_references']},{'2','3','6','7'})
        self.assertEqual({r['bookmark'][0] for r in p['reference_updates'] if r['kind']=='dataset_bonus'},{'2'})
        self.assertTrue(all(r['matched_candidate_in_selected_route'] and r['historical_original_identity_unresolved']
                            for r in p['preserved_deadlists'][0]['stored_references']))

    def test_accidental_alias_updates_refused(self):
        for mode in ('extra','snapshot','root'):
            p=self.plan()
            if mode=='extra':
                r=copy.deepcopy(p['reference_updates'][0]);r['bookmark'][0]='3';p['reference_updates'].append(r)
            elif mode=='snapshot':p['reference_updates'][0]['bookmark'][0]='3'
            else:p['preserved_dataset_roots'][0]['after_root_block']=p['replacements'][0]['id']
            with self.subTest(mode=mode),self.assertRaisesRegex(ValueError,'alias'):c.validate_plan(self.context,p)

    def test_freed_reused_or_missing_retained_allocations_refused(self):
        for mode in ('free','reuse','missing','retain','remove'):
            p=self.plan()
            if mode=='free':p['freed_allocations']=[p['allocation_before'][0]['id']]
            elif mode=='reuse':p['reused_allocations']=[p['allocation_before'][0]['id']]
            elif mode=='missing':p['allocation_after']=p['allocation_after'][1:]
            elif mode=='retain':p['retained_allocation_ids']=p['retained_allocation_ids'][1:]
            else:p['source_vdev_removed']=True
            with self.subTest(mode=mode),self.assertRaises(ValueError):c.validate_plan(self.context,p)

    def test_symbolic_overlap_and_physical_reuse_refused(self):
        for mode in ('overlap','old-space','identity','offset','destination','duplicate','overflow','bool-slot'):
            p=self.plan();a=p['new_allocations'][0];b=p['new_allocations'][1]
            if mode=='overlap':b['begin']=a['begin'];b['end']=str(int(a['begin'])+int(b['length_bound']))
            elif mode=='old-space':a['address_space']=p['allocation_before'][0]['address_space']
            elif mode=='identity':a['id']=p['allocation_before'][0]['id']
            elif mode=='offset':a['physical_offset']='0'
            elif mode=='destination':a['destination_vdev']='1'
            elif mode=='overflow':a['end']=str(c.U64+1)
            elif mode=='bool-slot':a['replica_slot']=False
            else:p['new_allocations'][1]=copy.deepcopy(a)
            p['allocation_after']=[dict(r) for r in p['allocation_before']+p['new_allocations']]
            with self.subTest(mode=mode),self.assertRaises(ValueError):c.validate_plan(self.context,p)

    def test_symbolic_arena_repacking_is_not_physical_placement(self):
        p=self.plan()
        for r in p['new_allocations']:
            r['begin']=str(int(r['begin'])+4096);r['end']=str(int(r['end'])+4096)
        p['allocation_after']=[dict(r) for r in p['allocation_before']+p['new_allocations']]
        counts=c.validate_plan(self.context,p)
        self.assertEqual(counts['symbolic_fresh_bytes_bound'],c.validate_plan(self.context,self.plan())['symbolic_fresh_bytes_bound'])

    def test_missing_or_invented_counter_birth_obligations_refused(self):
        for mode in ('missing','counter-missing','counter-zero','duplicate','resolved','delta','birth','replicas','compression'):
            p=self.plan()
            if mode=='missing':p['obligations']=p['obligations'][1:]
            elif mode=='counter-missing':p['obligations']=[r for r in p['obligations'] if r['kind']!='selected_dataset_referenced_unique_and_directory_counters']
            elif mode=='counter-zero':next(r for r in p['obligations'] if r['kind']=='selected_dataset_referenced_unique_and_directory_counters')['delta']='0'
            elif mode=='duplicate':p['obligations'].append(copy.deepcopy(p['obligations'][0]))
            elif mode=='resolved':p['obligations'][0]['status']='resolved'
            elif mode=='delta':p['obligations'][0]['delta']='0'
            elif mode=='birth':p['replacements'][0]['new_logical_birth']='31'
            elif mode=='replicas':p['replacements'][0]['modeled_replica_count']=0
            else:p['replacements'][1]['modeled_compression']['modeled_psize']='512'
            with self.subTest(mode=mode),self.assertRaises(ValueError):c.validate_plan(self.context,p)

    def test_modified_deadlist_bytes_header_or_lossy_rewrite_refused(self):
        for mode in ('bytes','header','entry','identity','rewrite'):
            p=self.plan();d=p['preserved_deadlists'][0]
            if mode=='bytes':d['decoded_hex']='ff'+d['decoded_hex'][2:]
            elif mode=='header':d['header_u64'][1]='0'
            elif mode=='entry':d['stored_references'][0]['stored_bp_words'][0]='0'*16
            elif mode=='identity':d['stored_references'][0]['historical_original_identity_unresolved']=False
            else:p['semantic_rewrites']=[dict(object='10',entry_index='0',replacement=p['replacements'][0]['id'])]
            with self.subTest(mode=mode),self.assertRaisesRegex(ValueError,'deadlist|lossy'):c.validate_plan(self.context,p)

    def test_allocation_union_no_hidden_or_duplicate_allocations(self):
        for mode in ('extra','duplicate','changed'):
            p=self.plan()
            if mode=='extra':
                r=copy.deepcopy(p['allocation_after'][0]);r['id']='invented';p['allocation_after'].append(r)
            elif mode=='duplicate':p['allocation_after'].append(copy.deepcopy(p['allocation_after'][0]))
            else:p['allocation_after'][0]['end']='1'
            with self.subTest(mode=mode),self.assertRaisesRegex(ValueError,'union'):c.validate_plan(self.context,p)

    def test_source_extent_bounds_overlap_and_route_coverage(self):
        for mode in ('overlap','empty','missing-route'):
            records=copy.deepcopy(self.records)
            allocations=[r for r in records if r['record']=='allocation']
            if mode=='overlap':records.append(copy.deepcopy(allocations[0]))
            elif mode=='empty':allocations[0]['length']='0'
            else:
                remove=next(r for r in allocations if r['vdev_id']=='0' and int(r['start']) <= 196608 < int(r['start'])+int(r['length']))
                records.remove(remove)
            with self.subTest(mode=mode),self.assertRaisesRegex(ValueError,'retained|retaining'):
                c._context(self.graph,self.sem,records,self.payloads,self.selection,self.inputs[-1],c.Budget())

    def test_invalid_selection_input_bindings_and_budgets(self):
        for selection in (dict(dataset_object='3',object='1',blkid='0'),dict(dataset_object='2',object='9',blkid='0')):
            with self.subTest(selection=selection),self.assertRaises(ValueError):self.public(selection=selection)
        for key in c.DEFAULT_LIMITS:
            with self.subTest(budget=key),self.assertRaisesRegex(ValueError,'budget'):self.public(limits={key:0})
        for args in (dict(sha='0'*64),dict(cut_sha='0'*64),dict(status='1'),dict(leaf_sha='0'*64),dict(leaf_data=None)):
            with self.subTest(args=args),self.assertRaises(ValueError):self.public(**args)

    def test_cli_positive_exclusive_and_negative_output(self):
        with tempfile.TemporaryDirectory() as name:
            d=pathlib.Path(name);data,sha,cut,target,leaf,leafsha=self.inputs
            (d/'capture').write_bytes(data);(d/'exit').write_text('0');(d/'leaf').write_bytes(leaf)
            cmd=[sys.executable,str(pathlib.Path(c.__file__)),str(d/'capture'),'--sha256',sha,'--cut-sha256',cut,
                '--leaf-sha256',leafsha,'--exit-status',str(d/'exit'),'--leaf-evidence',str(d/'leaf'),
                '--output',str(d/'out'),'--dataset-object','2','--object','1','--blkid','0']
            for k,v in target.items():cmd.extend(['--target-'+k.replace('_','-'),v])
            r=subprocess.run(cmd,capture_output=True,text=True);self.assertEqual(r.returncode,0,r.stderr)
            saved=(d/'out').read_bytes();self.assertTrue(all(c.parse(saved)[k] is False for k in c.SAFETY))
            self.assertEqual(subprocess.run(cmd,capture_output=True).returncode,2);self.assertEqual((d/'out').read_bytes(),saved)
            p=self.plan();p['freed_allocations']=['old'];b=canonical(p);(d/'proposal').write_bytes(b)
            cmd[cmd.index('--output')+1]=str(d/'refused');cmd.extend(['--proposal',str(d/'proposal'),'--proposal-sha256',hashlib.sha256(b).hexdigest()])
            self.assertEqual(subprocess.run(cmd,capture_output=True).returncode,2);self.assertFalse((d/'refused').exists())


if __name__=='__main__':unittest.main(verbosity=2)
