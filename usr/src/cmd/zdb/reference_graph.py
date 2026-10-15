#!/usr/bin/env python3
"""Offline observed reference graph and ancestor dependencies; no write schedule."""
import argparse
import collections
import hashlib
import heapq
import json
import pathlib
import sys

from observed_intervals import parse, require, uint
from paired_intervals import canonical, digest
from parent_edges import analyze as validate_parent_capture, SAFETY as PARENT_SAFETY
from relocation_demand import read_regular

INPUT_LIMIT = 512 << 20
SAFETY = dict(PARENT_SAFETY, capacity_proven=False, ancestor_rewrite_ready=False,
              write_schedule_available=False)
DEFAULT_LIMITS = dict(blocks=100000, slots=100000, observations=100000,
                      work=2000000, output=256 << 20)
SEMANTIC_TYPES = {5: 'bpobj_entries_opaque', 9: 'intent_log_payload_opaque',
                  50: 'deadlist_object_links_opaque', 51: 'deadlist_header_opaque',
                  53: 'bpobj_subobject_links_opaque',
                  195: 'generic_uint64_metadata_unclassified'}


class Budget:
    def __init__(self, limits=None):
        self.limits = dict(DEFAULT_LIMITS)
        if limits:
            require(set(limits) <= set(self.limits), 'unknown graph budget')
            for key, value in limits.items():
                require(type(value) is int and 0 <= value <= self.limits[key], 'invalid graph budget')
                self.limits[key] = value
        self.used = {key: 0 for key in self.limits}

    def take(self, **counts):
        for key, value in counts.items():
            require(value <= self.limits[key] - self.used[key], 'graph budget: '+key)
        for key, value in counts.items():
            self.used[key] += value


def block_id(bp):
    return 'bp:'+digest(bp)


def _replicas(bp, topology):
    w = [int(x, 16) for x in bp]
    replicas = []
    for slot in range(3):
        a, b = w[2*slot:2*slot+2]
        size = (a & 0xffffff)*512
        if not size:
            require(a == b == 0, 'noncanonical absent DVA')
            continue
        vid = str(a >> 32)
        require(vid in topology and not b >> 63, 'unknown/gang DVA')
        v = topology[vid]
        start = b*512
        require(start <= (1 << 64)-1-size and start+size <= uint(v['asize']), 'DVA bounds')
        replicas.append(dict(slot=slot, vdev_id=vid, vdev_guid=v['guid'],
                             offset=str(start), asize=str(size)))
    require(replicas, 'allocated BP has no DVA')
    return replicas


def _dependency_order(blocks, slots, budget):
    children = {key: set() for key in blocks}
    parents = {key: set() for key in blocks}
    incoming_slots = collections.defaultdict(list)
    for slot in slots.values():
        budget.take(work=1)
        child = slot['child_block']
        if child is None:
            continue
        parent = slot['parent_block']
        require(parent in blocks and child in blocks, 'missing graph block')
        children[parent].add(child)
        parents[child].add(parent)
        incoming_slots[child].append(slot['id'])
    remaining = {key: len(value) for key, value in children.items()}
    ready = sorted(key for key, count in remaining.items() if count == 0)
    heapq.heapify(ready)
    order = []
    while ready:
        budget.take(work=1)
        child = heapq.heappop(ready)
        order.append(child)
        for parent in sorted(parents[child]):
            budget.take(work=1)
            remaining[parent] -= 1
            if remaining[parent] == 0:
                heapq.heappush(ready, parent)
    require(len(order) == len(blocks), 'cycle after physical-block canonicalization')
    return parents, incoming_slots, order


def _semantic_inventory(blocks, closure, budget):
    result = []
    for key, block in sorted(blocks.items()):
        budget.take(work=1)
        if not block['container'] and block['bp_type'] in SEMANTIC_TYPES:
            budget.take(output=512)
            result.append(dict(block=key, bp_type=block['bp_type'],
                               classification=SEMANTIC_TYPES[block['bp_type']],
                               in_observed_closure=key in closure, ownership_status='unresolved'))
    return result


