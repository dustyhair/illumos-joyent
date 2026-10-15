"""Destroyed-tree owner provenance validation; never a relocation certificate."""
import collections
from observed_intervals import require,uint,sint,words
from pending_intervals import owner_refs
from paired_intervals import union,difference,digest
KINDS={'async_scope','async_queue','async_entry','async_owner','async_end'}
DESTROYED=2**64-1

def bookmark(z):
    require(len(z)==4 and uint(z[0])==DESTROYED,'destroyed bookmark objset')
    return uint(z[1]),sint(z[2]),uint(z[3])

def analyze(rows,result):
    require(len(rows)>=2 and rows[0]['record']=='async_scope' and rows[-1]['record']=='async_end','async envelope')
    scope,end=rows[0],rows[-1];obj=uint(scope['object']);active=scope['active']
    require(active is (obj!=0),'async applicability')
    require(end['async_complete'] is True and end['error']==0 and uint(end['object'])==obj,'async terminal')
    entries=[];owners=[];refs=[];pos=1;visits=uint(end['visits']);reads=uint(end['reads']);nbytes=uint(end['read_bytes'])
    require(visits<=100000 and reads<=100000 and nbytes<=64<<20,'async budgets')
    vdevs={v['id']:v for v in result['cut']['topology']};txg=uint(result['cut']['txg'])
    if active:
        require(pos<len(rows)-1 and rows[pos]['record']=='async_queue','async queue missing');q=rows[pos];pos+=1
        begin,stop=uint(q['begin']),uint(q['end'])
        require(uint(q['object'])==obj and begin<=stop and stop<=(2**64-1)//168 and stop-begin<=4096,'async queue boundaries')
        if begin==stop:require(all(uint(q[k])==0 for k in ('accounted_bytes','accounted_comp','accounted_uncomp')),'empty accounting')
        for index in range(begin,stop):
            require(pos<len(rows)-1 and rows[pos]['record']=='async_entry','missing async entry');r=rows[pos];pos+=1
            require(uint(r['object'])==obj and uint(r['entry_index'])==index,'async entry provenance/order')
            cutoff=uint(r['birth_cutoff']);done=cutoff==2**64-1
            require(r['completed_sentinel'] is done,'sentinel mismatch');words(r['root_bp']);entries.append(r)
            if done:continue
            require(cutoff<=txg,'future cutoff')
            if r['resume']!=['0']*4:
                o,l,b=bookmark(r['resume']);require(l==0 and b!=2**64-1,'unsupported resume')
            seen=set()
            while pos<len(rows)-1 and rows[pos]['record']=='async_owner':
                o=rows[pos];pos+=1
                require(uint(o['object'])==obj and uint(o['entry_index'])==index and o['queue']=='async_destroy','async owner provenance')
                key=tuple(o['bookmark']);bo,bl,bi=bookmark(o['bookmark'])
                require(key not in seen and uint(o['depth'])<64,'duplicate bookmark/depth');seen.add(key)
                w=words(o['bp_words']);require(w[10]>cutoff,'async owner birth cutoff')
                if bl==-1:
                    require(bo==bi==0 and uint(o['depth'])==0 and o['bp_words']==r['root_bp'],'async root identity')
                else:
                    require(0<=bl<=16 and uint(o['depth'])>0 and bl==((w[6]>>56)&31),'async bookmark level')
                projected=dict(o,root_object=str(obj))
                rr=owner_refs(projected,vdevs,txg)
                for ref in rr:ref.update(bookmark=o['bookmark'],depth=o['depth'],birth_cutoff=r['birth_cutoff'],resume=r['resume'])
                refs.extend(rr);owners.append(o)
        require(uint(end['entry_index'])==stop,'async final index')
        require(nbytes>=40+168*len(entries) and reads>=2+2*len(entries),'async read accounting lower bound')
    else:
        require(uint(end['entry_index'])==visits==reads==nbytes==0,'inactive counters')
    require(pos==len(rows)-1 and uint(end['entries'])==len(entries) and uint(end['owners'])==len(owners) and visits>=len(owners),'async terminal counts/order')
    findings={k:[] for k in ('async_outside_A','async_vs_observed_overlaps','async_vs_pending_overlaps','async_owner_overlaps','owners_residual_A_minus_observed')}
    for vid,v in vdevs.items():
        def spans(rs):return union([(uint(r['start']),uint(r['start'])+uint(r['length'])) for r in rs if r['vdev_id']==vid])
        aa=spans(result['allocation_records']);live=spans(result['observed_interval_references']);pending=spans(result['pending_interval_references']);own=spans(refs)
        def emit(k,ss,**extra):findings[k].extend(dict(vdev_id=vid,vdev_guid=v['guid'],start=str(s),length=str(e-s),**extra) for s,e in ss)
        emit('async_outside_A',difference(own,aa));emit('async_vs_observed_overlaps',difference(own,difference(own,live)))
        emit('async_vs_pending_overlaps',difference(own,difference(own,pending)))
        emit('owners_residual_A_minus_observed',difference(difference(difference(aa,live),pending),own))
        events=collections.Counter()
        for r in refs:
            if r['vdev_id']==vid:events[uint(r['start'])]+=1;events[uint(r['end'])]-=1
        previous=None;count=0
        for point,delta in sorted(events.items()):
            if previous is not None and point>previous and count>1:emit('async_owner_overlaps',[(previous,point)],references=count)
            previous=point;count+=delta
    return dict(async_destroy_enumeration_complete=True,async_entry_records=entries,async_owner_records=owners,async_owner_records_sha256=digest(owners),async_interval_references=refs,**findings)
