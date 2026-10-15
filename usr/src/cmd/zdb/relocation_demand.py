#!/usr/bin/env python3
"""Offline, single-destination base demand only. Never produces placement."""
import argparse
import collections
import json
import os
import pathlib
import stat
import sys
from observed_intervals import FLAGS, U64, parse, require, uint
from paired_intervals import digest
from profile_intervals import analyze as profile

LIMIT = 128 << 20
SAFETY = dict(FLAGS, pointer_rewrite_ready=False, placement_performed=False,
              movable_candidate=False, capacity_proven=False)


def checked(n):
    require(type(n) is int and 0 <= n <= U64, 'uint64 arithmetic overflow')
    return n


def raidz_asize(p, ashift, children, parity):
    """Pinned vdev_raidz_asize(), using physical compressed payload bytes."""
    require(all(type(x) is int for x in (p, ashift, children, parity)), 'integer geometry required')
    require(0 < p <= 16 << 20 and 9 <= ashift <= 16 and 0 < parity <= 3 and parity < children <= 255,
            'unsupported RAID-Z dimensions/payload')
    sector = 1 << ashift
    sectors = (p + sector - 1) // sector
    sectors += parity * ((sectors + children - parity - 1) // (children - parity))
    return checked(((sectors + parity) // (parity + 1)) * (parity + 1) * sector)


def analyze(data, exit_status, expected_sha, expected_cut_sha, source_id,
            source_guid, destination_id, destination_guid):
    require(len(data) <= LIMIT, 'capture byte budget')
    result = profile(data, exit_status, expected_sha)
    require(digest(result['cut']) == expected_cut_sha, 'expected cut mismatch')
    rows = [parse(line) for line in data.splitlines()]
    return _single(result, rows, expected_sha, expected_cut_sha, source_id,
                   source_guid, destination_id, destination_guid)


def _single(result, rows, expected_sha, expected_cut_sha, source_id,
            source_guid, destination_id, destination_guid):
    for x in (source_id, source_guid, destination_id, destination_guid):
        uint(x)
    require(source_id != destination_id and source_guid != destination_guid, 'distinct destination required')
    topology = result['cut']['topology']
    require(len({v['id'] for v in topology}) == len(topology) == len({v['guid'] for v in topology}), 'ambiguous topology identity')
    def select(vid, guid):
        matches = [v for v in topology if v['id'] == vid and v['guid'] == guid]
        require(len(matches) == 1, 'vdev ID/GUID mismatch')
        v = matches[0]
        require(v['type'] == 'raidz' and v['allocation_class'] == 'normal' and v['alloc_bias'] == 0 and not v['is_log'], 'unsupported vdev class')
        raidz_asize(512, uint(v['ashift']), uint(v['children']), uint(v['parity']))
        return v
    source = select(source_id, source_guid)
    destination = select(destination_id, destination_guid)
    bps = {r['sequence']: r for r in rows if r['record'] == 'bp'}
    checks = {r['sequence']: r for r in result['verification_results']}
    grouped = collections.defaultdict(list)
    for ref in result['observed_interval_references']:
        if ref['vdev_id'] == source_id:
            require(ref['vdev_guid'] == source_guid, 'source binding')
            grouped[(uint(ref['start']), uint(ref['end']))].append(ref)
    extents = []
    conflicts = []
    source_bytes = destination_bytes = reference_bytes = 0
    previous_end = 0
    replica_extents = collections.defaultdict(set)
    for (start, end), refs in sorted(grouped.items()):
        require(start >= previous_end and end > start, 'overlapping source extents')
        previous_end = end
        signatures = {(tuple(r['bp_words']), r['P'], r['Q'], r['physical_birth'], r['metadata']) for r in refs}
        require(len(signatures) == 1, 'incompatible physical extent descriptors')
        replica_extents[tuple(refs[0]['bp_words'])].add((start, end))
        first = refs[0]
        payload = uint(first['P'])
        demand = raidz_asize(payload, uint(destination['ashift']), uint(destination['children']), uint(destination['parity']))
        references = []
        for ref in sorted(refs, key=lambda r: (uint(r['sequence']), r['slot'])):
            bp = bps[ref['sequence']]
            verification = checks[ref['sequence']]
            require(verification['status'] == 'verified', 'source reference unverified')
            others = [d for d in bp['dvas'] if d['slot'] != ref['slot']]
            if any(d['vdev_id'] == destination_id for d in others):
                conflicts.append(dict(sequence=ref['sequence'], slot=ref['slot'], reason='destination_already_holds_BP_replica'))
            references.append(dict(interval_reference=ref, original_bp_record=bp, verification=verification))
        source_bytes = checked(source_bytes + end - start)
        destination_bytes = checked(destination_bytes + demand)
        reference_bytes = checked(reference_bytes + (end - start) * len(refs))
        extents.append(dict(source_start=str(start), source_length=str(end-start), physical_payload_bytes=str(payload),
                            destination_asize=str(demand), observed_reference_count=str(len(refs)), references=references, **SAFETY))
    require(source_bytes == result['per_vdev'][source_id]['allocation_bytes'], 'selected source allocation coverage mismatch')
    # Gross accounting over actual captured metaslabs; never a placement/fit proof.
    ms = [r for r in rows if r['record'] == 'metaslab' and r['vdev_id'] == destination_id]
    gross = checked(sum(uint(r['length']) for r in ms))
    allocated = result['per_vdev'][destination_id]['allocation_bytes']
    free = checked(gross - allocated)
    reasons = []
    if not extents: reasons.append('source_has_no_allocated_extents')
    coupled = [dict(bp_words=list(bp), source_extents=[dict(start=str(a), length=str(b-a)) for a,b in sorted(spans)])
               for bp,spans in sorted(replica_extents.items()) if len(spans)>1]
    if coupled: reasons.append('planned_replica_colocation')
    if conflicts: reasons.append('destination_replica_conflict')
    if destination_bytes > free: reasons.append('base_demand_exceeds_captured_gross_free_bytes')
    if any(uint(e['destination_asize']) > uint(destination['asize']) for e in extents):
        reasons.append('individual_extent_exceeds_destination_asize')
    return dict(schema='offline-relocation-demand-v1', input_sha256=expected_sha,
                cut=result['cut'], cut_sha256=expected_cut_sha, source=source, destination=destination,
                demand_calculation_complete=True, single_destination_contract_satisfied=not reasons,
                refusal_reasons=reasons, replica_conflicts=conflicts, coupled_source_replicas=coupled, extents=extents,
                physical_extent_count=str(len(extents)), observed_reference_count=str(sum(len(x) for x in grouped.values())),
                source_unique_bytes=str(source_bytes), source_reference_bytes=str(reference_bytes),
                destination_base_demand_bytes=str(destination_bytes), destination_captured_gross_free_bytes=str(free),
                exclusions=dict(hole_and_embedded_BP_references=str(sum(r['classification'] in ('hole','embedded') for r in bps.values())),
                                unallocated_origin_heads=str(result['profile_unallocated_origin_heads'])),
                limits=['Base demand only: no ancestor rewrite, transaction, reservation or fragmentation allowance.',
                        'Observed reference multiplicity is not an exhaustive reverse-reference map; traversal is birth-pruned.',
                        'Original-BP verification proves logical recovery, not independent readability of each DVA.',
                        'No destination offsets, allocation, pointer rewriting, publication or live-state exclusion.',
                        'Input hashes bind recorded evidence, not independent execution attestation.'], **SAFETY)


def assign_groups(items, capacities):
    """Deterministic greedy accounting only; failure is not infeasibility proof."""
    used = {key: 0 for key in capacities}
    assignments = []
    unresolved = []
    ordered = sorted(items, key=lambda x: (len(x['eligible_groups']),
                     -max((uint(g['destination_asize']) for g in x['eligible_groups']), default=0),
                     uint(x['source_start'])))
    for item in ordered:
        fit = [g for g in item['eligible_groups']
               if checked(used[g['id']] + uint(g['destination_asize'])) <= capacities[g['id']]]
        if not fit:
            unresolved.append(dict(source_start=item['source_start'], source_length=item['source_length'],
                                   reason='greedy_remaining_gross_bound_exhausted'))
            continue
        group = min(fit, key=lambda g: (used[g['id']] + uint(g['destination_asize']), uint(g['id'])))
        used[group['id']] = checked(used[group['id']] + uint(group['destination_asize']))
        assignments.append(dict(source_start=item['source_start'], source_length=item['source_length'],
                                destination_id=group['id'], destination_guid=group['guid'],
                                destination_asize=group['destination_asize'], **SAFETY))
    return assignments, used, unresolved


def analyze_groups(data, exit_status, expected_sha, expected_cut_sha, source_id, source_guid):
    require(len(data) <= LIMIT, 'capture byte budget')
    result = profile(data, exit_status, expected_sha)
    require(digest(result['cut']) == expected_cut_sha, 'expected cut mismatch')
    uint(source_id);uint(source_guid)
    topology = result['cut']['topology']
    rows = [parse(line) for line in data.splitlines()]
    candidates = [v for v in topology if v['id'] != source_id and v['type']=='raidz'
                  and v['allocation_class']=='normal' and v['alloc_bias']==0 and not v['is_log']]
    require(candidates, 'no other normal RAID-Z destination groups')
    candidates.sort(key=lambda v: uint(v['id']))
    reports = [_single(result, rows, expected_sha, expected_cut_sha, source_id, source_guid,
                       v['id'], v['guid']) for v in candidates]
    first = reports[0]
    capacities = {r['destination']['id']: uint(r['destination_captured_gross_free_bytes']) for r in reports}
    by_group = {r['destination']['id']: {(e['source_start'],e['source_length']): e for e in r['extents']} for r in reports}
    items = []
    for extent in first['extents']:
        forbidden = set()
        for ref in extent['references']:
            slot = ref['interval_reference']['slot']
            forbidden.update(d['vdev_id'] for d in ref['original_bp_record']['dvas'] if d['slot'] != slot)
        eligible = [];excluded = []
        for v in candidates:
            demand = by_group[v['id']][(extent['source_start'],extent['source_length'])]['destination_asize']
            entry = dict(id=v['id'],guid=v['guid'],destination_asize=demand)
            if v['id'] in forbidden:
                excluded.append(dict(entry,reason='existing_BP_replica'))
            elif uint(demand)>uint(v['asize']):
                excluded.append(dict(entry,reason='extent_exceeds_group_asize'))
            else:eligible.append(entry)
        # Preserve the physical extent and every observed reference, not a representative only.
        item = {k:v for k,v in extent.items() if k!='destination_asize'}
        item.update(eligible_groups=eligible,excluded_groups=excluded);items.append(item)
    reasons = []
    if not items:reasons.append('source_has_no_allocated_extents')
    if first['coupled_source_replicas']:reasons.append('coupled_source_replicas_unsupported')
    if any(not e['eligible_groups'] for e in items):reasons.append('no_eligible_destination_group')
    assignments=[];used={key:0 for key in capacities};unresolved=[]
    if not reasons:assignments,used,unresolved=assign_groups(items,capacities)
    if unresolved:reasons.append('greedy_capacity_assignment_unresolved')
    return dict(schema='offline-relocation-group-demand-v1',input_sha256=expected_sha,
                cut=result['cut'],cut_sha256=expected_cut_sha,source=first['source'],
                candidate_groups=candidates,
                excluded_topology_groups=[dict(id=v['id'],guid=v['guid'],reason='source_group' if v['id']==source_id else 'unsupported_group_class') for v in topology if v not in candidates],
                extents=items,physical_extent_count=first['physical_extent_count'],
                observed_reference_count=first['observed_reference_count'],source_unique_bytes=first['source_unique_bytes'],
                source_reference_bytes=first['source_reference_bytes'],coupled_source_replicas=first['coupled_source_replicas'],
                eligibility_complete=True,all_extents_have_eligible_group=bool(items) and all(e['eligible_groups'] for e in items),
                group_assignment_complete=bool(items) and not reasons,refusal_or_unresolved_reasons=reasons,
                assignments=assignments,unresolved_extents=unresolved,
                per_destination=[dict(id=v['id'],guid=v['guid'],assigned_base_bytes=str(used[v['id']]),
                                      captured_gross_free_bytes=str(capacities[v['id']]),
                                      remaining_gross_bytes=str(capacities[v['id']]-used[v['id']])) for v in candidates],
                exclusions=first['exclusions'],limits=first['limits']+[
                    'Group assignment is only deterministic gross-byte accounting, not physical placement.',
                    'Greedy failure is unresolved, not proof that no alternative assignment exists.',
                    'Coupled distinct source replicas are conservatively unsupported.'],**SAFETY)


def read_regular(path, limit):
    # Reject special files and symlinks before opening; recheck the opened handle.
    require(stat.S_ISREG(path.lstat().st_mode), 'regular input file required')
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(fd, 'rb') as f:
        require(stat.S_ISREG(os.fstat(f.fileno()).st_mode), 'regular input file required')
        data = f.read(limit + 1)
    require(len(data) <= limit, 'input byte budget')
    return data


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('capture', type=pathlib.Path)
    for name in ('sha256', 'cut-sha256', 'source-id', 'source-guid'):
        p.add_argument('--'+name, required=True)
    mode=p.add_mutually_exclusive_group(required=True)
    mode.add_argument('--destination-id')
    mode.add_argument('--all-destinations', action='store_true')
    p.add_argument('--destination-guid')
    p.add_argument('--exit-status', type=pathlib.Path, required=True)
    p.add_argument('--output', type=pathlib.Path, required=True)
    a = p.parse_args()
    try:
        require((a.destination_guid is None) if a.all_destinations else (a.destination_guid is not None), 'destination GUID required only in single mode')
        args=(read_regular(a.capture, LIMIT), read_regular(a.exit_status, 64).decode('ascii'),
              a.sha256, a.cut_sha256, a.source_id, a.source_guid)
        result = analyze_groups(*args) if a.all_destinations else analyze(*args,a.destination_id,a.destination_guid)
        with a.output.open('x') as f:
            json.dump(result, f, sort_keys=True, indent=2)
            f.write('\n')
    except (ValueError, OSError) as e:
        print('refused: '+str(e), file=sys.stderr)
        return 2
    return 0 if result['group_assignment_complete' if a.all_destinations else 'single_destination_contract_satisfied'] else 3


if __name__ == '__main__':
    sys.exit(main())
