#!/usr/bin/env python3
"""Strict supported-profile evidence only; no mover or evacuation clearance."""
import argparse,hashlib,json,pathlib
from observed_intervals import parse,record_schema,require,uint,words,FLAGS,DMU_OT_NUMTYPES,DMU_BSWAP_NUMFUNCS
from paired_intervals import canonical,same_cut
from verified_intervals import analyze as verified
KINDS={'profile_scope','profile_directory','profile_object','profile_dataset','profile_end'}

def metadata(rows,result):
    require(len(rows)>=2 and rows[0]['record']=='profile_scope' and rows[-1]['record']=='profile_end','profile envelope')
    scope,end=rows[0],rows[-1]
    require(scope['profile']=='ddt-absent-zil-zero-v1' and scope['ddt_enumerated'] is scope['zil_enumerated'] is False,'profile scope/claim')
    require(end['profile_metadata_complete'] is True and end['ddt_absence_checked'] is True and end['error']==0,'incomplete profile metadata')
    require(uint(end['cached_ddt_slots'])==42,'pinned cached DDT dimensions')
    require(uint(end['reads'])<=250000 and uint(end['read_bytes'])<=32<<20,'profile budget')
    pos=1;names=set();objects=[];datasets=[];heads=[];snapshots=[]
    while pos<len(rows)-1 and rows[pos]['record']=='profile_directory':
        r=rows[pos];pos+=1;n=r['name']
        require(n and '\0' not in n and not n.startswith('DDT') and n not in names and len(names)<4096,'DDT/invalid directory key')
        names.add(n)
    previous=0
    verified_words={tuple(r['bp_words']) for r in result['verification_results'] if r['status']=='verified'}
    while pos<len(rows)-1:
        r=rows[pos];pos+=1;require(r['record']=='profile_object','profile object ordering')
        obj=uint(r['object']);require(obj>previous and len(objects)<100000,'profile scan order/budget');previous=obj
        for t in (r['object_type'],r['bonus_type']):
            valid=(t&31)<DMU_BSWAP_NUMFUNCS if t&128 else t<DMU_OT_NUMTYPES
            require(valid and t not in (42,43),'DDT/unknown MOS object')
        require(r['object_type']!=16 or r['bonus_type']==16,'ambiguous dataset object')
        objects.append(r)
        if r['bonus_type']!=16:continue
        require(r['object_type'] in (16,196),'unsupported dataset object layout')
        require(pos<len(rows)-1 and rows[pos]['record']=='profile_dataset','missing dataset disposition');d=rows[pos];pos+=1
        require(uint(d['object'])==obj and len(datasets)<4096,'dataset provenance/budget')
        require(uint(d['dataset_flags']) & ~((1<<1)|(1<<2)|(1<<16))==0,'unsupported dataset flags')
        snap=uint(d['children'])!=0;require(d['snapshot'] is snap,'snapshot evidence mismatch')
        if snap:
            require(uint(d['next_snap_object']) not in (0,obj),'snapshot next link')
            require(uint(d['snapnames_object'])==0,'snapshot name map')
            require(d['zil_state']=='snapshot_not_applicable' and d['zil_header_words'] is None,'snapshot ZIL disposition');snapshots.append(d)
        else:
            require(uint(d['next_snap_object'])==0,'head next link')
            require(d['zil_state']=='canonical_zero_header' and d['zil_header_words']==['0000000000000000']*24,'nonempty/ambiguous ZIL header')
            w=words(d['root_bp']);require((w[6]>>48)&255==11 and (w[6]>>56)&31==0,'head root type/level')
            require(tuple(d['root_bp']) in verified_words,'head root lacks original-BP verification')
            heads.append(d)
        datasets.append(d)
    require(heads,'no relevant heads')
    requested_min=sum(((words(h['root_bp'])[6]&65535)+1)*512 for h in heads)+128*len(datasets)
    require(uint(end['read_bytes'])>=requested_min,'profile requested-byte lower bound')
    for k,n in [('directory_entries',len(names)),('objects',len(objects)),('datasets',len(datasets)),('heads',len(heads)),('snapshots',len(snapshots)),('active_object',previous)]:require(uint(end[k])==n,'profile counter '+k)
    # Count lower bound for directory init/retrieve/advance, MOS next/info/end,
    # dataset bonus and head ARC reads. Internal I/O amplification is excluded.
    require(uint(end['reads'])==2+2*len(names)+2*len(objects)+1+len(datasets)+len(heads),'profile read-operation accounting')
    return dict(profile_metadata_records=rows,ddt_absence_verified=True,zil_empty_heads_verified=True,profile_heads=len(heads),profile_snapshots_not_applicable=len(snapshots))

def analyze(data,exit_status,expected_sha):
    require(exit_status.strip()=='0','profile process failed')
    require(hashlib.sha256(data).hexdigest()==expected_sha and data.endswith(b'\n'),'profile hash/truncation')
    rows=[parse(s) for s in data.splitlines()]
    for r in rows:record_schema(r)
    require(rows and rows[0]['schema']=='zdb-observed-profile-v1','profile schema required')
    require(all(all(r.get(k) is False for k in FLAGS) for r in rows),'global profile certification claim')
    inds=[i for i,r in enumerate(rows) if r['record'] in KINDS]
    cuts=[i for i,r in enumerate(rows) if r['record']=='cut_end'];require(len(cuts)==1,'profile cut')
    require(inds and inds==list(range(inds[0],cuts[0])),'profile section missing/order')
    evidence=rows[inds[0]:cuts[0]]
    projected=[dict(r) for r in rows if r['record'] not in KINDS];projected[0]['schema']='zdb-observed-verified-v3'
    raw=b''.join(canonical(r)+b'\n' for r in projected)
    result=verified(raw,exit_status,hashlib.sha256(raw).hexdigest())
    require(not result['pending_owner_records'] and not result['async_entry_records'],'initial profile requires empty pending/async queues')
    require(result['reconciliation_passed'] and result['address_sets_equal'],'profile exact supported reconciliation required')
    require(not result['observation']['unknowns'],'unsupported descriptors')
    require(result['measured_read_summary']['verified_bp_references']>0,'original BP verification missing')
    result.update(metadata(evidence,result))
    result.update(schema='supported-profile-analysis-v1',input_sha256=expected_sha,supported_profile_passed=True,profile_scope='read-only dry-run input eligibility; no planner implemented',ddt_enumerated=False,zil_enumerated=False,movable_candidate=False,**FLAGS)
    result['observation']['input_sha256']=expected_sha
    return result

def main():
    p=argparse.ArgumentParser();p.add_argument('stream',type=pathlib.Path);p.add_argument('--sha256',required=True);p.add_argument('--exit-status',type=pathlib.Path,required=True);p.add_argument('--output',type=pathlib.Path,required=True)
    a=p.parse_args();r=analyze(a.stream.read_bytes(),a.exit_status.read_text(),a.sha256)
    with a.output.open('x') as f:json.dump(r,f,indent=2);f.write('\n')
if __name__=='__main__':main()
