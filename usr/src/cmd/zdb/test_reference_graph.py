"""Pool-free native parent fixtures, adversarial graph cases and CLI checks."""
import copy
import hashlib
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

from paired_intervals import canonical, digest
from parent_edges import analyze as parent_analyze
from test_edges import BUILD, ENV, full_fixture
import reference_graph as g


def encoded(rows):
    data = b''.join(canonical(r)+b'\n' for r in rows)
    return data, hashlib.sha256(data).hexdigest()


def context(rows):
    data, sha = encoded(rows)
    evidence = parent_analyze(data, '0', sha)
    datasets = {r['object']:r for r in rows if r['record']=='profile_dataset'}
    return evidence, datasets, sha, digest(evidence['cut'])


def selection(rows):
    edge = next(r for r in rows if r['record']=='parent_edge' and r['kind']=='indirect' and r['disposition']=='leaf')
    bp = [int(w,16) for w in edge['child_bp']]
    return dict(vdev_id='0',vdev_guid='7',offset=str(bp[1]*512),asize=str((bp[0]&0xffffff)*512),dva_slot='0')


def run(rows, target=None, **kwargs):
    data, sha = encoded(rows)
    identity = next(r for r in rows if r['record']=='pool')
    cut = dict(guid=identity['guid'],txg=identity['txg'],root_bp=identity['root_bp'],
               topology=sorted((r for r in rows if r['record']=='vdev'),key=lambda r:int(r['id'])))
    args=dict(status='0',expected_sha=sha,expected_cut_sha=digest(cut),target=target or selection(rows))
    args.update(kwargs)
    return g.analyze(data,**args)


class Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = pathlib.Path(tempfile.mkdtemp(prefix='reference-tests-',dir=BUILD))
        supplied=os.environ.get('REFERENCE_NATIVE_FIXTURE')
        path=pathlib.Path(supplied) if supplied else cls.tmp/'normal.jsonl'
        if not supplied:
            p=subprocess.run([str(BUILD/'test_edges'),'normal',str(path)],env=ENV,capture_output=True,text=True)
            if p.returncode:raise RuntimeError(p.stdout+p.stderr)
        cls.rows=full_fixture([json.loads(line) for line in path.read_text().splitlines()])

    def test_graph_and_aliases(self):
        r=run(self.rows)
        self.assertEqual(r['counts']['parent_observations'],8)
        self.assertEqual(r['counts']['parent_blocks'],5)
        self.assertEqual(r['counts']['dataset_roots'],2)
        self.assertEqual(r['counts']['blocks'],7)
        self.assertEqual(r['counts']['logical_slot_observations'],19)
        self.assertTrue(all(r[k] is False for k in g.SAFETY))
        seed=r['target']['block'];block=next(b for b in r['blocks'] if b['id']==seed)
        self.assertEqual(len(block['observation_nodes']),2)
        aliases=[o['bookmark'][0] for o in r['logical_slot_observations'] if o['node'] in block['observation_nodes']]
        self.assertEqual(aliases,['2','3'])
        slots=[s for s in r['slots'] if s['child_block']==seed]
        self.assertEqual(len(slots),1);self.assertEqual(len(slots[0]['observation_nodes']),2)
        self.assertEqual({a['dataset_object'] for a in r['observed_closure']['dataset_anchors']},{'2','3'})
        self.assertEqual(r['observed_closure']['publication_boundary']['kind'],'selected_import_uberblock')
        order=r['observed_closure']['structural_dependency_order'];positions={key:i for i,key in enumerate(order)}
        for slot in r['slots']:
            if slot['child_block'] in positions and slot['parent_block'] in positions:
                self.assertLess(positions[slot['child_block']],positions[slot['parent_block']])
        self.assertEqual(len(order),len(set(order)))
        self.assertEqual(r,run(self.rows))

    def test_terminals_and_distinct_slots(self):
        r=run(self.rows)
        holes=[s for s in r['slots'] if s['terminal'] is not None]
        self.assertGreater(len(holes),1);self.assertEqual(len({s['id'] for s in holes}),len(holes))
        # Root slots in the MOS stay distinct even though the two dataset roots share a block.
        roots=[s for s in r['slots'] if s['kind']=='dataset_bonus']
        self.assertEqual(len(roots),2);self.assertNotEqual(roots[0]['id'],roots[1]['id'])
        self.assertEqual(roots[0]['child_block'],roots[1]['child_block'])

    def test_conflicting_decoded_parent_bytes(self):
        rows=copy.deepcopy(self.rows)
        containers=[r for r in rows if r['record']=='parent_container']
        container=containers[5]
        data=bytearray.fromhex(container['decoded_hex']);data[1000]^=1;container['decoded_hex']=data.hex()
        # Unused objset padding still passes slot membership; canonicalization must refuse it.
        with self.assertRaisesRegex(ValueError,'conflicting decoded bytes'):run(rows)

    def test_incompatible_physical_extent_descriptors(self):
        evidence,datasets,sha,cutsha=context(self.rows);evidence=copy.deepcopy(evidence)
        ordinary=[r for r in evidence['parent_edges'] if r['record']=='parent_edge' and r['disposition']=='leaf']
        old=ordinary[0]['child_bp'];new=old.copy();new[12]='0000000000000001'
        ordinary[0]['child_bp']=new
        with self.assertRaisesRegex(ValueError,'overlapping physical extents'):
            g._build(evidence,datasets,sha,cutsha,selection(self.rows))

    def test_partial_overlap_and_duplicate_replica(self):
        evidence,datasets,sha,cutsha=context(self.rows)
        for mode in ('partial','duplicate'):
            bad=copy.deepcopy(evidence)
            leaf=next(r for r in bad['parent_edges'] if r['record']=='parent_edge' and r['disposition']=='leaf')
            if mode=='partial':leaf['child_bp'][1]=f'{int(leaf["child_bp"][1],16)-8:016x}'
            else:leaf['child_bp'][2:4]=leaf['child_bp'][:2]
            with self.subTest(mode=mode),self.assertRaisesRegex(ValueError,'overlapping physical extents'):
                g._build(bad,datasets,sha,cutsha,selection(self.rows))

    def test_cycles_and_diamond_dependencies(self):
        budget=g.Budget();blocks={key:{} for key in ('a','b','c','d')}
        slots={str(i):dict(id=str(i),parent_block=parent,child_block=child) for i,(parent,child) in enumerate([('b','a'),('c','a'),('d','b'),('d','c')])}
        parents,incoming,order=g._dependency_order(blocks,slots,budget)
        self.assertEqual(order,['a','b','c','d']);self.assertEqual(parents['a'],{'b','c'})
        slots['cycle']=dict(id='cycle',parent_block='a',child_block='d')
        with self.assertRaisesRegex(ValueError,'cycle'):g._dependency_order(blocks,slots,g.Budget())
        del blocks['a']
        with self.assertRaisesRegex(ValueError,'missing graph block'):g._dependency_order(blocks,slots,g.Budget())

    def test_invalid_inputs_and_missing_evidence(self):
        for kwargs in (dict(expected_sha='0'*64),dict(expected_cut_sha='0'*64),dict(status='1')):
            with self.subTest(kwargs=kwargs),self.assertRaises(ValueError):run(self.rows,**kwargs)
        for key,value in [('vdev_guid','99'),('vdev_id','99'),('offset','0'),('asize','1'),('dva_slot','2')]:
            target=selection(self.rows);target[key]=value
            with self.subTest(key=key),self.assertRaises(ValueError):run(self.rows,target)
        with self.assertRaisesRegex(ValueError,'target schema'):run(self.rows,{'vdev_id':'0'})
        for data in (b'{}\n',b'[]\n',b'{"schema":"zdb-parent-edges-v1"}\n'):
            with self.subTest(malformed=data),self.assertRaises(ValueError):
                g.analyze(data,'0',hashlib.sha256(data).hexdigest(),'0'*64,selection(self.rows))
        for kind in ('parent_edge','parent_container','verification'):
            rows=copy.deepcopy(self.rows);rows.pop(next(i for i,r in enumerate(rows) if r['record']==kind))
            with self.subTest(kind=kind),self.assertRaises(ValueError):run(rows)

    def test_replica_fidelity_and_root_target(self):
        bp=next(r['child_bp'] for r in self.rows if r['record']=='parent_edge' and r['disposition']=='leaf').copy()
        bp[2]=f'{(1<<32)|16:016x}';bp[3]='0000000000000020'
        topology={r['id']:r for r in self.rows if r['record']=='vdev'}
        replicas=g._replicas(bp,topology)
        self.assertEqual([r['slot'] for r in replicas],[0,1])
        self.assertEqual(replicas[1],dict(slot=1,vdev_id='1',vdev_guid='8',offset='16384',asize='8192'))
        bp[2]='0000000000000000'
        with self.assertRaisesRegex(ValueError,'absent DVA'):g._replicas(bp,topology)
        target=dict(vdev_id='0',vdev_guid='7',offset='65536',asize='8192',dva_slot='0')
        r=run(self.rows,target)
        self.assertEqual(r['observed_closure']['ancestor_blocks'],0)
        self.assertEqual(r['observed_closure']['blocks'],[r['selected_root']['block']])
        self.assertFalse(r['selected_root']['independent_label_election_verified'])

    def test_graph_budgets(self):
        for key in g.DEFAULT_LIMITS:
            with self.subTest(key=key),self.assertRaisesRegex(ValueError,'budget'):
                run(self.rows,limits={key:0})
        with self.assertRaisesRegex(ValueError,'budget'):run(self.rows,limits={'blocks':100001})

    def test_semantic_and_historical_obligations(self):
        r=run(self.rows)
        kinds={x['kind'] for x in r['unresolved_obligations']}
        self.assertIn('historical_roots_and_uberblocks',kinds)
        self.assertIn('non_tree_semantic_references',kinds)
        self.assertIsNone(r['observed_closure']['missing_global_reference_count'])
        opaque=g._semantic_inventory({'bpobj':{'bp_type':5,'container':False},'array':{'bp_type':195,'container':False},'file':{'bp_type':19,'container':False},'indirect':{'bp_type':5,'container':True}}, {'bpobj'},g.Budget())
        self.assertEqual({x['block'] for x in opaque},{'bpobj','array'})
        self.assertTrue(next(x for x in opaque if x['block']=='bpobj')['in_observed_closure'])
        self.assertFalse(next(x for x in opaque if x['block']=='array')['in_observed_closure'])
        self.assertTrue(all(x['ownership_status']=='unresolved' for x in opaque))

    def test_cli_exclusive_output(self):
        data,sha=encoded(self.rows);capture=self.tmp/'capture';status=self.tmp/'exit';output=self.tmp/'graph.json'
        capture.write_bytes(data);status.write_text('0\n');r=run(self.rows);target=selection(self.rows)
        cmd=[sys.executable,str(pathlib.Path(g.__file__)),str(capture),'--sha256',sha,'--cut-sha256',r['cut_sha256'],'--exit-status',str(status),'--output',str(output)]
        for key,value in target.items():cmd.extend(['--target-'+key.replace('_','-'),value])
        p=subprocess.run(cmd,capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stderr)
        saved=output.read_bytes();self.assertFalse(json.loads(saved)['write_schedule_available'])
        p=subprocess.run(cmd,capture_output=True);self.assertEqual(p.returncode,2);self.assertEqual(output.read_bytes(),saved)
        status.write_text('1\n');refused=self.tmp/'must-not-exist.json';cmd[cmd.index('--output')+1]=str(refused)
        self.assertEqual(subprocess.run(cmd,capture_output=True).returncode,2);self.assertFalse(refused.exists())
        self.assertEqual(output.read_bytes(),saved)


if __name__=='__main__':
    unittest.main(verbosity=2)
