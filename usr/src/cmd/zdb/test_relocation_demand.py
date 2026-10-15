"""Synthetic complete captures plus accepted-runtime offline regression support."""
import copy
import hashlib
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest
from test_profile import combined
from test_verified_intervals import sample
from paired_intervals import canonical, digest, cut
from observed_intervals import FLAGS as F
import relocation_demand as d


def fixture(repeat=False, second_source=False, replica=False):
    bp=copy.deepcopy(sample()[3]);bp['bp_words'][6]='000b070200070007'
    dataset=dict(record='profile_dataset',object='2',snapshot=False,children='0',dataset_flags='0',next_snap_object='0',snapnames_object='0',zil_state='canonical_zero_header',root_bp=bp['bp_words'],dir_object='10',prev_snap_object='0',prev_snap_txg='0',creation_txg='10',space_bytes=['0']*4,next_clones_object='0',userrefs_object='0',props_object='0',zil_header_words=['0000000000000000']*24,**F)
    evidence=[dict(record='profile_scope',profile='ddt-absent-zil-zero-v1',origin_snapshot='0',origin_head='0',origin_dir='0',ddt_enumerated=False,zil_enumerated=False,**F),dict(record='profile_object',object='2',object_type=16,bonus_type=16,**F),dataset,dict(record='profile_end',profile_metadata_complete=True,error=0,objects='1',directory_entries='0',datasets='1',heads='1',snapshots='0',reads='7',read_bytes='5000',active_object='2',cached_ddt_slots='42',ddt_absence_checked=True,**F)]
    rows=combined(evidence)
    vd=copy.deepcopy(next(r for r in rows if r['record']=='vdev'));vd.update(id='1',guid='8')
    rows.insert(3,vd)
    ms=dict(record='metaslab',vdev_id='1',id='0',start='0',length='1048576',unflushed_txg='20',map_object='51',**F)
    mp=dict(record='allocation_map',source='base',map_object='51',log_txg='0',length='0',shift=12,**F)
    i=next(i for i,r in enumerate(rows) if r['record']=='pending_scope')
    rows[i:i]=[ms,mp]
    end=next(r for r in rows if r['record']=='allocation_end');end['records']=str(int(end['records'])+2)
    bp=next(r for r in rows if r['record']=='bp')
    if second_source or replica:
        v='1' if replica else '0';offset=4096 if replica else 16384
        dva=copy.deepcopy(bp['dvas'][0]);dva.update(slot=1,vdev_id=v,vdev_guid='8' if replica else '7',offset=str(offset))
        bp['dvas'].append(dva);bp['bp_words'][2]=f'{(int(v)<<32)|16:016x}';bp['bp_words'][3]=f'{offset//512:016x}'
        next(r for r in rows if r['record']=='verification')['bp_words']=copy.deepcopy(bp['bp_words'])
        next(r for r in rows if r['record']=='profile_dataset')['root_bp']=copy.deepcopy(bp['bp_words'])
        target=mp if replica else next(r for r in rows if r['record']=='allocation_map')
        target['length']=str(int(target['length'])+8)
        op=dict(record='allocation_op',source='base',map_object=target['map_object'],log_txg='0',ordinal='1' if replica else '2',vdev_id=v,metaslab='0',start=str(offset),length='8192',op='alloc',applied=True,**F)
        al=dict(record='allocation',vdev_id=v,vdev_guid='8' if replica else '7',metaslab='0',start=str(offset),length='8192',**F)
        # Maps/ops precede final coalesced allocations for each metaslab.
        i=rows.index(target)+1
        while rows[i]['record']=='allocation_op':i+=1
        rows.insert(i,op)
        i=rows.index(target)+1
        while rows[i]['record'] in ('allocation_op','allocation'):i+=1
        rows.insert(i,al)
        for key,add in [('records',2),('intervals',1),('bytes',8192),('map_bytes',8),('map_words',1)]:end[key]=str(int(end[key])+add)
    if repeat:
        i=rows.index(bp);pair=copy.deepcopy(rows[i:i+2])
        for r in pair:r['sequence']='2';r['objset']='99'
        rows[i+2:i+2]=pair
        next(r for r in rows if r['record']=='end')['observed']='2'
        ve=next(r for r in rows if r['record']=='verification_end')
        for k in ('seen','submitted','completed','passed'):ve[k]='2'
        ve['requested_p']=ve['verified_p']='8192'
    allocations=[r for r in rows if r['record']=='allocation']
    rows=[r for r in rows if r['record']!='allocation']
    i=next(i for i,r in enumerate(rows) if r['record']=='pending_scope')
    rows[i:i]=sorted(allocations,key=lambda r:(int(r['vdev_id']),int(r['start'])))
    return rows


