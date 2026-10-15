#!/usr/bin/env python3
"""Real paired evidence diagnostics. No device access, synthetic certificate or mover."""
import argparse, collections, hashlib, json, pathlib
from observed_intervals import parse, require, uint, analyze as observed, FLAGS, record_schema

def canonical(x):return json.dumps(x,sort_keys=True,separators=(',',':')).encode()
def digest(x):return hashlib.sha256(canonical(x)).hexdigest()
def cut(pool,vdevs):
    record_schema(pool,'pool')
    for v in vdevs:record_schema(v,'vdev')
    return {'guid':pool['guid'],'txg':pool['txg'],'root_bp':pool['root_bp'],
            'topology':sorted(vdevs,key=lambda v:uint(v['id']))}
def validate_cut(c):
    require(type(c) is dict and set(c)=={'guid','txg','root_bp','topology'},'invalid cut schema')
    record_schema({k:c[k] for k in ('guid','txg','root_bp')},'pool')
    require(type(c['topology']) is list,'invalid cut topology')
    for v in c['topology']:record_schema(v,'vdev')
def same_cut(left,right):
    validate_cut(left);validate_cut(right)
    require(left==right,'cut identity mismatch: GUID/TXG/root/topology')
def union(spans):
    out=[]
    for s,e in sorted(spans):
        require(0<=s<e<=2**64-1,'invalid interval')
        if out and s<=out[-1][1]:out[-1][1]=max(out[-1][1],e)
        else:out.append([s,e])
    return out

def difference(left,right):
    out=[];right=union(right);j=0
    for s,e in union(left):
        while j<len(right) and right[j][1]<=s:j+=1
        k=j;pos=s
        while k<len(right) and right[k][0]<e:
            a,b=right[k]
            if a>pos:out.append([pos,min(a,e)])
            pos=max(pos,b)
            if pos>=e:break
            k+=1
        if pos<e:out.append([pos,e])
    return out

def apply(tree,s,e,op):
    if op=='alloc':
        require(all(e<=a or b<=s for a,b in tree),'overlapping allocation')
        return union(tree+[[s,e]])
    require(op=='free','unknown map operation')
    require(not difference([[s,e]],tree),'free outside allocated tree')
    return difference(tree,[[s,e]])

