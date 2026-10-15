#!/usr/bin/env python3
"""Bounded offline deadlist references and contextual unflushed-TXG layouts."""
import argparse
import collections
import hashlib
import pathlib
import re
import struct
import sys

from observed_intervals import parse, require, uint, words, U64
from paired_intervals import canonical, digest
from parent_edges import bpwords, raw, valid_type
from reference_graph import analyze as graph_analyze, block_id, SAFETY
from relocation_demand import read_regular

KEY = 'com.delphix:ms_unflushed_phys_txgs'
EVIDENCE_LIMIT = 16 << 20
DEFAULT_LIMITS = dict(work=2000000, objects=100000, entries=100000,
                      decoded=8 << 20, leaves=64, output=64 << 20)


class Budget:
    def __init__(self, limits=None):
        self.limits = dict(DEFAULT_LIMITS)
        for k, v in (limits or {}).items():
            require(k in self.limits and type(v) is int and 0 <= v <= self.limits[k],
                    'invalid semantic budget')
            self.limits[k] = v
        self.used = dict.fromkeys(self.limits, 0)

    def take(self, **counts):
        for k, n in counts.items():
            require(0 <= n <= self.limits[k]-self.used[k], 'semantic budget: '+k)
        for k, n in counts.items():
            self.used[k] += n


def lz4(data, size, budget):
    """ZFS's BE32 compressed length followed by a bounded LZ4 block."""
    require(4 <= len(data) and 0 < size <= 1 << 20, 'LZ4 size')
    n = int.from_bytes(data[:4], 'big')
    require(0 < n <= len(data)-4, 'LZ4 compressed length')
    source = data[4:4+n]; at = 0; out = bytearray()

    def length(n):
        nonlocal at
        if n == 15:
            while True:
                budget.take(work=1)
                require(at < len(source), 'truncated LZ4 length')
                value = source[at]; at += 1; n += value
                require(n <= size, 'LZ4 output bound')
                if value != 255:
                    break
        return n

    while at < len(source):
        budget.take(work=1)
        token = source[at]; at += 1
        n = length(token >> 4)
        require(n <= len(source)-at and n <= size-len(out), 'LZ4 literal bounds')
        budget.take(work=n)
        out.extend(source[at:at+n]); at += n
        if at == len(source):
            break
        require(at+2 <= len(source), 'truncated LZ4 offset')
        offset = int.from_bytes(source[at:at+2], 'little'); at += 2
        require(0 < offset <= len(out), 'LZ4 match offset')
        n = length(token & 15)+4
        require(n <= size-len(out), 'LZ4 match bounds')
        budget.take(work=n)
        for _ in range(n):
            out.append(out[-offset])
    require(len(out) == size, 'LZ4 decoded length')
    return bytes(out)


def embedded(bp, budget):
    w = words(bp); p = w[6]
    require(p & (1 << 39) and not ((p >> 40) & 255) and p >> 63 == 1,
            'unsupported embedded encoding')
    size = (p & ((1 << 25)-1))+1; packed = ((p >> 25) & 127)+1
    require(size <= 128 << 10 and packed <= 112, 'embedded size')
    budget.take(decoded=size)
    data = b''.join(struct.pack('<Q', v) for i, v in enumerate(w) if i not in (6, 10))[:packed]
    compression = (p >> 32) & 127
    if compression == 2:
        require(size == packed, 'embedded uncompressed size')
        return data
    require(compression == 15, 'unsupported embedded compression')
    return lz4(data, size, budget)


def microzap(data, budget):
    require(512 <= len(data) <= 128 << 10 and len(data) % 64 == 0,
            'unsupported micro-ZAP size')
    header = struct.unpack_from('<8Q', data)
    require(header[0] == (1 << 63)+3 and header[2:] == (0,)*6,
            'unsupported micro-ZAP header/normalization')
    result = {}
    for offset in range(64, len(data), 64):
        budget.take(work=1)
        entry = data[offset:offset+64]
        value, cd, pad = struct.unpack_from('<QIH', entry)
        name = entry[14:]
        require(b'\0' in name and pad == 0, 'malformed micro-ZAP entry')
        stop = name.index(0)
        require(not any(name[stop:]), 'micro-ZAP name padding')
        if stop == 0:
            require(not any(entry), 'noncanonical empty micro-ZAP entry')
            continue
        require(all(32 <= v < 127 for v in name[:stop]), 'micro-ZAP name encoding')
        key = name[:stop].decode('ascii')
        require(key not in result, 'duplicate micro-ZAP key')
        result[key] = dict(value=value, decoded_offset=offset, collision=cd)
    return result


