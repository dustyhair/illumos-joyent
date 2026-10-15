"""Real observation diagnostics only. No synthetic certificate, device or pool API."""
import collections, hashlib, json, pathlib, re
FLAGS=dict(coverage_complete=False,payload_validation_performed=False,evacuation_authorized=False)
U64=2**64-1

def require(ok,why):
    if not ok: raise ValueError(why)
def pairs(items):
    out={}
    for k,v in items:
        require(k not in out,'duplicate JSON key '+k);out[k]=v
    return out

def parse(s):
    return json.loads(s,object_pairs_hook=pairs,parse_constant=lambda x: (_ for _ in ()).throw(ValueError(x)))
def uint(x):
    require(isinstance(x,str) and re.fullmatch(r'0|[1-9][0-9]*',x) is not None,'noncanonical uint')
    n=int(x);require(n<=U64,'uint overflow');return n

def validate(data,exit_status):
    require(exit_status.strip()=='0','process failed')
    require(data.endswith(b'\n'),'unterminated final line')
    rows=[parse(s) for s in data.splitlines()]
    require(len(rows)>=4,'short stream')
    counts=collections.Counter(r['record'] for r in rows)
    require(set(counts)<= {'begin','pool','vdev','bp','dnode','end'},'unknown record')
    require(all(counts[k]==1 for k in ('begin','pool','end')),'duplicate/missing envelope')
    begin,pool,end=rows[0],rows[1],rows[-1]
    require([begin['record'],pool['record'],end['record']]==['begin','pool','end'],'envelope ordering')
    require(begin['schema']=='zdb-observed-bp-v1' and begin['bp_encoding']=='host-normalized-u64-hex','schema')
    require(begin['source_base']=='da512a7e2e6716980271582e436faef06a575f71','base')
    require(end['export_complete'] is True and type(end['error']) is int and end['error']==0,'terminal failure')
    require(all(all(r.get(k) is False for k in FLAGS) for r in rows),'certification flag')
    vdevs={};events=[];started=False
    for r in rows[2:-1]:
        if r['record']=='vdev':
            require(not started,'late vdev');i=uint(r['id'])
            require(i not in vdevs,'duplicate vdev');vdevs[i]=r
        else:
            require(r['record'] in ('bp','dnode'),'unexpected middle record');started=True;events.append(r)
            require(uint(r['sequence'])==len(events),'sequence gap')
    require(len(events)==uint(end['observed'])<=uint(begin['record_limit']),'count mismatch')
    bps=[r for r in events if r['record']=='bp']
    require(all(type(r['unsupported']) is bool and type(r['children_skipped']) is bool for r in bps),'invalid bool')
    require(sum(r['unsupported'] for r in bps)==uint(end['unsupported']),'unsupported count')
    for r in bps:
        require(len(r['bp_words'])==16 and all(re.fullmatch('[0-9a-f]{16}',w) for w in r['bp_words']),'raw BP encoding')
    require(pool['root_bp']==bps[0]['bp_words'] and bps[0]['objset']=='0' and bps[0]['level']=='-1','MOS root binding')
    return rows,counts,pool,vdevs,bps

