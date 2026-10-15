#!/usr/bin/env python3
"""Head-only symbolic COW allocation/retention ledger; never an image writer."""
import argparse
from bisect import bisect_right
import copy
import hashlib
import pathlib
import sys

from observed_intervals import parse, require, uint, words, U64
from paired_intervals import canonical, digest
from reference_graph import analyze as graph_analyze, block_id, SAFETY as GRAPH_SAFETY
import semantic_references as semantic
from relocation_demand import read_regular, raidz_asize, checked

SAFETY = dict(GRAPH_SAFETY, movable_candidate=False, accounting_proven=False,
              retention_proven=False, publication_ready=False,
              physical_placement_checked=False, payload_copy_performed=False,
              transaction_accounting_complete=False)
DEFAULT_LIMITS = dict(work=2000000, route=64, allocations=100000,
                      replicas=4096, output=64 << 20)
FRESH = 'fresh-symbolic-only-never-a-vdev-address'
GLOBAL_OBLIGATIONS = (
    'pool_allocation_deflation_and_space_counters',
    'MOS_retention_deferred_free_and_space_counters',
    'selected_dataset_referenced_unique_and_directory_counters',
    'snapshot_clone_lifetimes_and_deadlist_counters',
    'stock_COW_deadlist_BPOBJ_counter_and_MOS_expansion',
    'metaslab_log_spacemap_and_TXG_bookkeeping',
    'replica_separation_reservation_and_capacity',
    'target_payload_acquisition_and_copy_verification',
    'encoded_metadata_checksums_and_atomic_publication')
BLOCK_OBLIGATIONS = ('retained_old_block_ownership', 'new_BP_births', 'replacement_counter_deltas')


class Budget:
    def __init__(self, limits=None):
        self.limits = dict(DEFAULT_LIMITS)
        for k, v in (limits or {}).items():
            require(k in self.limits and type(v) is int and 0 <= v <= self.limits[k], 'invalid ledger budget')
            self.limits[k] = v
        self.used = dict.fromkeys(self.limits, 0)

    def take(self, **counts):
        for k, n in counts.items():
            require(0 <= n <= self.limits[k]-self.used[k], 'ledger budget: '+k)
        for k, n in counts.items():
            self.used[k] += n


def _route(graph, edges, selection, budget):
    require(type(selection) is dict and set(selection) == {'dataset_object','object','blkid'}, 'head selection schema')
    for value in selection.values():
        uint(value)
    anchors = {a['dataset_object']: a for a in graph['dataset_anchors']}
    ds = selection['dataset_object']
    require(ds in anchors and anchors[ds]['snapshot'] is False, 'selected dataset is not a captured head')
    bookmark = [ds, selection['object'], '0', selection['blkid']]
    starts = [r for r in edges.values() if r['bookmark'] == bookmark and r['disposition'] == 'leaf'
              and block_id(r['child_bp']) == graph['target']['block']]
    require(len(starts) == 1, 'selected head target observation missing/ambiguous')
    require((int(starts[0]['child_bp'][6],16) >> 48) & 255 == 19, 'initial ledger requires plain file-data leaf')
    route = []; seen = set(); current = starts[0]
    while True:
        budget.take(work=1, route=1)
        require(current['node'] not in seen, 'logical route cycle')
        seen.add(current['node']); route.append(current)
        if current['parent_node'] == '0':
            require(current['node'] == graph['selected_root']['observation_node'], 'wrong publication boundary')
            break
        require(current['parent_node'] in edges, 'missing logical route parent')
        current = edges[current['parent_node']]
    dsroots = [r for r in route if r['kind'] == 'dataset_bonus']
    require(len(dsroots) == 1 and dsroots[0]['bookmark'][0] == ds, 'route crosses another dataset anchor')
    require(all(r['bookmark'][0] in (ds, '0') for r in route), 'route updates another dataset')
    return route, anchors


