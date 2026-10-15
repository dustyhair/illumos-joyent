#!/usr/bin/env python3
"""Validate measured original-BP read results bound to one paired capture."""
import argparse,collections,hashlib,json,pathlib
from observed_intervals import parse,record_schema,require,uint,FLAGS,words,DMU_OT_NUMTYPES,DMU_BSWAP_NUMFUNCS
from paired_intervals import analyze as paired,canonical,digest,same_cut

def analyze(data,exit_status,expected_sha):
    require(exit_status.strip()=='0','process failed')
    require(hashlib.sha256(data).hexdigest()==expected_sha,'stream hash mismatch')
    require(data.endswith(b'\n'),'truncated stream')
    rows=[parse(s) for s in data.splitlines()]
    require(len(rows)>=4,'missing verified envelope')
    for r in rows:record_schema(r)
    require(rows[0].get('schema') in ('zdb-observed-verified-v1','zdb-observed-verified-v2','zdb-observed-verified-v3'),'verified schema required')
    require(all(all(r.get(k) is False for k in FLAGS) for r in rows),'global certification flag')
    ends=[r for r in rows if r['record']=='verification_end']
    require(len(ends)==1 and rows[-3] is ends[0],'verification terminal order/count')
    stripped=[dict(r) for r in rows if r['record'] not in ('verification','verification_end')]
    stripped[0]['schema']=rows[0]['schema'].replace('verified','allocation')
    projected=b''.join(canonical(r)+b'\n' for r in stripped)
    result=paired(projected,exit_status,hashlib.sha256(projected).hexdigest())
    result['input_sha256']=expected_sha;result['observation']['input_sha256']=expected_sha
    bps={r['sequence']:r for r in rows if r['record']=='bp'}
    checks=[];seen=set();verified_p=0;passed=0;na=0
    for i,r in enumerate(rows):
        if r['record']!='verification':continue
        seq=r['sequence'];require(seq in bps and seq not in seen,'unbound/duplicate verification')
        seen.add(seq);bp=bps[seq]
        require(i>0 and rows[i-1] is bp,'verification must immediately follow its BP')
        for key in ('bp_words','objset','object','level','blkid'):require(r[key]==bp[key],'verification context mismatch: '+key)
        require(r['error']==0,'failed verification result')
        # Native zv_record applies these gates BEFORE hole/embedded disposition.
        # raw_classification intentionally classifies nonallocated layouts before
        # dedup, so paired validation alone cannot enforce verifier eligibility.
        prop=words(bp['bp_words'])[6];typ=(prop>>48)&255
        valid=(typ&31)<DMU_BSWAP_NUMFUNCS if typ&128 else typ<DMU_OT_NUMTYPES
        require(valid and not (prop & ((1<<61)|(1<<62))) and typ not in (42,43,49),
                'native verification eligibility: type/crypt/dedup/DDT')
        require(bp['unsupported'] is False and bp['children_skipped'] is False,
                'native verification eligibility: rejected geometry/layout')
        if bp['classification'] in ('hole','embedded'):
            require(r['status']=='not_applicable' and r['measured'] is False and uint(r['requested_p'])==uint(r['completed_p'])==0,'nonallocated verification')
            na+=1
        else:
            require(bp['classification']=='ordinary' and bp['unsupported'] is False and bp['children_skipped'] is False,'unsupported BP verified')
            require(bp['bp_type'] not in (42,43,49) and bp['checksum_id'] in (7,8),'unsupported type/checksum verified')
            require(r['status']=='verified' and r['measured'] is True,'missing measured result')
            p=uint(bp['psize']);require(0<p<=16<<20 and uint(r['requested_p'])==uint(r['completed_p'])==p,'completed byte mismatch')
            verified_p+=p;passed+=1
        checks.append(r)
    require(seen==set(bps),'BP missing verification disposition')
    end=ends[0]
    require(end['verification_complete'] is True,'verification incomplete')
    require(uint(end['seen'])==len(bps) and uint(end['not_applicable'])==na,'verification disposition counts')
    require(uint(end['submitted'])==uint(end['completed'])==uint(end['passed'])==passed,'verification completion counts')
    require(uint(end['failed'])==uint(end['unsupported'])==uint(end['outstanding'])==0,'failed or outstanding verification')
    require(uint(end['requested_p'])==uint(end['verified_p'])==verified_p<=1<<30,'verification byte counts')
    result['schema']='real-verified-interval-analysis-'+rows[0]['schema'].rsplit('-',1)[1]
    result['verification_results']=checks
    result['verification_results_sha256']=digest(checks)
    result['measured_read_summary']={'verified_bp_references':passed,'not_applicable_bp_references':na,'verified_raw_P_bytes':verified_p,'submitted':passed,'completed':passed,'outstanding':0,'scope':'original-BP checksum-valid logical payload recovery; not per-replica or per-member health'}
    return result

def main():
    p=argparse.ArgumentParser();p.add_argument('stream',type=pathlib.Path);p.add_argument('--sha256',required=True);p.add_argument('--exit-status',type=pathlib.Path,required=True);p.add_argument('--output',type=pathlib.Path,required=True);p.add_argument('--same-cut-as',type=pathlib.Path)
    a=p.parse_args();result=analyze(a.stream.read_bytes(),a.exit_status.read_text(),a.sha256)
    if a.same_cut_as:same_cut(result['cut'],parse(a.same_cut_as.read_text())['cut'])
    with a.output.open('x') as f:json.dump(result,f,indent=2);f.write('\n')
if __name__=='__main__':main()