def analyze(data,exit_status,expected_sha):
    require(hashlib.sha256(data).hexdigest()==expected_sha,'stream hash mismatch')
    require(data.endswith(b'\n'),'truncated stream')
    rows=[parse(x) for x in data.splitlines()]
    for r in rows:record_schema(r)
    counts=collections.Counter(r['record'] for r in rows)
    for kind in ['begin','pool','end','allocation_scope','cut_end','allocation_end']:
        require(counts[kind]==1,'missing/duplicate '+kind)
    require(rows[0]['schema']=='zdb-observed-allocation-v1','paired schema required')
    require(rows[-3]['record']=='cut_end' and rows[-2]['record']=='allocation_end' and rows[-1]['record']=='end','terminal order')
    require(all(all(r.get(k) is False for k in FLAGS) for r in rows),'certification flags')
    kinds={'begin','pool','vdev','bp','dnode','end','allocation_scope','metaslab','allocation_map','allocation_op','allocation','cut_end','allocation_end'}
    require(set(counts)<=kinds,'unknown record type')
    # Validate the BP stream via a lossless projection; no synthetic conversion.
    bp_rows=[dict(r) for r in rows if r['record'] in {'begin','pool','vdev','bp','dnode','end'}]
    bp_rows[0]['schema']='zdb-observed-bp-v1'
    observation,refs=observed(b''.join(canonical(r)+b'\n' for r in bp_rows),exit_status)
    observation['input_sha256']=expected_sha
    observation['input_projection']='BP records from validated paired stream'
    pool=observation['pool'];vdevs={r['id']:r for r in rows if r['record']=='vdev'}
    identity=cut(pool,list(vdevs.values()))
    require(rows[-3]['txg']==pool['txg'] and rows[-3]['root_bp']==pool['root_bp'],'end cut mismatch')
    scope=next(r for r in rows if r['record']=='allocation_scope')
    require(scope['checkpoint_unsupported'] is False and scope['removal_unsupported'] is False and scope['allocation_adjustments']=='none','unsupported allocation scope')
    require(scope['owner_obligations']==['DDT:not_enumerated','deferred_free:not_enumerated','async_destroy:not_enumerated','ZIL:not_enumerated'],'missing owner obligations')
    first=rows.index(scope)
    require(all(r['record'] in {'begin','pool','vdev','bp','dnode'} for r in rows[:first]),'premature allocation records')
    phase='base';maps={};tree={};actual={};operations=[];current=None;last_ms=None;last_log=0;map_bytes=0
    record_count=0;base_seen=set();map_objects=set()
    for r in rows[first:-3]:
        kind=r['record'];record_count+=1
        if kind=='allocation_scope':continue
        if kind=='metaslab':
            require(phase=='base','late metaslab');key=(r['vdev_id'],r['id']);v=vdevs[key[0]]
            require(key not in maps and (last_ms is None or (uint(key[0]),uint(key[1]))>(uint(last_ms[0]),uint(last_ms[1]))),'metaslab order')
            s=uint(r['start']);n=uint(r['length']);u=uint(r['unflushed_txg']);sec=1<<uint(v['ashift'])
            require(n>0 and s+n<=uint(v['asize']) and s%sec==n%sec==0 and u<=uint(pool['txg']),'metaslab bounds')
            for (vid,_),old in maps.items():
                if vid==key[0]:require(s>=uint(old['start'])+uint(old['length']),'overlapping metaslabs')
            maps[key]=r;tree[key]=[];actual[key]=[];last_ms=key;current=None
        elif kind=='allocation_map':
            source=r['source'];length=uint(r['length']);obj=uint(r['map_object']);txg=uint(r['log_txg'])
            if obj:
                require(obj not in map_objects,'map object reused across owners');map_objects.add(obj)
            require(length%8==0 and type(r['shift']) is int and 0<=r['shift']<=63,'map encoding')
            require(phase!='allocation','late map')
            if source=='base':
                require(phase=='base' and last_ms is not None and current is None,'base map order')
                require(last_ms not in base_seen,'duplicate base map');base_seen.add(last_ms)
                require(str(obj)==maps[last_ms]['map_object'] and txg==0,'base provenance')
                require(obj!=0 or length==0,'absent base map length')
                if obj:require(r['shift']==uint(vdevs[last_ms[0]]['ashift']),'base shift')
            else:
                require(source=='log' and 0<txg<=uint(pool['txg']) and txg>last_log and obj>0 and r['shift']==9,'log order/provenance')
                phase='log';last_log=txg
            current=dict(record=r,ordinal=0);map_bytes+=length
        elif kind=='allocation_op':
            require(current is not None and phase!='allocation','operation without map')
            cm=current['record'];key=(r['vdev_id'],r['metaslab']);require(key in maps,'unknown metaslab');ms=maps[key]
            require(r['source']==cm['source'] and r['map_object']==cm['map_object'] and r['log_txg']==cm['log_txg'],'operation provenance')
            current['ordinal']+=1;require(uint(r['ordinal'])==current['ordinal'],'operation ordinal')
            require(current['ordinal']<=uint(cm['length'])//8,'too many map operations')
            if cm['source']=='base':require(key==last_ms,'base map identity')
            s=uint(r['start']);n=uint(r['length']);e=s+n;sec=1<<uint(vdevs[key[0]]['ashift'])
            require(n>0 and s>=uint(ms['start']) and e<=uint(ms['start'])+uint(ms['length']) and e<2**64 and s%sec==n%sec==0,'operation bounds')
            should_apply=cm['source']=='base' or uint(cm['log_txg'])>=uint(ms['unflushed_txg'])
            require(type(r['applied']) is bool and r['applied']==should_apply,'replay threshold')
            require(r['op'] in ('alloc','free'),'operation type')
            if should_apply:tree[key]=apply(tree[key],s,e,r['op'])
            operations.append(r)
        elif kind=='allocation':
            phase='allocation';key=(r['vdev_id'],r['metaslab']);require(key in maps,'unknown allocation metaslab');ms=maps[key]
            require(r['vdev_guid']==vdevs[key[0]]['guid'],'allocation GUID')
            s=uint(r['start']);n=uint(r['length']);e=s+n
            require(n>0 and s>=uint(ms['start']) and e<=uint(ms['start'])+uint(ms['length']),'allocation bounds')
            require(not actual[key] or s>actual[key][-1][1],'overlap/unsorted/uncoalesced allocations')
            actual[key].append([s,e])
        else:raise ValueError('unexpected allocation record '+kind)
    for vid,v in vdevs.items():
        ids=sorted(uint(k[1]) for k in maps if k[0]==vid)
        require(ids==list(range(uint(v['metaslab_count']))) and ids,'metaslab coverage')
    require(base_seen==set(maps),'missing base map provenance')
    require(tree==actual,'final allocation intervals disagree with independent replay')
    end=rows[-2];require(uint(end['records'])==record_count and uint(end['intervals'])==counts['allocation'],'allocation counts')
    total=sum(e-s for spans in actual.values() for s,e in spans)
    require(uint(end['bytes'])==total and uint(end['map_bytes'])==map_bytes and uint(end['map_words'])==map_bytes//8,'allocation totals')
    per={};missing=[];outside=[]
    for vid,v in vdevs.items():
        aa=[span for key,spans in actual.items() if key[0]==vid for span in spans]
        oo=[[int(r['start']),int(r['end'])] for r in refs if r['vdev_id']==vid]
        for r in refs:
            if r['vdev_id']==vid:require(int(r['end'])<=uint(v['asize']),'BP outside vdev')
        aa=union(aa);oo=union(oo);minus=difference(aa,oo);plus=difference(oo,aa)
        for target,spans in [(missing,minus),(outside,plus)]:
            target.extend({'vdev_id':vid,'vdev_guid':v['guid'],'start':str(s),'length':str(e-s),'owner_explanation':'unknown'} for s,e in spans)
        per[vid]={'allocation_bytes':sum(e-s for s,e in aa),'observed_bytes':sum(e-s for s,e in oo),'allocation_union_sha256':digest(aa),'observed_union_sha256':digest(oo)}
    return dict(schema='real-paired-interval-analysis-v1',input_sha256=expected_sha,cut=identity,record_counts=dict(counts),allocation_bytes=total,operation_sha256=digest(operations),allocation_intervals_sha256=digest([{'vdev':k[0],'metaslab':k[1],'spans':v} for k,v in sorted(actual.items())]),per_vdev=per,A_minus_observed=missing,observed_minus_A=outside,observation=observation,owner_obligations=scope['owner_obligations'],observed_interval_references=refs,allocation_records=[r for r in rows if r['record']=='allocation'],address_sets_equal=not missing and not outside,reconciliation_passed=not missing and not outside and not observation['descriptor_conflicts'] and not observation['partial_overlaps'] and not observation['unknowns'],**FLAGS)

def main():
    p=argparse.ArgumentParser();p.add_argument('stream',type=pathlib.Path);p.add_argument('--sha256',required=True);p.add_argument('--exit-status',required=True,type=pathlib.Path);p.add_argument('--output',required=True,type=pathlib.Path);p.add_argument('--same-cut-as',type=pathlib.Path)
    a=p.parse_args();result=analyze(a.stream.read_bytes(),a.exit_status.read_text(),a.sha256)
    if a.same_cut_as:same_cut(result['cut'],parse(a.same_cut_as.read_text())['cut'])
    with a.output.open('x') as f:json.dump(result,f,indent=2);f.write('\n')
if __name__=='__main__':main()