def _context(graph, semantics, records, payloads, selection, leaf_sha, budget):
    edges = {r['node']:r for r in records if r['record'] == 'parent_edge'}
    blocks = {r['id']:r for r in graph['blocks']}
    route, anchors = _route(graph, edges, selection, budget)
    selected_nodes = {r['node'] for r in route}
    selected_blocks = {block_id(r['child_bp']) for r in route}
    topology = {v['id']:v for v in graph['cut']['topology']}
    candidates = [v for v in graph['cut']['topology'] if v['id'] != graph['target']['vdev_id']
                  and v['type'] == 'raidz' and v['allocation_class'] == 'normal'
                  and not v['is_log'] and v['alloc_bias'] == 0]
    candidates.sort(key=lambda v:uint(v['id']))
    require(candidates, 'no modeled non-source normal RAID-Z geometry')
    observations = {r['node']:r for r in graph['logical_slot_observations']}
    replacements = []; fresh = []; obligations = []; replacement_ids = {}; cursor = 0
    profiles = {r['object']:r for r in records if r['record'] == 'profile_dataset'}
    require(selection['dataset_object'] in profiles, 'missing selected dataset counter provenance')
    previous_snapshot=uint(profiles[selection['dataset_object']]['prev_snap_txg'])
    for row in route:
        budget.take(work=1)
        original = block_id(row['child_bp'])
        if original in replacement_ids:
            continue
        block = blocks[original]; w = words(block['bp_words']); p = w[6]
        lsize = ((p & 65535)+1)*512; old_p = (((p >> 16) & 65535)+1)*512
        rid = 'cow:'+digest([graph['input_sha256'],selection,original])
        replacement_ids[original] = rid
        is_target = original == graph['target']['block']
        require(is_target or block['container'], 'unsupported non-container route ancestor')
        modeled_p = old_p if is_target else lsize
        cases = []
        for v in candidates:
            budget.take(work=1)
            ashift=uint(v['ashift']); sector=1 << ashift
            cases.append(dict(vdev_id=v['id'], vdev_guid=v['guid'], ashift=v['ashift'],
                children=v['children'], parity=v['parity'],
                qsize=str(checked(((modeled_p+sector-1)//sector)*sector)),
                asize=str(raidz_asize(modeled_p,ashift,uint(v['children']),uint(v['parity'])))))
        require(0 < len(block['replicas']) <= len(candidates), 'insufficient distinct modeled replica groups')
        length = max(uint(c['asize']) for c in cases)
        replicas = []
        for replica in block['replicas']:
            budget.take(replicas=1, allocations=1, output=1024)
            token = rid+'/replica/'+str(replica['slot'])
            replicas.append(dict(allocation_id=token, original_DVA_slot=replica['slot']))
            end = checked(cursor+length)
            fresh.append(dict(id=token, address_space=FRESH, begin=str(cursor), end=str(end),
                              length_bound=str(length), replacement=rid, replica_slot=replica['slot'],
                              destination_vdev=None, physical_offset=None))
            cursor = end
        incoming = []
        for node in block['observation_nodes']:
            budget.take(work=1, output=1024)
            require(node in edges, 'missing original logical reference')
            e=edges[node]
            incoming.append(dict(node=node, bookmark=e['bookmark'], parent_node=e['parent_node'],
                canonical_slot=observations[node]['slot'] if node in observations else None,
                selected_for_new_copy=node in selected_nodes,
                unselected_child_BP_value_preserved=node not in selected_nodes))
        old_births = dict(logical=str(w[10]), physical=str(w[9] or w[10]))
        kill_path = ('MOS_free_path' if row['bookmark'][0] == '0' else
                     'dataset_deadlist_path' if w[10] <= previous_snapshot else 'dataset_free_path')
        replacements.append(dict(id=rid, original_block=original, original_bp_words=block['bp_words'],
            original_replicas=block['replicas'], lsize=str(lsize), original_psize=str(old_p),
            original_compression_id=(p >> 32) & 127,
            modeled_compression=dict(policy='preserve_original_encoded_bytes' if is_target else 'OFF_size_assumption_only',
                                     compression_id=(p >> 32) & 127 if is_target else 2,
                                     modeled_psize=str(modeled_p), encoded_payload_available=False),
            old_births=old_births, new_logical_birth=None, new_physical_birth=None,
            ordinary_stock_COW_kill_path_if_replaced=kill_path,
            stock_kill_side_effects_simulated=False,
            modeled_replica_count=len(replicas), replicas=replicas, geometry_cases=cases,
            distinct_destination_groups_required=True, destination_assignment=None,
            original_logical_references=incoming))
        for kind in BLOCK_OBLIGATIONS:
            obligations.append(dict(id=kind+':'+rid, kind=kind, replacement=rid,
                status='unresolved', before_births=old_births if kind == 'new_BP_births' else None,
                after=None, delta=None))
    updates = []
    for row in route:
        budget.take(work=1, output=1536)
        original = block_id(row['child_bp']); parent = row['parent_node']
        updates.append(dict(observation_node=row['node'], bookmark=row['bookmark'], kind=row['kind'],
            decoded_offset=row['decoded_offset'], dnode_offset=row['dnode_offset'], slot=row['slot'],
            canonical_slot=observations[row['node']]['slot'] if row['node'] in observations else None,
            original_child=original, replacement_child=replacement_ids[original],
            original_parent=block_id(row['parent_bp']) if parent != '0' else None,
            replacement_parent=replacement_ids[block_id(row['parent_bp'])] if parent != '0' else None,
            publication_boundary_intent_only=parent == '0'))
    before = []
    for row in records:
        budget.take(work=1)
        if row['record'] != 'allocation':
            continue
        budget.take(allocations=1, output=1024)
        start=uint(row['start']); end=checked(start+uint(row['length']))
        require(row['vdev_id'] in topology and topology[row['vdev_id']]['guid'] == row['vdev_guid'], 'allocation topology binding')
        require(end > start and end <= uint(topology[row['vdev_id']]['asize']), 'retained allocation bounds')
        before.append(dict(id='old:'+digest([row['vdev_guid'],row['start'],row['length']]),
            address_space='source-vdev:'+row['vdev_guid'], vdev_id=row['vdev_id'], vdev_guid=row['vdev_guid'],
            begin=str(start), end=str(end), length=row['length']))
    require(before, 'missing captured allocated set')
    before.sort(key=lambda r:(uint(r['vdev_id']),uint(r['begin'])))
    coverage={}
    for row in before:
        budget.take(work=1)
        spans=coverage.setdefault(row['vdev_id'],[]); start=uint(row['begin']);end=uint(row['end'])
        require(not spans or start >= spans[-1][1], 'overlapping retained source allocations')
        if spans and start == spans[-1][1]:spans[-1][1]=end
        else:spans.append([start,end])
    starts={vid:[s for s,e in spans] for vid,spans in coverage.items()}
    for replacement in replacements:
        for replica in replacement['original_replicas']:
            budget.take(work=1)
            vid=replica['vdev_id'];start=uint(replica['offset']);end=checked(start+uint(replica['asize']))
            i=bisect_right(starts.get(vid,[]),start)-1
            require(i >= 0 and end <= coverage[vid][i][1], 'old route DVA is not retained in captured allocated set')
    frozen = []
    for obj in semantics['decoded_bpobj_objects']:
        budget.take(work=1)
        key=obj['block']; require(key in payloads, 'missing preserved deadlist payload')
        data=payloads[key]['data']; budget.take(output=4096+4*len(data))
        entries=[copy.deepcopy(r) for r in semantics['semantic_references'] if r['owner_block'] == key]
        for ref in entries:
            ref['matched_candidate_in_selected_route']=ref['matched_block'] in selected_blocks
        frozen.append(dict(object=obj['object'], block=key, header_u64=obj['header_u64'],
            role='captured_before_image_with_hypothetical_fixed_model_totals',
            actual_stock_after_header=None, actual_stock_new_entry_count=None,
            header_hex=b''.join(uint(v).to_bytes(8,'little') for v in obj['header_u64']).hex(),
            decoded_hex=data.hex(), decoded_sha256=obj['decoded_sha256'],
            physical_sha256=obj['physical_sha256'], stored_references=entries,
            identity_policy='preserve_old_bytes_and_allocations; never infer historical original identity'))
    roots=[]
    for ds,a in sorted(anchors.items(),key=lambda kv:uint(kv[0])):
        if ds != selection['dataset_object']:
            budget.take(output=1024)
            roots.append(dict(dataset_object=ds, snapshot=a['snapshot'], original_root_block=a['root_block'],
                              after_root_block=a['root_block'], canonical_slot=a['slot']))
    profile=profiles[selection['dataset_object']]
    for kind in GLOBAL_OBLIGATIONS:
        obligations.append(dict(id=kind, kind=kind, replacement=None, status='unresolved',
            before=dict(space_bytes=dict(zip(('referenced','compressed','uncompressed','unique'),profile['space_bytes'])),
                        prev_snap_txg=profile['prev_snap_txg'])
                if kind == 'selected_dataset_referenced_unique_and_directory_counters' else None,
            after=None, delta=None))
    budget.take(output=2048*len(replacements)+1024*len(obligations))
    plan=dict(schema='head-only-symbolic-COW-plan-v1', capture_sha256=graph['input_sha256'],
        cut_sha256=graph['cut_sha256'], leaf_evidence_sha256=leaf_sha, selection=selection,
        target=graph['target'], route_observation_nodes=[r['node'] for r in route],
        model_scope='hypothetical_no_free_selected_reference_route_only',
        full_stock_transaction_replacement_count=None,
        replacements=replacements, reference_updates=updates, preserved_dataset_roots=roots,
        preserved_deadlists=frozen, semantic_rewrites=[],
        allocation_before=before, new_allocations=fresh,
        allocation_after=[dict(r) for r in before+fresh],
        retained_allocation_ids=[r['id'] for r in before], freed_allocations=[], reused_allocations=[],
        source_vdev_removed=False, obligations=obligations,
        source_vdev_excluded_from_modeled_destinations=graph['target']['vdev_id'])
    return dict(expected=plan, semantic_unresolved=semantics['unresolved_obligations'],
                structural_closure=graph['observed_closure'], graph_counts=graph['counts'])


def draft(context):
    return copy.deepcopy(context['expected'])


def validate_plan(context, plan, budget=None):
    """Validate an untrusted symbolic proposal against revalidated source facts."""
    budget=budget or Budget(); expected=context['expected']
    require(type(plan) is dict and set(plan) == set(expected), 'ledger plan schema')
    require(plan['semantic_rewrites'] == [], 'semantic rewrite relying on lossy identity is unsupported')
    require(plan['freed_allocations'] == [] and plan['reused_allocations'] == [] and
            plan['source_vdev_removed'] is False, 'free/reuse/removal of retained allocations refused')
    require(plan['preserved_deadlists'] == expected['preserved_deadlists'], 'modified deadlist bytes/header/references')
    require(plan['reference_updates'] == expected['reference_updates'] and
            plan['preserved_dataset_roots'] == expected['preserved_dataset_roots'], 'accidental alias or root update')
    require(type(plan['obligations']) is list and all(type(r) is dict and 'id' in r for r in plan['obligations']), 'counter obligation schema')
    require(len({r['id'] for r in plan['obligations']}) == len(plan['obligations']) and
            {r['id'] for r in plan['obligations']} == {r['id'] for r in expected['obligations']}, 'missing/duplicate counter or retention obligations')
    require(plan['obligations'] == expected['obligations'], 'invented counter/birth/retention resolution')
    variable={'new_allocations','allocation_after'}
    for key in set(expected)-variable:
        require(canonical(plan[key]) == canonical(expected[key]), 'changed ledger source fact/policy: '+key)
    require(type(plan['new_allocations']) is list and len(plan['new_allocations']) == len(expected['new_allocations']), 'replacement allocation count')
    wanted={r['id']:r for r in expected['new_allocations']}; actual={}; spans=[]
    for row in plan['new_allocations']:
        budget.take(work=1)
        require(type(row) is dict and set(row) == set(expected['new_allocations'][0]), 'symbolic allocation schema')
        require(row['id'] in wanted and row['id'] not in actual, 'duplicate/unknown replacement allocation')
        require(row['address_space'] == FRESH, 'fresh symbolic space required; retained allocation reuse refused')
        want=wanted[row['id']]
        require(canonical({k:row[k] for k in want if k not in ('begin','end')}) ==
                canonical({k:want[k] for k in want if k not in ('begin','end')}), 'changed replica demand or physical placement')
        start=uint(row['begin']); end=uint(row['end'])
        require(end > start and end-start == uint(want['length_bound']), 'symbolic replacement interval length')
        actual[row['id']]=row; spans.append((start,end))
    previous=0
    for start,end in sorted(spans):
        budget.take(work=1)
        require(start >= previous, 'overlapping symbolic replacements')
        previous=end
    union={r['id']:r for r in plan['allocation_before']}; require(len(union) == len(plan['allocation_before']), 'duplicate retained allocation')
    require(not set(union)&set(actual), 'reused retained allocation identity')
    union.update(actual)
    require(type(plan['allocation_after']) is list and all(type(r) is dict and 'id' in r for r in plan['allocation_after']), 'after-allocation schema')
    after={r['id']:r for r in plan['allocation_after']}
    require(len(after) == len(plan['allocation_after']) and canonical(after) == canonical(union),
            'allocated_after must equal allocated_before union new_allocations')
    return dict(selected_route_replacement_blocks=len(plan['replacements']), changed_logical_references=len(plan['reference_updates']),
        full_stock_transaction_replacement_count=None,
        stock_dataset_deadlist_path_blocks=sum(r['ordinary_stock_COW_kill_path_if_replaced']=='dataset_deadlist_path' for r in plan['replacements']),
        stock_dataset_free_path_blocks=sum(r['ordinary_stock_COW_kill_path_if_replaced']=='dataset_free_path' for r in plan['replacements']),
        stock_MOS_free_path_blocks=sum(r['ordinary_stock_COW_kill_path_if_replaced']=='MOS_free_path' for r in plan['replacements']),
        preserved_dataset_roots=len(plan['preserved_dataset_roots']), preserved_deadlist_objects=len(plan['preserved_deadlists']),
        preserved_stored_references=sum(len(r['stored_references']) for r in plan['preserved_deadlists']),
        original_allocation_intervals=len(plan['allocation_before']), symbolic_new_replica_allocations=len(actual),
        retained_source_allocated_bytes=str(checked(sum(uint(r['length']) for r in plan['allocation_before']))),
        symbolic_fresh_bytes_bound=str(checked(sum(uint(r['length_bound']) for r in actual.values()))),
        tagged_union_bytes_bound=str(checked(sum(uint(r['length']) for r in plan['allocation_before'])+
                                            sum(uint(r['length_bound']) for r in actual.values()))),
        unresolved_counter_birth_retention_obligations=len(plan['obligations']))


def _result(context, plan, budget):
    counts=validate_plan(context,plan,budget)
    result=dict(schema='head-only-symbolic-COW-ledger-v1', plan=plan, counts=counts,
        plan_canonical_sha256=digest(plan),
        symbolic_invariants_validated=True, physical_allocation_performed=False,
        full_stock_transaction_enumerated=False, stock_kill_side_effects_simulated=False,
        structural_global_closure=context['structural_closure'], graph_counts=context['graph_counts'],
        semantic_unresolved_obligations=context['semantic_unresolved'],
        budget=dict(limits=budget.limits,used=budget.used),
        limits=['The allocated-set union is over tagged model spaces, not an updated pool allocation map.',
                'Selected route replacements are not the full stock transaction: deadlisting, frees and counters may dirty additional BPOBJ/deadlist/MOS branches.',
                'Fixed deadlist totals and no frees are hypothetical model constraints; actual stock COW may append descriptors and change headers while preserving existing entries.',
                'Fresh byte coordinates index only the symbolic arena; destination vdevs and physical offsets remain null.',
                'Metadata OFF sizes and preserved data PSIZE are explicit assumptions, not encoded output or allocator reservations.',
                'Each new BP preserves observed replica count; geometry cases do not assign distinct destination groups or prove capacity.',
                'All old allocations and stored deadlist bytes stay intact; stock-consistent retention/accounting, births and publication remain unresolved.',
                'Selected logical references alone change; other aliases retain original child BP values.',
                'Lossy stored BP matches never authorize semantic rewriting, freeing or historical identity claims.'], **SAFETY)
    require(len(canonical(result))+1 <= budget.limits['output'], 'serialized ledger output budget')
    return result


def analyze(data,status,sha,cut_sha,target,selection,leaf_data,leaf_sha,
            proposal_data=None,proposal_sha=None,limits=None):
    # One complete raw graph/profile/original-verification validation, followed
    # by the same bounded semantic helpers used by its public analyzer.
    graph=graph_analyze(data,status,sha,cut_sha,target)
    require(type(leaf_data) is bytes and len(leaf_data) <= semantic.EVIDENCE_LIMIT and
            hashlib.sha256(leaf_data).hexdigest() == leaf_sha, 'missing/mismatched ledger leaf evidence')
    budget=Budget(limits)
    try:
        sb=semantic.Budget();payloads=semantic.leaf_payloads(parse(leaf_data),graph,sb)
        records=[parse(line) for line in data.splitlines()]
        semantics=semantic._build(graph,records,payloads,sb)
        context=_context(graph,semantics,records,payloads,selection,leaf_sha,budget)
        if proposal_data is None:
            require(proposal_sha is None, 'proposal hash without proposal')
            plan=draft(context)
        else:
            require(type(proposal_data) is bytes and len(proposal_data) <= 64 << 20 and
                    hashlib.sha256(proposal_data).hexdigest() == proposal_sha, 'proposal hash/byte budget')
            plan=parse(proposal_data)
        return _result(context,plan,budget)
    except (KeyError,IndexError,TypeError,AttributeError) as error:
        raise ValueError('malformed ledger evidence/proposal') from error


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('capture',type=pathlib.Path)
    for name in ('sha256','cut-sha256','target-vdev-id','target-vdev-guid','target-offset','target-asize',
                 'target-dva-slot','dataset-object','object','blkid','leaf-sha256'):
        p.add_argument('--'+name,required=True)
    for name in ('exit-status','leaf-evidence','output'):
        p.add_argument('--'+name,type=pathlib.Path,required=True)
    p.add_argument('--proposal',type=pathlib.Path);p.add_argument('--proposal-sha256')
    a=p.parse_args()
    target=dict(vdev_id=a.target_vdev_id,vdev_guid=a.target_vdev_guid,offset=a.target_offset,
                asize=a.target_asize,dva_slot=a.target_dva_slot)
    selection=dict(dataset_object=a.dataset_object,object=a.object,blkid=a.blkid)
    try:
        proposal=read_regular(a.proposal,64 << 20) if a.proposal else None
        result=analyze(read_regular(a.capture,512 << 20),read_regular(a.exit_status,64).decode('ascii'),
            a.sha256,a.cut_sha256,target,selection,read_regular(a.leaf_evidence,semantic.EVIDENCE_LIMIT),
            a.leaf_sha256,proposal,a.proposal_sha256)
        with a.output.open('xb') as f:f.write(canonical(result)+b'\n')
    except (ValueError,OSError) as error:
        print('refused: '+str(error),file=sys.stderr);return 2
    return 0


if __name__=='__main__':sys.exit(main())
