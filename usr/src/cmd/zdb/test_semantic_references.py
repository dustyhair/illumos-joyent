"""Pool-free format, ownership, normalization and refusal tests."""
import copy
import hashlib
import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from paired_intervals import canonical
from parent_edges import bpwords, raw
from reference_graph import block_id, SAFETY
import semantic_references as s


def fletcher(data):
    sums = [0]*4
    for (v,) in struct.iter_unpack('<I', data):
        sums[0] = (sums[0]+v) & s.U64
        for i in range(1, 4):
            sums[i] = (sums[i]+sums[i-1]) & s.U64
    return sums


def bp(typ, offset, size=512, checksum=None):
    w = [0]*16; w[0] = size//512; w[1] = offset//512
    w[6] = (1 << 63) | (typ << 48) | (7 << 40) | (2 << 32) | ((size//512-1) << 16) | (size//512-1)
    w[10] = 306; w[11] = 1; w[12:] = checksum or [11, 12, 13, 14]
    return [f'{v:016x}' for v in w]


def compressed_zeros(data):
    """Fixture-only LZ4 encoder using repeated zero runs, with final literals."""
    out = bytearray(); anchor = 0; at = 1

    def extent(n):
        if n >= 15:
            n -= 15
            while n >= 255:
                out.append(255); n -= 255
            out.append(n)

    while at < len(data)-12:
        if data[at-1] != 0 or data[at:at+8] != b'\0'*8:
            at += 1; continue
        end = at+8
        while end < len(data)-5 and data[end] == 0:
            end += 1
        literal = data[anchor:at]; n = end-at-4
        out.append((min(len(literal), 15) << 4) | min(n, 15))
        extent(len(literal)); out.extend(literal); out.extend(b'\x01\x00'); extent(n)
        at = anchor = end
    literal = data[anchor:]; out.append(min(len(literal), 15) << 4)
    extent(len(literal)); out.extend(literal)
    return len(out).to_bytes(4, 'big')+out


def zap(entries):
    data = bytearray(512); struct.pack_into('<Q', data, 0, (1 << 63)+3)
    for i, (name, value) in enumerate(entries):
        off = 64*(i+1); struct.pack_into('<Q', data, off, value)
        text = name.encode('ascii'); data[off+14:off+14+len(text)] = text
    return bytes(data)


def embed(data, typ):
    payload = compressed_zeros(data); assert len(payload) <= 112
    w = [0]*16
    p = (1 << 63) | (typ << 48) | (1 << 39) | (15 << 32) | ((len(payload)-1) << 25) | (len(data)-1)
    positions = [i for i in range(16) if i not in (6, 10)]
    for i, pos in enumerate(positions):
        w[pos] = int.from_bytes(payload[i*8:(i+1)*8], 'little')
    w[6] = p; w[10] = 318
    return [f'{v:016x}' for v in w]


def dnode(typ, pointer, bonus_type=0, bonus=b'', blocksize=512, count=3):
    data = bytearray(512); data[:5] = bytes([typ, 17, 1, count, bonus_type])
    struct.pack_into('<H', data, 8, blocksize//512); struct.pack_into('<H', data, 10, len(bonus))
    data[64:192] = raw(pointer); at = 64+count*128; data[at:at+len(bonus)] = bonus
    assert len(data) == 512
    return data


def fixture():
    originals = [bp(19 if i == 0 else 10, 0x100000+i*512) for i in range(7)]
    data = b''.join(raw(list(s.normalize(w))) for w in originals).ljust(128 << 10, b'\0')
    ownerbp = bp(5, 0x400000, len(data), fletcher(data)); arraybp = bp(195, 0x800000, 128 << 10)
    unknownbp = bp(195, 0xc00000, 128 << 10)
    mos = bytearray(16 << 10); dsbonus = bytearray(320); struct.pack_into('<Q', dsbonus, 64, 2)
    objects = {
        1: dnode(196, ['0'*16]*16, 16, dsbonus, count=1),
        2: dnode(50, embed(zap([('1', 3)]), 50), 51, bytes(320), count=1),
        3: dnode(5, ownerbp, 6, struct.pack('<6Q', 7, 7*512, 7*512, 7*512, 0, 0), 128 << 10),
        4: dnode(196, embed(zap([(s.KEY, 5)]), 196)),
        5: dnode(195, arraybp, blocksize=128 << 10),
        6: dnode(195, unknownbp, blocksize=128 << 10)}
    for obj, d in objects.items():
        mos[obj*512:(obj+1)*512] = d
    records = [dict(record='parent_edge', node='1', child_bp=bp(10, 0x10000, len(mos)), bookmark=['0','0','0','0']),
               dict(record='parent_container', node='1', decoded_hex=mos.hex())]
    allbp = originals+[ownerbp, arraybp, unknownbp]
    blocks = [dict(id=block_id(w), bp_words=w, bp_type=int(w[6],16) >> 48 & 255, container=False,
                   replicas=[dict(slot=0,vdev_id='0',vdev_guid='7',offset=str(int(w[1],16)*512),
                                  asize=str((int(w[0],16)&0xffffff)*512))]) for w in allbp]
    graph = dict(input_sha256='a'*64, cut_sha256='b'*64, cut=dict(txg='330'), blocks=blocks,
                 target=dict(block=block_id(originals[0])), observed_closure=dict(blocks=[block_id(w) for w in originals]),
                 semantic_payloads=[dict(block=block_id(w), bp_type=int(w[6],16) >> 48 & 255,
                     classification='opaque', ownership_status='unresolved', in_observed_closure=False)
                     for w in [ownerbp, arraybp, unknownbp]],
                 unresolved_obligations=[dict(kind='historical_roots_and_uberblocks', status='unresolved')])
    evidence = dict(schema='semantic-leaf-evidence-v1', capture_sha256='a'*64, cut_sha256='b'*64,
                    leaves=[dict(bp_words=ownerbp, physical_hex=data.hex(),source_replica=dict(blocks[7]['replicas'][0]))])
    return graph, records, evidence


def build(graph, records, evidence, limits=None):
    budget = s.Budget(limits)
    payloads = s.leaf_payloads(evidence, graph, budget)
    return s._build(graph, records, payloads, budget)


def change(records, obj, offset, value):
    data = bytearray.fromhex(records[1]['decoded_hex'])
    data[obj*512+offset:obj*512+offset+len(value)] = value
    records[1]['decoded_hex'] = data.hex()


def repeat_mos(records, blkid):
    edge = copy.deepcopy(records[0]); edge['node'] = '2'; edge['bookmark'][3] = str(blkid)
    container = copy.deepcopy(records[1]); container['node'] = '2'
    records.extend([edge, container])


def public_mos_alias_fixture():
    """Complete synthetic stream accepted by the public parent/graph validators.

    One MOS buffer appears at logical indexes 0 and 1. Its dataset dnodes
    therefore represent objects 2/3 and 6/7 despite identical physical bytes.
    This tests capture-validation contracts, not actual disk authentication.
    """
    from parent_edges import SAFETY as parent_flags
    from test_edges import full_fixture

    def pointer(typ, offset, size):
        w=bp(typ,offset,size);w[0]='0000000000000010';w[10]='0000000000000014'
        return w

    root=pointer(11,65536,4096);mosbp=pointer(10,81920,2048);dsroot=pointer(11,131072,4096)
    rootdata=bytearray(4096);dn=dnode(10,mosbp,blocksize=2048,count=2)
    dn[192:320]=raw(mosbp);rootdata[:512]=dn
    emptyzap=embed(zap([]),50);emptyzap[10]='0000000000000014'
    mosdata=bytearray(2048);mosdata[512:1024]=dnode(50,emptyzap,51,bytes(320),count=1)
    for obj in (2,3):
        bonus=bytearray(320)
        for index,value in [(0,10),(2,25),(3,2 if obj==3 else 0),(5,1 if obj==3 else 0),(8,1)]:
            struct.pack_into('<Q',bonus,index*8,value)
        bonus[128:256]=raw(dsroot)
        mosdata[obj*512:(obj+1)*512]=dnode(16,['0'*16]*16,16,bonus,count=1)
    rows=[dict(record='parent_scope',encoding='decoded-amd64-little-endian',snapshot_birth_pruning=False,
        scope='current_MOS_and_dataset_tree_pointer_slots',semantic_BP_payloads_enumerated=False,
        historical_roots_enumerated=False,anchor='selected_import_uberblock_not_independent_label_proof',**parent_flags)]
    edges={}

    def edge(parent,kind,bm,bp,offset=0,base=0,slot=0,data=None):
        node=str(len(edges)+1);w=[int(v,16) for v in bp]
        disposition='container' if data is not None else 'embedded' if w[6]&(1<<39) else 'hole'
        r=dict(record='parent_edge',node=node,parent_node=parent,kind=kind,bookmark=bm,
            decoded_offset=str(offset),dnode_offset=str(base),slot=str(slot),child_bp=bp,
            parent_bp=edges[parent]['child_bp'] if parent!='0' else None,
            parent_bookmark=edges[parent]['bookmark'] if parent!='0' else None,
            disposition=disposition,**parent_flags)
        edges[node]=r;rows.append(r)
        if data is not None:
            rows.append(dict(record='parent_container',node=node,decoded_size=str(len(data)),
                authentication='original_bp_arc_checksum',decoded_hex=bytes(data).hex(),**parent_flags))
        return node

    edge('0','selected_uberblock',['0','0','-1','0'],root,data=rootdata)
    for blkid in (0,1):
        parent=edge('1','dnode',['0','0','0',str(blkid)],mosbp,64+128*blkid,slot=blkid,data=mosdata)
        first=4*blkid
        edge(parent,'dnode',['0',str(first+1),'0','0'],emptyzap,576,512)
        for obj in (first+2,first+3):
            base=(obj-first)*512
            edge(parent,'dnode',['0',str(obj),'0','0'],['0'*16]*16,base+64,base)
        for obj in (first+2,first+3):
            base=(obj-first)*512
            edge(parent,'dataset_bonus',[str(obj),'0','-1','0'],dsroot,base+320,data=bytes(4096))
    containers=[r for r in rows if r['record']=='parent_container'];decoded=sum(int(r['decoded_size']) for r in containers)
    rows.append(dict(record='parent_end',parent_capture_complete=True,error=0,nodes=str(len(edges)),
        containers=str(len(containers)),decoded_bytes=str(decoded),dataset_roots='4',
        output_charge=str(4096*len(edges)+2048*len(containers)+2*decoded),**parent_flags))
    full=full_fixture(rows);end=next(r for r in full if r['record']=='profile_end')
    additions=[]
    for old,new in [('2','6'),('3','7')]:
        for kind in ('profile_object','profile_dataset'):
            r=copy.deepcopy(next(r for r in full if r['record']==kind and r['object']==old))
            r['object']=new;additions.append(r)
    full[full.index(end):full.index(end)]=additions
    end.update(objects='4',datasets='4',heads='2',snapshots='2',active_object='7',reads='17',read_bytes='8704')
    return full


class Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.base = fixture()

    def test_seven_references_outside_owner_and_unknown_preservation(self):
        r = build(*copy.deepcopy(self.base))
        self.assertEqual(r['counts'],dict(semantic_references=7,decoded_bpobj_objects=1,
            contextual_TXG_arrays=1,target_matches=1,ancestor_matches=6,unmatched=0))
        self.assertEqual([v['decoded_byte_offset'] for v in r['semantic_references']], [str(i*128) for i in range(7)])
        self.assertTrue(all(not v['owner_in_observed_closure'] and not v['original_BP_identity'] for v in r['semantic_references']))
        self.assertTrue(all(v['candidate_count_among_captured_originals']==1 and v['historical_original_identity_unresolved']
                            and not v['stored_BP_payload_verifiable'] for v in r['semantic_references']))
        self.assertTrue(all(v['owner_chains'][0]['dataset_object']=='1' for v in r['semantic_references']))
        self.assertTrue(all(r[k] is False for k in SAFETY))
        self.assertFalse(r['semantic_enumeration_complete'])
        self.assertTrue(any(v['kind']=='opaque_semantic_payload' for v in r['unresolved_obligations']))
        c = r['unflushed_TXG_contexts'][0]
        self.assertFalse(c['payload_values_validated']); self.assertFalse(c['vdev_registration_independently_validated'])
        self.assertTrue(c['accounting_metadata_must_be_preserved'])
        self.assertEqual(r,build(*copy.deepcopy(self.base)))

    def test_missing_raw_and_physical_checksum_truncation_binding(self):
        for mode in ('missing','checksum','truncated','cut','descriptor','duplicate','replica'):
            g,r,e = copy.deepcopy(self.base)
            if mode=='missing':e['leaves']=[]
            elif mode=='checksum':e['leaves'][0]['physical_hex']='ff'+e['leaves'][0]['physical_hex'][2:]
            elif mode=='truncated':e['leaves'][0]['physical_hex']=e['leaves'][0]['physical_hex'][:-2]
            elif mode=='cut':e['cut_sha256']='0'*64
            elif mode=='descriptor':e['leaves'][0]['bp_words'][12]='0'*16
            elif mode=='replica':e['leaves'][0]['source_replica']['offset']='0'
            else:e['leaves']*=2
            with self.subTest(mode=mode), self.assertRaises(ValueError):build(g,r,e)

    def test_forged_key_does_not_classify_type195(self):
        g,r,e = copy.deepcopy(self.base)
        change(r,4,64,raw(embed(zap([(s.KEY+'x',5)]),196)))
        result=build(g,r,e)
        self.assertEqual(result['counts']['contextual_TXG_arrays'],0)
        self.assertEqual(sum(v['classification']=='opaque' for v in result['semantic_payloads']),2)

    def test_invalid_object_links_and_layouts(self):
        for obj,entries in [(4,[(s.KEY,999)]),(4,[(s.KEY,3)]),(4,[(s.KEY,4)]),(2,[('1',999)]),(2,[('1',2)]),(2,[('01',3)]),(2,[('xyz',3)]),(2,[('14b',3)])]:
            g,r,e=copy.deepcopy(self.base);typ=196 if obj==4 else 50
            change(r,obj,64,raw(embed(zap(entries),typ)))
            with self.subTest(entries=entries),self.assertRaises(ValueError):build(g,r,e)
        g,r,e=copy.deepcopy(self.base);change(r,5,0,bytes([19]))
        with self.assertRaisesRegex(ValueError,'target layout'):build(g,r,e)

    def test_header_counts_subobjects_and_unsupported_trees(self):
        for offset,value in [(10,struct.pack('<H',32)),(448,struct.pack('<Q',s.U64)),
                             (448,struct.pack('<Q',1025)),(480,struct.pack('<Q',9)),
                             (488,struct.pack('<Q',1)),(16,struct.pack('<Q',1)),(2,b'\x02')]:
            g,r,e=copy.deepcopy(self.base);change(r,3,offset,value)
            with self.subTest(offset=offset,value=value.hex()),self.assertRaises(ValueError):build(g,r,e)

    def test_microzap_adversarial_layouts(self):
        original=zap([('1',3)])
        for mode in ('short','header','norm','padding','name','duplicate','empty'):
            d=bytearray(original)
            if mode=='short':d=d[:-1]
            elif mode=='header':d[0]=0
            elif mode=='norm':d[16]=1
            elif mode=='padding':d[76]=1
            elif mode=='name':d[78:128]=b'x'*50
            elif mode=='duplicate':d[128:192]=d[64:128]
            else:d[128]=1
            with self.subTest(mode=mode),self.assertRaises(ValueError):s.microzap(bytes(d),s.Budget())

    def test_lz4_bounds_and_embedded_roundtrip(self):
        data=zap([(s.KEY,5)])
        self.assertEqual(s.embedded(embed(data,196),s.Budget()),data)
        for data,size in [(b'',512),(b'\0\0\0\x10x',512),
                          (b'\0\0\0\x03\x00\x00\x00',512),
                          (b'\0\0\0\x01\xf0',512),
                          (compressed_zeros(bytes(512)),511)]:
            with self.subTest(data=data[:10]),self.assertRaises(ValueError):s.lz4(data,size,s.Budget())

    def test_compressed_leaf_original_checksum_and_decoded_membership(self):
        for malformed in (False,True):
            g,r,e=copy.deepcopy(self.base)
            leaf=e['leaves'][0];plain=bytes.fromhex(leaf['physical_hex'])
            packed=compressed_zeros(plain).ljust(4096,b'\0')
            self.assertEqual(len(packed),4096)
            if malformed:packed=b'\xff'*4+packed[4:]
            old=block_id(leaf['bp_words']);w=[int(v,16) for v in leaf['bp_words']]
            w[6]=(w[6]&~((127<<32)|(65535<<16)))|(15<<32)|(7<<16)
            w[12:]=fletcher(packed);newbp=[f'{v:016x}' for v in w];new=block_id(newbp)
            block=next(b for b in g['blocks'] if b['id']==old);block.update(id=new,bp_words=newbp)
            next(b for b in g['semantic_payloads'] if b['block']==old)['block']=new
            change(r,3,64,raw(newbp));leaf.update(bp_words=newbp,physical_hex=packed.hex())
            if malformed:
                with self.assertRaisesRegex(ValueError,'LZ4 compressed length'):build(g,r,e)
            else:
                result=build(g,r,e);self.assertEqual(result['counts']['semantic_references'],7)
                self.assertEqual(result['decoded_bpobj_objects'][0]['decoded_sha256'],hashlib.sha256(plain).hexdigest())

    def test_empty_maps_and_noncanonical_empty_bpobj(self):
        g,r,e=copy.deepcopy(self.base);change(r,2,64,raw(embed(zap([]),50)));e['leaves']=[]
        self.assertEqual(build(g,r,e)['counts']['semantic_references'],0)
        g,r,e=copy.deepcopy(self.base);change(r,3,448,bytes(48));e['leaves']=[]
        with self.assertRaisesRegex(ValueError,'empty BPOBJ'):build(g,r,e)
        change(r,3,64,bytes(128))
        self.assertEqual(build(g,r,e)['counts']['decoded_bpobj_objects'],0)

    def test_duplicate_context_owner_and_unsupported_deadlist(self):
        g,r,e=copy.deepcopy(self.base)
        change(r,6,0,dnode(196,embed(zap([(s.KEY,5)]),196)))
        with self.assertRaisesRegex(ValueError,'ambiguous unflushed'):build(g,r,e)
        g,r,e=copy.deepcopy(self.base);change(r,2,64,raw(bp(50,0xe00000)))
        with self.assertRaisesRegex(ValueError,'deadlist map'):build(g,r,e)

    def test_normalization_dedup_embedded_and_ambiguous_matches(self):
        original=bp(19,512); stored=list(s.normalize(original))
        self.assertEqual(stored[11:16],['0'*16]*5)
        self.assertEqual(s.resolve(stored,{tuple(stored):['one']}),'one')
        with self.assertRaisesRegex(ValueError,'ambiguous'):s.resolve(stored,{tuple(stored):['one','two']})
        with self.assertRaisesRegex(ValueError,'normalization'):s.resolve(original,{})
        dedup=original.copy();dedup[6]=f'{int(dedup[6],16)|(1<<62):016x}'
        self.assertEqual(s.normalize(dedup)[12:],tuple(dedup[12:]))
        embedded=embed(zap([]),196);normalized=s.normalize(embedded)
        self.assertTrue(all(v=='0'*16 for i,v in enumerate(normalized) if i not in (6,10)))
        self.assertIsNone(s.resolve(list(normalized),{}))
        with self.assertRaisesRegex(ValueError,'hole'):s.resolve(['0'*16]*16,{})

    def test_unmatched_reference_obligation(self):
        g,r,e=copy.deepcopy(self.base)
        g['blocks']=g['blocks'][1:]
        result=build(g,r,e)
        self.assertEqual(result['counts']['unmatched'],1)
        self.assertIsNone(result['semantic_references'][0]['relationship'])
        self.assertTrue(any(v['kind']=='unmatched_stored_BP' for v in result['unresolved_obligations']))

    def test_logical_mos_alias_preserves_dataset_owner_chains(self):
        g,r,e=copy.deepcopy(self.base)
        # Isolate dataset ownership: the contextual ZAP alias is tested below.
        change(r,4,0,bytes(512));repeat_mos(r,1)
        result=build(g,r,e)
        self.assertEqual(result['counts']['semantic_references'],7)
        self.assertTrue(all({c['dataset_object'] for c in ref['owner_chains']}=={'1','33'}
                            for ref in result['semantic_references']))
        # Identical observations at the same logical index may be deduplicated.
        g,r,e=copy.deepcopy(self.base);repeat_mos(r,0)
        actual=build(g,r,e);original=build(*copy.deepcopy(self.base))
        self.assertEqual(actual['semantic_references'],original['semantic_references'])
        self.assertEqual(actual['counts'],original['counts'])
        self.assertEqual(actual['budget']['used']['objects'],original['budget']['used']['objects'])
        # Conflicting observations at that logical index must refuse.
        r[-1]['decoded_hex']='ff'+r[-1]['decoded_hex'][2:]
        with self.assertRaisesRegex(ValueError,'conflicting logical MOS'):build(g,r,e)

    def test_logical_mos_alias_exposes_duplicate_contextual_owner(self):
        g,r,e=copy.deepcopy(self.base);repeat_mos(r,1)
        self.assertEqual({v['object'] for v in s.mos_objects(r,s.Budget()).values() if v['type']==196 and not v['bonus']},
                         {4,36})
        with self.assertRaisesRegex(ValueError,'ambiguous unflushed-TXG owners'):build(g,r,e)

    def test_public_validated_capture_preserves_all_logical_dataset_dnodes(self):
        from parent_edges import analyze as parent_analyze
        from reference_graph import analyze as graph_analyze
        from paired_intervals import digest
        rows=public_mos_alias_fixture();data=b''.join(canonical(r)+b'\n' for r in rows)
        sha=hashlib.sha256(data).hexdigest();parent=parent_analyze(data,'0',sha)
        roots={r['bookmark'][0] for r in parent['parent_edges'] if r['record']=='parent_edge' and r['kind']=='dataset_bonus'}
        self.assertEqual(roots,{'2','3','6','7'})
        objects=s.mos_objects(parent['parent_edges'],s.Budget())
        self.assertEqual({str(k) for k,v in objects.items() if v['bonus_type']==16},roots)
        target=dict(vdev_id='0',vdev_guid='7',offset='131072',asize='8192',dva_slot='0')
        cutsha=digest(parent['cut']);graph=graph_analyze(data,'0',sha,cutsha,target)
        self.assertEqual({r['dataset_object'] for r in graph['dataset_anchors']},roots)
        leaf=canonical(dict(schema='semantic-leaf-evidence-v1',capture_sha256=sha,cut_sha256=cutsha,leaves=[]))
        result=s.analyze(data,'0',sha,cutsha,target,leaf,hashlib.sha256(leaf).hexdigest())
        self.assertEqual(result['counts']['semantic_references'],0)
        self.assertTrue(all(result[k] is False for k in SAFETY))

    def test_birth_bucket_and_future_birth(self):
        for name in ('132','14a'):
            g,r,e=copy.deepcopy(self.base);change(r,2,64,raw(embed(zap([(name,3)]),50)))
            with self.subTest(name=name),self.assertRaisesRegex(ValueError,'birth outside'):build(g,r,e)

    def test_budget_refusals(self):
        for key in s.DEFAULT_LIMITS:
            with self.subTest(key=key),self.assertRaisesRegex(ValueError,'budget'):build(*copy.deepcopy(self.base),limits={key:0})
        with self.assertRaises(ValueError):s.Budget({'work':2000001})

    def test_public_raw_capture_boundary_and_evidence_hash(self):
        g,r,e=copy.deepcopy(self.base);data=b''.join(canonical(v)+b'\n' for v in r);leaf=canonical(e);sha=hashlib.sha256(leaf).hexdigest()
        # Public entry always calls the graph's full raw-capture validation.
        with self.assertRaises(ValueError):s.analyze(data,'0','a'*64,'b'*64,{},leaf,sha)
        with patch.object(s,'graph_analyze',return_value=g) as check:
            result=s.analyze(data,'0','a'*64,'b'*64,{},leaf,sha)
            check.assert_called_once_with(data,'0','a'*64,'b'*64,{})
            self.assertEqual(result['counts']['semantic_references'],7)
        with patch.object(s,'graph_analyze',return_value=g),self.assertRaises(ValueError):
            s.analyze(data,'0','a'*64,'b'*64,{},leaf,'0'*64)
        with patch.object(s,'graph_analyze',return_value=g),self.assertRaises(ValueError):
            s.analyze(data,'0','a'*64,'b'*64,{},None,None)

    def test_cli_refusal_does_not_create_or_replace_output(self):
        with tempfile.TemporaryDirectory() as name:
            d=pathlib.Path(name);(d/'capture').write_bytes(b'{}\n');(d/'exit').write_text('0');(d/'leaf').write_bytes(b'{}')
            cmd=[sys.executable,str(pathlib.Path(s.__file__)),str(d/'capture'),'--sha256','0'*64,
                 '--cut-sha256','0'*64,'--leaf-sha256','0'*64,'--exit-status',str(d/'exit'),
                 '--leaf-evidence',str(d/'leaf'),'--output',str(d/'out')]
            for key in ('vdev-id','vdev-guid','offset','asize','dva-slot'):cmd.extend(['--target-'+key,'0'])
            self.assertEqual(subprocess.run(cmd,capture_output=True).returncode,2);self.assertFalse((d/'out').exists())
            (d/'out').write_text('preserve')
            self.assertEqual(subprocess.run(cmd,capture_output=True).returncode,2);self.assertEqual((d/'out').read_text(),'preserve')


if __name__=='__main__':
    unittest.main(verbosity=2)