def invoke(rows,**kwargs):
    data=b''.join(canonical(r)+b'\n' for r in rows)
    identity=cut(next(r for r in rows if r['record']=='pool'),[r for r in rows if r['record']=='vdev'])
    args=dict(exit_status='0',expected_sha=hashlib.sha256(data).hexdigest(),expected_cut_sha=digest(identity),source_id='0',source_guid='7',destination_id='1',destination_guid='8');args.update(kwargs)
    return d.analyze(data,**args)


class Tests(unittest.TestCase):
    def test_success(self):
        r=invoke(fixture());self.assertTrue(r['single_destination_contract_satisfied'])
        self.assertEqual(r,invoke(fixture()))
        self.assertEqual(r['destination_base_demand_bytes'],'8192');self.assertEqual(r['physical_extent_count'],'1')
        self.assertTrue(all(r[k] is False for k in d.SAFETY));self.assertEqual(r['destination_captured_gross_free_bytes'],'1048576')
    def test_repeated_references(self):
        r=invoke(fixture(repeat=True));self.assertEqual(r['physical_extent_count'],'1');self.assertEqual(r['observed_reference_count'],'2')
        self.assertEqual(r['source_reference_bytes'],'16384');self.assertEqual(r['destination_base_demand_bytes'],'8192')
        self.assertEqual([x['original_bp_record']['objset'] for x in r['extents'][0]['references']],['0','99'])
    def test_distinct_DVAs(self):
        r=invoke(fixture(second_source=True));self.assertEqual(r['physical_extent_count'],'2');self.assertEqual(r['destination_base_demand_bytes'],'16384')
    def test_replica_conflict(self):
        r=invoke(fixture(replica=True));self.assertFalse(r['single_destination_contract_satisfied']);self.assertEqual(r['refusal_reasons'],['destination_replica_conflict'])
        self.assertEqual(r['replica_conflicts'][0]['sequence'],'1')
    def test_identity_and_evidence_refusals(self):
        for kw in (dict(expected_sha='0'*64),dict(expected_cut_sha='0'*64),dict(exit_status='1'),dict(source_guid='8'),dict(destination_guid='7'),dict(destination_id='0',destination_guid='7'),dict(destination_id='99')):
            with self.subTest(kw=kw),self.assertRaises(ValueError):invoke(fixture(),**kw)
        for kind,key,val in [('profile_end','profile_metadata_complete',False),('cut_end','txg','31'),('verification','status','failed'),('allocation','length','4096'),('bp','unsupported',True)]:
            rows=fixture();next(r for r in rows if r['record']==kind)[key]=val
            with self.subTest(kind=kind),self.assertRaises(ValueError):invoke(rows)
    def test_conflicting_descriptors(self):
        rows=fixture(repeat=True)
        for r in rows:
            if r.get('sequence')=='2' and 'bp_words' in r:r['bp_words'][12]='0000000000000001'
        with self.assertRaises(ValueError):invoke(rows)
    def test_geometry_and_overflow(self):
        self.assertEqual(d.raidz_asize(4096,12,3,1),8192)
        self.assertEqual(d.raidz_asize(4096,12,6,2),12288)
        self.assertEqual(d.raidz_asize(4097,12,3,1),16384)
        for args in [(0,12,3,1),(4096,8,3,1),(4096,12,3,3),(4096,12,5,4),(True,12,3,1)]:
            with self.assertRaises(ValueError):d.raidz_asize(*args)
        with self.assertRaises(ValueError):d.checked(1<<64)
    def test_destination_geometry_and_capacity(self):
        rows=fixture();v=next(r for r in rows if r['record']=='vdev' and r['id']=='1');v.update(children='6',parity='2')
        self.assertEqual(invoke(rows)['destination_base_demand_bytes'],'12288')
        rows=fixture();next(r for r in rows if r['record']=='metaslab' and r['vdev_id']=='1')['length']='4096'
        r=invoke(rows);self.assertFalse(r['single_destination_contract_satisfied']);self.assertIn('base_demand_exceeds_captured_gross_free_bytes',r['refusal_reasons'])
        r=invoke(fixture(),source_id='1',source_guid='8',destination_id='0',destination_guid='7')
        self.assertEqual(r['refusal_reasons'],['source_has_no_allocated_extents'])
        for key,value in [('type','mirror'),('parity','0'),('ashift','8'),('allocation_class','special')]:
            rows=fixture();next(r for r in rows if r['record']=='vdev' and r['id']=='1')[key]=value
            with self.subTest(key=key),self.assertRaises(ValueError):invoke(rows)
    def test_overlap_and_missing_evidence(self):
        rows=fixture(second_source=True)
        for r in rows:
            if r['record'] in ('bp','verification','profile_dataset'):
                w=r.get('bp_words',r.get('root_bp'));w[3]='0000000000000010'
            if r['record']=='bp':r['dvas'][1]['offset']='8192'
            if r['record'] in ('allocation_op','allocation') and r.get('start')=='16384':r['start']='8192'
        with self.assertRaises(ValueError):invoke(rows)
        rows=fixture();rows=[r for r in rows if r['record']!='verification']
        with self.assertRaises(ValueError):invoke(rows)
    def test_native_geometry(self):
        path=os.environ.get('RELOCATION_NATIVE_GEOMETRY')
        if not path:self.skipTest('native libzpool geometry harness not supplied')
        lines=subprocess.check_output([path],text=True).splitlines();self.assertGreater(len(lines),1000)
        for line in lines:
            p,a,n,k,want=map(int,line.split());self.assertEqual(d.raidz_asize(p,a,n,k),want,line)
    def test_regular_files_and_cli(self):
        with tempfile.TemporaryDirectory() as td:
            td=pathlib.Path(td);cap=td/'capture';status=td/'exit';out=td/'result'
            rows=fixture(replica=True);cap.write_bytes(b''.join(canonical(r)+b'\n' for r in rows));status.write_text('0\n')
            result=invoke(rows)
            cmd=[sys.executable,str(pathlib.Path(d.__file__)),str(cap),'--sha256',result['input_sha256'],'--cut-sha256',result['cut_sha256'],'--source-id','0','--source-guid','7','--destination-id','1','--destination-guid','8','--exit-status',str(status),'--output',str(out)]
            r=subprocess.run(cmd,capture_output=True);self.assertEqual(r.returncode,3,r.stderr)
            saved=out.read_bytes();self.assertFalse(json.loads(saved)['placement_performed'])
            self.assertEqual(subprocess.run(cmd,capture_output=True).returncode,2);self.assertEqual(out.read_bytes(),saved)
            link=td/'link';link.symlink_to(cap)
            with self.assertRaises(ValueError):d.read_regular(link,d.LIMIT)
            fifo=td/'fifo';os.mkfifo(fifo)
            with self.assertRaises(ValueError):d.read_regular(fifo,d.LIMIT)

if __name__=='__main__':unittest.main(verbosity=2)