def _build(evidence, dataset_records, expected_sha, expected_cut_sha,
           target, limits=None):
    """Internal construction; public analyze() always revalidates raw capture."""
    budget = Budget(limits)
    cut = evidence['cut']
    budget.take(output=8192+len(canonical(cut)))
    topology = {v['id']: v for v in cut['topology']}
    require(len(topology) == len(cut['topology']), 'ambiguous topology')
    blocks = {}; edges = {}; payloads = {}; slots = {}; observations = []
    selected = None
    rows = evidence['parent_edges']
    # First pass establishes all allocated descriptors and exact parent bytes.
    for row in rows:
        budget.take(work=1)
        if row['record'] == 'parent_edge':
            require(row['node'] not in edges, 'duplicate observation node')
            edges[row['node']] = row
            if row['disposition'] not in ('container', 'leaf'):
                continue
            bp = row['child_bp']; key = block_id(bp)
            if key not in blocks:
                budget.take(blocks=1, output=2048)
                prop = int(bp[6], 16)
                blocks[key] = dict(id=key, bp_words=bp, bp_type=(prop >> 48) & 255,
                                   bp_level=(prop >> 56) & 31,
                                   physical_payload_bytes=str((((prop >> 16) & 65535)+1)*512),
                                   replicas=_replicas(bp, topology),
                                   container=row['disposition'] == 'container',
                                   decoded_sha256=None, decoded_size=None,
                                   observation_nodes=[])
            block = blocks[key]
            require(block['bp_words'] == bp and block['container'] == (row['disposition'] == 'container'),
                    'incompatible BP identity/disposition')
            budget.take(output=32)
            block['observation_nodes'].append(row['node'])
        elif row['record'] == 'parent_container':
            require(row['node'] in edges, 'container without observation')
            key = block_id(edges[row['node']]['child_bp'])
            require(key in blocks, 'missing parent block')
            text = row['decoded_hex']
            if key in payloads:
                require(payloads[key] == text, 'same parent BP has conflicting decoded bytes')
            else:
                payloads[key] = text
                blocks[key]['decoded_sha256'] = hashlib.sha256(bytes.fromhex(text)).hexdigest()
                blocks[key]['decoded_size'] = row['decoded_size']
    # Reject all overlapping distinct physical allocations, even if descriptors
    # differ only in another replica. Whole-BP compatibility never implies this.
    intervals = collections.defaultdict(list)
    for key, block in blocks.items():
        budget.take(work=1)
        require(not block['container'] or key in payloads, 'missing decoded parent bytes')
        for replica in block['replicas']:
            budget.take(work=1)
            start = uint(replica['offset']); end = start+uint(replica['asize'])
            intervals[replica['vdev_id']].append((start, end, key, replica['slot']))
    for spans in intervals.values():
        previous = None
        for span in sorted(spans):
            budget.take(work=1)
            require(previous is None or span[0] >= previous[1],
                    'conflicting/overlapping physical extents or duplicate DVA replicas')
            previous = span
    roots = []
    for row in rows:
        if row['record'] != 'parent_edge':
            continue
        budget.take(work=1)
        child = block_id(row['child_bp']) if row['disposition'] in ('container', 'leaf') else None
        if row['parent_node'] == '0':
            require(selected is None and row['kind'] == 'selected_uberblock' and child in blocks,
                    'selected-root anchor')
            selected = dict(kind='selected_import_uberblock', block=child,
                            observation_node=row['node'], bookmark=row['bookmark'],
                            independent_label_election_verified=False)
            continue
        require(row['parent_node'] in edges, 'missing parent observation')
        parent = block_id(row['parent_bp'])
        require(parent in payloads and edges[row['parent_node']]['child_bp'] == row['parent_bp'],
                'missing/incompatible authenticated parent')
        offset = uint(row['decoded_offset'])
        data = payloads[parent]
        child_hex = ''.join(int(x, 16).to_bytes(8, 'little').hex() for x in row['child_bp'])
        require(data[offset*2:(offset+128)*2] == child_hex, 'slot byte membership')
        key = 'slot:'+digest([parent, row['decoded_offset']])
        description = dict(id=key, parent_block=parent, decoded_offset=row['decoded_offset'],
                           kind=row['kind'], dnode_offset=row['dnode_offset'], slot=row['slot'],
                           child_block=child,
                           terminal=None if child is not None else dict(disposition=row['disposition'], bp_words=row['child_bp']))
        if key not in slots:
            budget.take(slots=1, output=768)
            slots[key] = dict(description, observation_nodes=[])
        else:
            require(all(slots[key][k] == v for k, v in description.items()),
                    'conflicting child/shape in one canonical parent slot')
        budget.take(observations=1, output=1024)
        slots[key]['observation_nodes'].append(row['node'])
        observations.append(dict(node=row['node'], slot=key, parent_node=row['parent_node'],
                                 bookmark=row['bookmark'], parent_bookmark=row['parent_bookmark']))
        if row['kind'] == 'dataset_bonus':
            obj = row['bookmark'][0]
            require(obj in dataset_records, 'dataset anchor missing profile context')
            ds = dataset_records[obj]
            budget.take(output=1024)
            roots.append(dict(dataset_object=obj, slot=key, root_block=child,
                              disposition=row['disposition'], snapshot=ds['snapshot'],
                              dir_object=ds['dir_object'], previous_snapshot=ds['prev_snap_object'],
                              relationship_limit='previous-snapshot fields do not prove clone origin'))
    require(selected is not None and len(roots) == len(dataset_records), 'incomplete root anchors')
    # Aliases are counted as observations; no observation ID is a physical ID.
    require(len(observations) == evidence['nodes']-1, 'lost logical aliases')
    require(len({r['node'] for r in observations}) == len(observations), 'duplicate logical observation')
    for v in target.values():
        uint(v)
    require(set(target) == {'vdev_id','vdev_guid','offset','asize','dva_slot'}, 'target schema')
    require(uint(target['asize']) > 0 and uint(target['dva_slot']) < 3, 'target extent/slot')
    vid = target['vdev_id']
    require(vid in topology and topology[vid]['guid'] == target['vdev_guid'], 'target ID/GUID mismatch')
    budget.take(work=sum(len(b['replicas']) for b in blocks.values()))
    matches = [(key, replica) for key, b in blocks.items() for replica in b['replicas']
               if all(str(replica[k]) == target[k] for k in ('vdev_id','vdev_guid','offset','asize'))
               and str(replica['slot']) == target['dva_slot']]
    require(len(matches) == 1, 'target DVA is missing or ambiguous')
    seed, replica = matches[0]
    parents, incoming, order = _dependency_order(blocks, slots, budget)
    closure = set(); todo = [seed]
    while todo:
        budget.take(work=1)
        node = todo.pop()
        if node in closure:
            continue
        budget.take(output=512)
        closure.add(node)
        todo.extend(sorted(parents[node]-closure, reverse=True))
    frontiers = sorted(node for node in closure if not parents[node])
    require(frontiers == [selected['block']], 'observed closure has a missing parent/root boundary')
    budget.take(work=len(closure)+sum(len(incoming[node]) for node in closure))
    closure_slots = sorted(slot for node in closure for slot in incoming[node]
                           if slots[slot]['parent_block'] in closure)
    semantic = _semantic_inventory(blocks, closure, budget)
    obligations = [
        dict(kind='non_tree_semantic_references', status='unresolved', blocks=[x['block'] for x in semantic],
             limit='typed inventory only; no absence or exhaustive-owner inference'),
        dict(kind='other_semantic_fields_and_payloads', status='unresolved',
             limit='tree-slot capture does not enumerate all BP-bearing semantics'),
        dict(kind='historical_roots_and_uberblocks', status='unresolved'),
        dict(kind='independent_label_election', status='unresolved'),
        dict(kind='allocation_COW_accounting_publication_and_crash_recovery', status='unresolved'),
        dict(kind='stock_reader_and_update_compatibility', status='unresolved')]
    budget.take(output=4096+256*len(semantic))
    result = dict(schema='observed-reference-closure-v1', input_sha256=expected_sha,
                  cut=cut, cut_sha256=expected_cut_sha, graph_validated=True,
                  counts=dict(blocks=len(blocks), parent_blocks=len(payloads),
                              parent_observations=sum(r['record']=='parent_container' for r in rows),
                              slots=len(slots), logical_slot_observations=len(observations),
                              aliased_slots=sum(len(s['observation_nodes'])>1 for s in slots.values()),
                              dataset_roots=len(roots)),
                  blocks=[blocks[k] for k in sorted(blocks)],
                  slots=[slots[k] for k in sorted(slots)],
                  logical_slot_observations=sorted(observations,key=lambda r:uint(r['node'])),
                  selected_root=selected, dataset_anchors=sorted(roots,key=lambda r:uint(r['dataset_object'])),
                  semantic_payloads=semantic, unresolved_obligations=obligations,
                  target=dict(target, block=seed),
                  observed_closure=dict(blocks=sorted(closure), slots=closure_slots,
                      structural_dependency_order=[key for key in order if key in closure],
                      ancestor_blocks=len(closure)-1, selected_root_reached=True,
                      dataset_anchors=[r for r in sorted(roots,key=lambda r:uint(r['dataset_object']))
                                       if r['root_block'] in closure],
                      publication_boundary=selected, observed_missing_parent_frontiers=[],
                      missing_global_reference_count=None),
                  budget=dict(limits=budget.limits, used=budget.used),
                  limits=['Observed current tree slots only; a closed graph does not prove no external references.',
                          'Whole-BP nodes retain all DVA replicas; decoded slots are not disk byte addresses.',
                          'Dependency order is structural and supplies no writes, destination offsets or reservations.',
                          'Authentication relies on revalidated native evidence; independent runtime review is external.'],
                  **SAFETY)
    require(len(canonical(result))+1 <= budget.limits['output'], 'serialized graph output budget')
    return result


