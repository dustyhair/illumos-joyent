"""Pool-free synthetic serializer fixtures, never passed to certificate model."""
import copy,hashlib,json,random,unittest
import paired_intervals as p
F=p.FLAGS

def fixture(extra=False,omit=False,log=False):
    # Hand-built BP is only an interval/schema test fixture, not real observation.
    w=['0000000000000010','0000000000000008']+['0000000000000000']*4+['0000130200070007']+['0000000000000000']*3+['0000000000000014']+['0000000000000000']*5
    bp=dict(record='bp',sequence='1',classification='ordinary',objset='0',object='0',level='-1',blkid='0',bp_words=w,dnode_present=False,bp_type=19,bp_level=0,logical_birth='20',physical_birth='20',lsize='4096',psize='4096',checksum_id=19,compression_id=2,metadata=True,dvas=[dict(slot=0,vdev_id='0',offset='4096',asize='8192',gang=False,vdev_guid='7',ashift='12',allocation_class='normal',alloc_bias=0,is_log=False,qsize='4096',expected_asize='8192',geometry_valid=True)],unsupported=False,children_skipped=False,**F)
    # property word: type 19, checksum 7, compression 2, sizes 4096.
    bp['bp_words'][6]='0013070200070007';bp['checksum_id']=7
    begin=dict(record='begin',schema='zdb-observed-allocation-v1',bp_encoding='host-normalized-u64-hex',source_base='da512a7e2e6716980271582e436faef06a575f71',record_limit='1000000',**F)
    pool=dict(record='pool',guid='9',txg='30',root_bp=w,**F)
    vd=dict(record='vdev',id='0',guid='7',type='raidz',ashift='12',children='3',parity='1',allocation_class='normal',alloc_bias=0,is_log=False,asize='1048576',metaslab_count='1',**F)
    scope=dict(record='allocation_scope',checkpoint_unsupported=False,removal_unsupported=False,allocation_adjustments='none',owner_obligations=['DDT:not_enumerated','deferred_free:not_enumerated','async_destroy:not_enumerated','ZIL:not_enumerated'],**F)
    ms=dict(record='metaslab',vdev_id='0',id='0',start='0',length='1048576',unflushed_txg='20',map_object='50',**F)
    spans=[] if omit else [[4096,12288]]
    if extra:spans.append([16384,20480])
    records=[begin,pool,vd,bp,scope,ms,dict(record='allocation_map',source='base',map_object='50',log_txg='0',length=str(len(spans)*8),shift=12,**F)]
    for i,(s,e) in enumerate(spans):records.append(dict(record='allocation_op',source='base',map_object='50',log_txg='0',ordinal=str(i+1),vdev_id='0',metaslab='0',start=str(s),length=str(e-s),op='alloc',applied=True,**F))
    if log:
        records.append(dict(record='allocation_map',source='log',map_object='51',log_txg='20',length='16',shift=9,**F))
        records.append(dict(record='allocation_op',source='log',map_object='51',log_txg='20',ordinal='1',vdev_id='0',metaslab='0',start='24576',length='4096',op='alloc',applied=True,**F))
        spans.append([24576,28672])
    for s,e in spans:records.append(dict(record='allocation',vdev_id='0',vdev_guid='7',metaslab='0',start=str(s),length=str(e-s),**F))
    records.append(dict(record='cut_end',txg='30',root_bp=w,**F))
    mapbytes=sum(int(r['length']) for r in records if r['record']=='allocation_map')
    records.append(dict(record='allocation_end',records=str(len(records)-5),intervals=str(len(spans)),bytes=str(sum(e-s for s,e in spans)),map_bytes=str(mapbytes),map_words=str(mapbytes//8),**F))
    records.append(dict(record='end',export_complete=True,error=0,observed='1',unsupported='0',**F))
    return records

def run(rows,status='0'):
    data=b''.join(p.canonical(r)+b'\n' for r in rows)
    return p.analyze(data,status,hashlib.sha256(data).hexdigest())
class Tests(unittest.TestCase):
    def test_exact(self):
        r=run(fixture());self.assertTrue(r['address_sets_equal']);self.assertFalse(r['coverage_complete'])
    def test_both_differences(self):
        r=run(fixture(extra=True,omit=True));self.assertEqual(r['A_minus_observed'][0]['start'],'16384');self.assertEqual(r['observed_minus_A'][0]['start'],'4096')
    def test_unflushed_replay(self):
        r=run(fixture(log=True));self.assertEqual(r['A_minus_observed'][0]['start'],'24576')
    def test_wrong_cut(self):
        for field,value in [('txg','31'),('root_bp',['0']*16)]:
            rows=fixture();rows[-3][field]=value
            with self.assertRaises(ValueError):run(rows)
        with self.assertRaises(ValueError):p.same_cut({'txg':'30'},{'txg':'31'})
    def test_nonzero_exit(self):
        with self.assertRaises(ValueError):run(fixture(),'1')
    def test_hash_mismatch(self):
        with self.assertRaises(ValueError):p.analyze(b'{}\n','0','0'*64)
    def test_missing_duplicate_envelope(self):
        rows=fixture();rows.insert(-1,copy.deepcopy(rows[-2]))
        with self.assertRaises(ValueError):run(rows)
    def test_wrong_totals(self):
        for key in ['records','intervals','bytes','map_words','map_bytes']:
            rows=fixture();rows[-2][key]='999'
            with self.assertRaises(ValueError):run(rows)
    def test_wrong_final_allocation(self):
        rows=fixture();next(r for r in rows if r['record']=='allocation')['start']='0'
        with self.assertRaises(ValueError):run(rows)
    def test_missing_base_map(self):
        rows=fixture(omit=True);rows=[r for r in rows if r['record']!='allocation_map']
        with self.assertRaises(ValueError):run(rows)
    def test_invalid_replay(self):
        for mutate in [lambda r:r.update(op='free'),lambda r:r.update(applied=False),lambda r:r.update(start='512'),lambda r:r.update(vdev_id='2'),lambda r:r.update(ordinal='2')]:
            rows=fixture();mutate(next(r for r in rows if r['record']=='allocation_op'))
            with self.assertRaises(ValueError):run(rows)
    def test_log_future_and_threshold(self):
        for txg in ['0','19','31']:
            rows=fixture(log=True)
            for r in rows:
                if r.get('source')=='log':r['log_txg']=txg
            with self.assertRaises(ValueError):run(rows)
    def test_checkpoint_and_removal(self):
        for k in ['checkpoint_unsupported','removal_unsupported']:
            rows=fixture();rows[4][k]=True
            with self.assertRaises(ValueError):run(rows)
    def test_overlap_accounting(self):
        with self.assertRaises(ValueError):p.apply([[0,10]],5,15,'alloc')
        with self.assertRaises(ValueError):p.apply([[0,10]],5,15,'free')
        self.assertEqual(p.apply([[0,10]],3,7,'free'),[[0,3],[7,10]])
    def test_allocated_raw_cannot_masquerade_as_hole_or_embedded(self):
        for kind in ('hole','embedded'):
            rows=fixture(omit=True);rows[3].update(classification=kind,dvas=[])
            with self.assertRaisesRegex(ValueError,'classification contradicts'):run(rows)
    def test_raw_classification_matrix(self):
        for bit,kind in [(39,'embedded'),(61,'protected_unsupported'),(62,'dedup_unsupported')]:
            rows=fixture();r=rows[3];r['bp_words'][6]=f'{int(r["bp_words"][6],16)|(1<<bit):016x}'
            with self.assertRaisesRegex(ValueError,'classification contradicts'):run(rows)
        rows=fixture();rows[3]['bp_words'][1]='8000000000000008'
        with self.assertRaisesRegex(ValueError,'classification contradicts'):run(rows)
        rows=fixture();r=rows[3];r['bp_words'][6]=f'{(int(r["bp_words"][6],16)&~(255<<48))|(127<<48):016x}'
        with self.assertRaisesRegex(ValueError,'classification contradicts'):run(rows)
        rows=fixture();rows[3]['bp_words'][0]=rows[3]['bp_words'][1]='0000000000000000'
        with self.assertRaisesRegex(ValueError,'classification contradicts'):run(rows)
    def test_valid_hole_and_embedded_raw(self):
        for embedded in (False,True):
            rows=fixture(omit=True);r=rows[3];r.update(dvas=[],classification='embedded' if embedded else 'hole')
            r['bp_words'][0]=r['bp_words'][1]='0000000000000000'
            if embedded:
                prop=(19<<48)|(1<<39)|(2<<32)|(7<<25)|1023
                r['bp_words'][6]=f'{prop:016x}';r.update(lsize='1024',psize='0',checksum_id=2,physical_birth='0',embedded_payload_bytes='8')
            result=run(rows);self.assertTrue(result['address_sets_equal']);self.assertFalse(result['coverage_complete'])
    def test_global_map_object_aliases(self):
        rows=fixture(log=True)
        for r in rows:
            if r.get('source')=='log':r['map_object']='50'
        with self.assertRaisesRegex(ValueError,'map object reused'):run(rows)
        rows=fixture();rows.insert(-4,dict(record='allocation_map',source='log',map_object='50',log_txg='20',length='0',shift=9,**F))
        with self.assertRaisesRegex(ValueError,'map object reused'):run(rows)
        rows=fixture(log=True);idx=next(i for i,r in enumerate(rows) if r['record']=='allocation')
        rows.insert(idx,dict(record='allocation_map',source='log',map_object='51',log_txg='21',length='0',shift=9,**F))
        with self.assertRaisesRegex(ValueError,'map object reused'):run(rows)
    def test_base_base_object_alias(self):
        rows=fixture();ms=copy.deepcopy(rows[5]);ms.update(id='1',start='1048576')
        rows[2].update(asize='2097152',metaslab_count='2')
        idx=next(i for i,r in enumerate(rows) if r['record']=='allocation')
        rows[idx:idx]=[ms,dict(record='allocation_map',source='base',map_object='50',log_txg='0',length='0',shift=12,**F)]
        with self.assertRaisesRegex(ValueError,'map object reused'):run(rows)
    def test_new_type_and_crypt_precedence(self):
        for typ in (54,127,138,255):
            rows=fixture();r=rows[3];prop=int(r['bp_words'][6],16);r['bp_words'][6]=f'{(prop&~(255<<48))|(typ<<48):016x}'
            with self.assertRaisesRegex(ValueError,'classification contradicts'):run(rows)
        rows=fixture(omit=True);r=rows[3];r.update(classification='hole',dvas=[]);r['bp_words'][0]=r['bp_words'][1]='0000000000000000';r['bp_words'][6]=f'{int(r["bp_words"][6],16)|(1<<61):016x}'
        with self.assertRaisesRegex(ValueError,'classification contradicts'):run(rows)
    def test_all_numeric_provenance_fields(self):
        for bad in ['not-a-guid','-1','01',str(2**64),True,1]:
            for field in ('guid','vdev_guid','objset','object','blkid','sequence','physical_birth','txg','map_object','log_txg','ordinal','id','metaslab'):
                rows=fixture(log=True)
                for r in rows:
                    if field in r:r[field]=bad
                    for d in r.get('dvas',[]):
                        if field in d:d[field]=bad
                with self.subTest(field=field,bad=bad):
                    with self.assertRaises(ValueError):run(rows)
        for bad in ['-0','00','-01',str(2**63),str(-2**63-1),True,0]:
            rows=fixture();rows[3]['level']=bad
            with self.assertRaises(ValueError):run(rows)
    def test_small_integer_and_boolean_types(self):
        for field in ('bp_type','bp_level','checksum_id','compression_id'):
            rows=fixture();rows[3][field]=True
            with self.assertRaises(ValueError):run(rows)
    def test_invalid_cut_schema(self):
        from paired_intervals import same_cut,cut
        rows=fixture();c=cut(rows[1],[rows[2]])
        for field in ('guid','txg'):
            bad=copy.deepcopy(c);bad[field]='not-an-integer'
            with self.assertRaises(ValueError):same_cut(bad,bad)
    def test_pointset_oracle(self):
        rng=random.Random(67)
        for _ in range(1000):
            a=[sorted(rng.sample(range(50),2)) for _ in range(rng.randrange(8))];b=[sorted(rng.sample(range(50),2)) for _ in range(rng.randrange(8))]
            points=lambda x:{v for s,e in x for v in range(s,e)}
            self.assertEqual(points(p.difference(a,b)),points(a)-points(b))
if __name__=='__main__':unittest.main(verbosity=2)
