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
    rows = [parse(line) for line in data.splitlines()]
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
    for (start, end), refs in sorted(grouped.items()):
        require(start >= previous_end and end > start, 'overlapping source extents')
        previous_end = end
        signatures = {(tuple(r['bp_words']), r['P'], r['Q'], r['physical_birth'], r['metadata']) for r in refs}
        require(len(signatures) == 1, 'incompatible physical extent descriptors')
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
    if conflicts: reasons.append('destination_replica_conflict')
    if destination_bytes > free: reasons.append('base_demand_exceeds_captured_gross_free_bytes')
    if any(uint(e['destination_asize']) > uint(destination['asize']) for e in extents):
        reasons.append('individual_extent_exceeds_destination_asize')
    return dict(schema='offline-relocation-demand-v1', input_sha256=expected_sha,
                cut=result['cut'], cut_sha256=expected_cut_sha, source=source, destination=destination,
                demand_calculation_complete=True, single_destination_contract_satisfied=not reasons,
                refusal_reasons=reasons, replica_conflicts=conflicts, extents=extents,
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
    for name in ('sha256', 'cut-sha256', 'source-id', 'source-guid', 'destination-id', 'destination-guid'):
        p.add_argument('--'+name, required=True)
    p.add_argument('--exit-status', type=pathlib.Path, required=True)
    p.add_argument('--output', type=pathlib.Path, required=True)
    a = p.parse_args()
    try:
        result = analyze(read_regular(a.capture, LIMIT), read_regular(a.exit_status, 64).decode('ascii'),
                         a.sha256, a.cut_sha256, a.source_id, a.source_guid, a.destination_id, a.destination_guid)
        with a.output.open('x') as f:
            json.dump(result, f, sort_keys=True, indent=2)
            f.write('\n')
    except (ValueError, OSError) as e:
        print('refused: '+str(e), file=sys.stderr)
        return 2
    return 0 if result['single_destination_contract_satisfied'] else 3


if __name__ == '__main__':
    sys.exit(main())