def normalize(bp):
    """Exact bpobj_enqueue transformation; never an original-BP identity."""
    w = words(bp)
    if w[6] & (1 << 39):
        w = [v if i in (6, 10) else 0 for i, v in enumerate(w)]
    elif not w[6] & (1 << 62):
        w[12:16] = [0]*4
    w[11] = 0
    return tuple(f'{v:016x}' for v in w)


def leaf_payloads(document, graph, budget):
    require(type(document) is dict and set(document) ==
            {'schema', 'capture_sha256', 'cut_sha256', 'leaves'}, 'leaf evidence schema')
    require(document['schema'] == 'semantic-leaf-evidence-v1' and
            document['capture_sha256'] == graph['input_sha256'] and
            document['cut_sha256'] == graph['cut_sha256'], 'leaf evidence cut binding')
    require(type(document['leaves']) is list, 'leaf evidence list')
    blocks = {b['id']: b for b in graph['blocks']}; payloads = {}
    for record in document['leaves']:
        budget.take(leaves=1)
        require(type(record) is dict and set(record) == {'bp_words', 'physical_hex', 'source_replica'},
                'leaf record schema')
        w = words(record['bp_words']); key = block_id(record['bp_words'])
        require(key in blocks and not blocks[key]['container'] and key not in payloads,
                'unknown/duplicate/non-leaf original BP')
        replica = record['source_replica']
        require(type(replica) is dict and set(replica) == {'slot','vdev_id','vdev_guid','offset','asize'} and
                type(replica['slot']) is int and replica in blocks[key]['replicas'],
                'leaf source replica mismatch')
        p = w[6]; size = ((p & 65535)+1)*512; packed = (((p >> 16) & 65535)+1)*512
        require(p >> 63 == 1 and not p & ((1 << 39) | (1 << 61) | (1 << 62)) and
                not (p >> 56) & 31 and (p >> 48) & 255 in (5, 195),
                'unsupported semantic leaf BP')
        require((p >> 40) & 255 == 7 and size <= 1 << 20 and packed <= 1 << 20,
                'unsupported leaf checksum/size')
        text = record['physical_hex']
        require(type(text) is str and len(text) == packed*2 and
                re.fullmatch('[0-9a-f]+', text) is not None, 'physical leaf length/hex')
        data = bytes.fromhex(text); sums = [0]*4
        budget.take(work=packed//4, decoded=size)
        for (value,) in struct.iter_unpack('<I', data):
            sums[0] = (sums[0]+value) & U64
            for i in range(1, 4):
                sums[i] = (sums[i]+sums[i-1]) & U64
        require(sums == w[12:16], 'original-BP physical Fletcher4 mismatch')
        compression = (p >> 32) & 127
        if compression == 2:
            require(packed == size, 'uncompressed leaf size')
            decoded = data
        else:
            require(compression == 15, 'unsupported leaf compression')
            decoded = lz4(data, size, budget)
        payloads[key] = dict(data=decoded, physical_sha256=hashlib.sha256(data).hexdigest(),
                             decoded_sha256=hashlib.sha256(decoded).hexdigest(), source_replica=replica)
    return payloads


def mos_objects(records, budget):
    edges = {r['node']: r for r in records if r['record'] == 'parent_edge'}
    objects = {}; seen = {}
    for r in records:
        budget.take(work=1)
        if r['record'] != 'parent_container':
            continue
        edge = edges[r['node']]; z = edge['bookmark']; w = words(edge['child_bp'])
        if z[:3] != ['0', '0', '0'] or (w[6] >> 48) & 255 != 10:
            continue
        key = block_id(edge['child_bp'])
        # Dnode object numbers depend on the logical block index. A shared
        # physical buffer at another index describes another set of owners.
        logical = uint(z[3]); observation = (key, r['decoded_hex'])
        if logical in seen:
            require(seen[logical] == observation, 'conflicting logical MOS dnode-block observation')
            continue
        seen[logical] = observation; data = bytes.fromhex(r['decoded_hex'])
        for offset in range(0, len(data), 512):
            budget.take(work=1)
            d = data[offset:offset+512]
            if d[0] == 0:
                continue
            budget.take(objects=1)
            obj = logical*(len(data)//512)+offset//512
            require(obj not in objects, 'conflicting MOS object observation')
            start = 64+d[3]*128; n = int.from_bytes(d[10:12], 'little')
            objects[obj] = dict(object=obj, type=d[0], bonus_type=d[4], bonus=d[start:start+n],
                                levels=d[2], count=d[3], flags=d[7],
                                block_size=int.from_bytes(d[8:10], 'little')*512,
                                maxblkid=int.from_bytes(d[16:24], 'little'),
                                pointers=[bpwords(d[64+i*128:192+i*128]) for i in range(d[3])],
                                provenance=dict(parent_block=key, dnode_offset=str(offset)),
                                dnode_hex=d.hex())
    return objects


def single_pointer(obj):
    require(obj['levels'] == 1 and obj['maxblkid'] == 0 and 1 <= obj['count'] <= 3 and
            not obj['flags'] & 4, 'unsupported semantic object tree')
    require(all(bp == ['0'*16]*16 for bp in obj['pointers'][1:]),
            'unsupported extra semantic data pointers')
    return obj['pointers'][0]


def resolve(stored, index):
    require(tuple(stored) == normalize(stored), 'noncanonical stored BPOBJ normalization')
    w = words(stored); p = w[6]
    require(valid_type((p >> 48) & 255) and not p & (1 << 61), 'unsupported stored BP type/encryption')
    if p & (1 << 39):
        require(not ((p >> 40) & 255), 'unsupported embedded BPOBJ entry')
    else:
        require(any(w[i] & 0xffffff for i in (0, 2, 4)), 'hole BPOBJ entry')
        require(all(not w[i] >> 63 for i in (1, 3, 5)), 'gang BPOBJ entry')
    matches = index.get(tuple(stored), [])
    require(len(matches) <= 1, 'ambiguous normalized original-BP match')
    return matches[0] if matches else None


def _build(graph, records, payloads, budget):
    objects = mos_objects(records, budget); zaps = {}; contexts = []; obligations = []
    blocks = {b['id']: b for b in graph['blocks']}
    cut_txg = uint(graph['cut']['txg'])
    for obj, value in sorted(objects.items()):
        budget.take(work=1)
        if value['type'] not in (50, 196) or (value['type'] == 196 and value['bonus_type'] != 0):
            continue
        # Non-embedded ZAPs are explicitly outside this initial decoder.
        if value['levels'] != 1 or value['maxblkid'] != 0 or not value['count']:
            continue
        bp = value['pointers'][0]
        if not int(bp[6], 16) & (1 << 39):
            continue
        require(value['bonus_type'] == (51 if value['type'] == 50 else 0), 'ZAP owner bonus type')
        single_pointer(value)
        zaps[obj] = microzap(embedded(bp, budget), budget)
        if KEY in zaps[obj] and value['type'] == 196:
            child = zaps[obj][KEY]['value']
            require(child in objects and child != obj, 'invalid unflushed-TXG object link')
            array = objects[child]
            require(array['type'] == 195 and array['bonus_type'] == 0 and not array['bonus'],
                    'unflushed-TXG target layout')
            bp = single_pointer(array); p = int(bp[6], 16); key = block_id(bp)
            require(key in blocks and
                    not p & (1 << 39) and (p >> 48) & 255 == 195 and
                    array['block_size'] == ((p & 65535)+1)*512 == 128 << 10,
                    'unflushed-TXG data layout')
            budget.take(output=2048)
            contexts.append(dict(block=key, object=str(child), owner_zap_object=str(obj),
                owner_key=KEY, owner_entry_offset=str(zaps[obj][KEY]['decoded_offset']),
                owner_provenance=value['provenance'], object_provenance=array['provenance'],
                classification='ms_unflushed_txg_scalar_array_contextual',
                format='metaslab_unflushed_phys_t: one uint64 TXG per metaslab',
                contains_blkptr_slots_in_this_layout=False,
                vdev_registration_independently_validated=False,
                payload_values_validated=key in payloads,
                accounting_metadata_must_be_preserved=True))
            if key in payloads:
                values = struct.unpack('<'+'Q'*(len(payloads[key]['data'])//8), payloads[key]['data'])
                budget.take(work=len(values))
                require(all(v <= cut_txg for v in values), 'future unflushed TXG')
            obligations.append(dict(kind='unflushed_TXG_accounting_and_owner_registration',
                                    object=str(child), status='unresolved'))
    require(len({r['block'] for r in contexts}) == len(contexts), 'ambiguous unflushed-TXG owners')
    owners = collections.defaultdict(list)
    for obj, value in sorted(objects.items()):
        budget.take(work=1)
        if value['bonus_type'] != 16:
            continue
        require(len(value['bonus']) == 320, 'dataset semantic bonus length')
        deadlist = int.from_bytes(value['bonus'][64:72], 'little')
        require(deadlist in objects and objects[deadlist]['type'] == 50 and
                objects[deadlist]['bonus_type'] == 51 and len(objects[deadlist]['bonus']) == 320 and deadlist in zaps,
                'unsupported/missing dataset deadlist map')
        buckets = []
        for name, entry in zaps[deadlist].items():
            require(re.fullmatch('0|[1-9a-f][0-9a-f]*', name) and len(name) <= 16,
                    'noncanonical deadlist birth key')
            birth = int(name, 16)
            require(birth <= cut_txg, 'future deadlist birth key')
            buckets.append((birth, entry))
        buckets.sort()
        for i, (birth, entry) in enumerate(buckets):
            child = entry['value']
            require(child in objects and objects[child]['type'] == 5 and child not in (obj, deadlist),
                    'invalid deadlist BPOBJ link')
            budget.take(output=1024)
            owners[child].append(dict(dataset_object=str(obj), deadlist_object=str(deadlist),
                bpobj_object=str(child), mintxg=str(birth),
                maxtxg=str(buckets[i+1][0]) if i+1 < len(buckets) else None,
                dataset_provenance=value['provenance'],
                deadlist_provenance=objects[deadlist]['provenance'],
                deadlist_entry_offset=str(entry['decoded_offset'])))
    index = collections.defaultdict(list)
    for key, block in blocks.items():
        budget.take(work=1)
        index[normalize(block['bp_words'])].append(key)
    closure = set(graph['observed_closure']['blocks']); references = []; decoded_objects = []
    for obj, chains in sorted(owners.items()):
        value = objects[obj]; bonus = value['bonus']
        require(value['bonus_type'] == 6 and len(bonus) == 48, 'unsupported BPOBJ header')
        count, space, compressed, uncompressed, subobject, subcount = struct.unpack('<6Q', bonus)
        require(subobject == subcount == 0, 'unsupported BPOBJ subobjects')
        require(count <= U64//128, 'BPOBJ entry offset overflow')
        budget.take(entries=count, output=(2048+1024*len(chains))*count+1024*len(chains))
        bp = single_pointer(value)
        require(value['block_size'] > 0 and count <= value['block_size']//128,
                'BPOBJ count/data bounds')
        if not count:
            require(space == compressed == uncompressed == 0 and bp == ['0'*16]*16,
                    'unsupported noncanonical empty BPOBJ')
            continue
        key = block_id(bp)
        require(key in blocks and blocks[key]['bp_type'] == 5 and
                ((int(bp[6], 16) & 65535)+1)*512 == value['block_size'], 'BPOBJ original data BP')
        require(key in payloads, 'missing authenticated raw BPOBJ leaf evidence')
        data = payloads[key]['data']
        require(count*128 <= len(data) == value['block_size'], 'truncated BPOBJ payload')
        decoded_objects.append(dict(object=str(obj), block=key, entries=str(count),
            header_u64=[str(v) for v in (count, space, compressed, uncompressed, subobject, subcount)],
            object_provenance=value['provenance'], owner_chains=chains,
            source_replica=payloads[key]['source_replica'],
            physical_sha256=payloads[key]['physical_sha256'], decoded_sha256=payloads[key]['decoded_sha256']))
        for i in range(count):
            budget.take(work=1)
            stored = bpwords(data[i*128:(i+1)*128]); matched = resolve(stored, index)
            birth = int(stored[10], 16)
            require(0 < birth <= cut_txg and all(birth > uint(c['mintxg']) and
                    (c['maxtxg'] is None or birth <= uint(c['maxtxg'])) for c in chains),
                    'BPOBJ entry birth outside owner bucket/cut')
            relationship = ('target' if matched == graph['target']['block'] else
                            'ancestor' if matched in closure else 'outside_observed_closure' if matched else None)
            references.append(dict(kind='dataset_deadlist_stored_BP', bpobj_object=str(obj),
                owner_block=key, owner_in_observed_closure=key in closure, owner_chains=chains,
                entry_index=str(i), decoded_byte_offset=str(i*128), stored_bp_words=stored,
                original_BP_identity=False, matched_block=matched,
                match_status='unique_among_captured_originals' if matched else 'unmatched',
                candidate_count_among_captured_originals=1 if matched else 0,
                historical_original_identity_unresolved=True,
                stored_BP_payload_verifiable=False,
                match_scope='captured_original_BP_descriptors_only', relationship=relationship))
            if matched is None:
                obligations.append(dict(kind='unmatched_stored_BP', bpobj_object=str(obj),
                                        entry_index=str(i), status='unresolved'))
    classified = {r['block']: r for r in contexts}
    classified.update({r['block']: dict(classification='dataset_deadlist_BP_payload_decoded',
                                       object=r['object']) for r in decoded_objects})
    inventory = []
    for row in graph['semantic_payloads']:
        value = dict(row)
        if row['block'] in classified:
            value.update(classified[row['block']])
            value['ownership_status'] = 'captured_object_link_context'
        else:
            obligations.append(dict(kind='opaque_semantic_payload', block=row['block'], status='unresolved'))
        inventory.append(value)
    result = dict(schema='observed-semantic-references-v1', capture_sha256=graph['input_sha256'],
        cut_sha256=graph['cut_sha256'], structural_graph_canonical_sha256=digest(graph),
        target=graph['target'], observed_closure=graph['observed_closure'],
        semantic_payloads=inventory, unflushed_TXG_contexts=contexts,
        decoded_bpobj_objects=decoded_objects, semantic_references=references,
        counts=dict(semantic_references=len(references), decoded_bpobj_objects=len(decoded_objects),
                    contextual_TXG_arrays=len(contexts), target_matches=sum(r['relationship']=='target' for r in references),
                    ancestor_matches=sum(r['relationship']=='ancestor' for r in references),
                    unmatched=sum(r['matched_block'] is None for r in references)),
        unresolved_obligations=graph['unresolved_obligations']+obligations,
        semantic_enumeration_complete=False,
        limits=['Only authenticated embedded micro-ZAP ownership and 48-byte, single-leaf, zero-subobject deadlist BPOBJs supported.',
                'Stored BPs are normalized references, never original identity or child-payload checksum proof.',
                'Match uniqueness applies only to captured originals; erased fields leave global original-identity ambiguity.',
                'Header counts cover only each decoded BPOBJ, not other owners or historical roots.',
                'TXG layout classification does not prove vdev registration or discharge allocation/log accounting.',
                'Structural closure/order is unchanged; semantic matches are not a COW or write schedule.'],
        budget=dict(limits=budget.limits, used=budget.used), **SAFETY)
    require(len(canonical(result))+1 <= budget.limits['output'], 'semantic serialized output budget')
    return result


def analyze(data, status, expected_sha, expected_cut_sha, target,
            leaf_data, leaf_sha256, limits=None):
    graph = graph_analyze(data, status, expected_sha, expected_cut_sha, target)
    require(type(leaf_data) is bytes and len(leaf_data) <= EVIDENCE_LIMIT and
            hashlib.sha256(leaf_data).hexdigest() == leaf_sha256, 'missing/mismatched raw leaf evidence')
    budget = Budget(limits)
    try:
        payloads = leaf_payloads(parse(leaf_data), graph, budget)
        records = [parse(line) for line in data.splitlines()]
        result = _build(graph, records, payloads, budget)
    except (KeyError, IndexError, TypeError, struct.error) as error:
        raise ValueError('malformed semantic evidence') from error
    result['leaf_evidence_sha256'] = leaf_sha256
    require(len(canonical(result))+1 <= budget.limits['output'], 'semantic serialized output budget')
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('capture', type=pathlib.Path)
    for name in ('sha256', 'cut-sha256', 'target-vdev-id', 'target-vdev-guid',
                 'target-offset', 'target-asize', 'target-dva-slot', 'leaf-sha256'):
        p.add_argument('--'+name, required=True)
    for name in ('exit-status', 'leaf-evidence', 'output'):
        p.add_argument('--'+name, type=pathlib.Path, required=True)
    a = p.parse_args()
    target = dict(vdev_id=a.target_vdev_id, vdev_guid=a.target_vdev_guid,
                  offset=a.target_offset, asize=a.target_asize, dva_slot=a.target_dva_slot)
    try:
        result = analyze(read_regular(a.capture, 512 << 20), read_regular(a.exit_status, 64).decode('ascii'),
            a.sha256, a.cut_sha256, target, read_regular(a.leaf_evidence, EVIDENCE_LIMIT), a.leaf_sha256)
        with a.output.open('xb') as f:
            f.write(canonical(result)+b'\n')
    except (ValueError, OSError) as error:
        print('refused: '+str(error), file=sys.stderr)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main())