def analyze(data,exit_status):
    rows,counts,pool,vdevs,bps=validate(data,exit_status)
    intervals=[];unknown=[];dist=collections.Counter();metadata=collections.Counter()
    for r in bps:
        if r['unsupported'] or r['children_skipped'] or r['classification'] not in ('ordinary','hole','embedded'):
            unknown.append({'sequence':r['sequence'],'classification':r['classification']});continue
        if r['classification']!='ordinary':
            require(r['dvas']==[],'nonordinary DVAs');continue
        require(type(r['metadata']) is bool,'unknown metadata')
        w=list(map(lambda s:int(s,16),r['bp_words']));prop=w[6]
        require(not ((prop>>39)&1) and not ((prop>>61)&1) and not ((prop>>62)&1),'embedded/crypt/dedup ordinary')
        p=uint(r['psize']);l=uint(r['lsize'])
        require(p==(((prop>>16)&65535)+1)*512 and l==((prop&65535)+1)*512,'raw sizes')
        require(r['bp_type']==(prop>>48)&255 and r['bp_level']==(prop>>56)&31,'raw type/level')
        require(r['compression_id']==(prop>>32)&127 and r['checksum_id']==(prop>>40)&255,'raw algorithms')
        require(uint(r['logical_birth'])==w[10] and uint(r['physical_birth'])==(w[9] or w[10]),'raw birth')
        require(uint(r['logical_birth'])<=uint(pool['txg']) and uint(r['physical_birth'])<=uint(pool['txg']),'future birth')
        slots=[i for i in range(3) if (w[2*i]&0xffffff)!=0]
        require([d['slot'] for d in r['dvas']]==slots and slots,'DVA slot coverage')
        for d in r['dvas']:
            slot=d['slot'];a,b=w[2*slot:2*slot+2]
            vid=uint(d['vdev_id']);off=uint(d['offset']);size=uint(d['asize']);v=vdevs[vid]
            require(vid==a>>32 and off==(b&((1<<63)-1))*512 and size==(a&0xffffff)*512,'raw DVA')
            require(d['gang'] is False and b>>63==0,'gang interval forbidden')
            require(v['type']=='raidz' and v['allocation_class']==d['allocation_class']=='normal' and v['alloc_bias']==d['alloc_bias']==0 and v['is_log'] is d['is_log'] is False,'unsupported class')
            require(d['vdev_guid']==v['guid'] and d['ashift']==v['ashift'],'vdev binding')
            ashift=uint(v['ashift']);n=uint(v['children']);parity=uint(v['parity'])
            require(9<=ashift<=16 and 0<parity<n,'geometry dimensions')
            sec=1<<ashift;q=((p+sec-1)//sec)*sec;s=q//sec
            az=((s+parity*((s+n-parity-1)//(n-parity))+parity)//(parity+1))*(parity+1)*sec
            require(uint(d['qsize'])==q and uint(d['expected_asize'])==size==az and d['geometry_valid'] is True,'PQA geometry')
            require(off%sec==0 and off<=U64-size,'address arithmetic')
            # Original reference remains in input; source hash + line sequence + DVA slot is lossless locator.
            intervals.append(dict(vdev_id=str(vid),vdev_guid=v['guid'],start=str(off),end=str(off+size),length=str(size),P=str(p),Q=str(q),A=str(size),sequence=r['sequence'],slot=slot,objset=r['objset'],object=r['object'],level=r['level'],blkid=r['blkid'],metadata=r['metadata'],logical_birth=r['logical_birth'],physical_birth=r['physical_birth'],bp_words=r['bp_words'],**FLAGS))
            dist[(p,q,size)]+=1;metadata['metadata' if r['metadata'] else 'ordinary_data']+=1
    groups=collections.defaultdict(list)
    for x in intervals:groups[(x['vdev_id'],x['start'],x['end'])].append(x)
    conflicts=[];duplicates=[]
    for k,refs in groups.items():
        if len(refs)>1:
            # Conservative: differing raw context is a review finding, not proof of corruption.
            signatures={(tuple(x['bp_words']),x['P'],x['Q'],x['physical_birth'],x['metadata']) for x in refs}
            target=duplicates if len(signatures)==1 else conflicts
            target.append(dict(kind='exact_repeat' if len(signatures)==1 else 'descriptor_conflict',interval=k,references=[(x['sequence'],x['slot']) for x in refs]))
    per={};partial=[]
    for vid,v in vdevs.items():
        refs=[x for x in intervals if x['vdev_id']==str(vid)]
        unique=sorted((int(s),int(e)) for i,s,e in groups if i==str(vid));union=[];active=[]
        for s,e in unique:
            active=[old for old in active if old[1]>s]
            for old in active:partial.append(dict(vdev_id=str(vid),left=old,right=(s,e)))
            active.append((s,e))
            if union and s<=union[-1][1]:union[-1][1]=max(e,union[-1][1])
            else:union.append([s,e])
        per[str(vid)]=dict(guid=v['guid'],references=len(refs),unique_intervals=len(unique),reference_A_bytes=sum(int(x['A']) for x in refs),union_bytes=sum(e-s for s,e in union),coalesced_intervals=len(union),metadata_references=sum(x['metadata'] for x in refs),data_references=sum(not x['metadata'] for x in refs))
    summary=dict(schema='real-observation-interval-analysis-v1',input_sha256=hashlib.sha256(data).hexdigest(),pool=pool,record_counts=dict(counts),classification_counts=dict(collections.Counter(r['classification'] for r in bps)),interval_reference_count=len(intervals),unique_address_intervals=len(groups),exact_repeats=duplicates,descriptor_conflicts=conflicts,partial_overlaps=partial,unknowns=unknown,per_vdev=per,PQA_distribution=[dict(P=p,Q=q,A=a,references=n) for (p,q,a),n in sorted(dist.items())],metadata_dva_counts=dict(metadata),source_allocation_set_available=False,A_minus_observed=None,observed_minus_A=None,limitations=['Intervals use top-vdev DVA byte coordinates, not child-disk offsets.','vdev_asize is absent from v1; bound check relies on recorded exporter geometry validation.','Whole BP equality is a conservative duplicate compatibility rule.','No allocation-set input; coverage and differences cannot be computed.','Hash and local logs bind recorded evidence; they are not signed execution attestation.'],**FLAGS)
    return summary,intervals