def analyze(data, status, expected_sha, expected_cut_sha, target, limits=None):
    require(type(data) is bytes and len(data) <= INPUT_LIMIT, 'graph capture byte budget')
    require(type(target) is dict and set(target) == {'vdev_id','vdev_guid','offset','asize','dva_slot'}, 'target schema')
    for value in target.values():
        uint(value)
    try:
        evidence = validate_parent_capture(data, status, expected_sha)
    except (KeyError, IndexError, TypeError) as error:
        raise ValueError('malformed parent evidence') from error
    require(digest(evidence['cut']) == expected_cut_sha, 'expected graph cut mismatch')
    datasets = {}
    for line in data.splitlines():
        record = parse(line)
        if record['record'] == 'profile_dataset':
            require(record['object'] not in datasets, 'duplicate profile dataset')
            datasets[record['object']] = record
    return _build(evidence, datasets, expected_sha, expected_cut_sha, target, limits)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('capture', type=pathlib.Path)
    for name in ('sha256','cut-sha256','target-vdev-id','target-vdev-guid','target-offset','target-asize','target-dva-slot'):
        p.add_argument('--'+name, required=True)
    p.add_argument('--exit-status', required=True, type=pathlib.Path)
    p.add_argument('--output', required=True, type=pathlib.Path)
    a = p.parse_args()
    target = dict(vdev_id=a.target_vdev_id, vdev_guid=a.target_vdev_guid,
                  offset=a.target_offset, asize=a.target_asize, dva_slot=a.target_dva_slot)
    try:
        result = analyze(read_regular(a.capture, INPUT_LIMIT), read_regular(a.exit_status, 64).decode('ascii'),
                         a.sha256, a.cut_sha256, target)
        payload = canonical(result)+b'\n'
        require(len(payload) <= result['budget']['limits']['output'], 'serialized graph output budget')
        with a.output.open('xb') as f:
            f.write(payload)
    except (ValueError, OSError) as error:
        print('refused: '+str(error), file=sys.stderr); return 2
    return 0


if __name__ == '__main__':
    sys.exit(main())
