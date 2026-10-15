"""Validate bounded pending-free owner provenance; never infer movable payloads."""
import collections
from observed_intervals import require,uint,words,DMU_OT_NUMTYPES,DMU_BSWAP_NUMFUNCS
from paired_intervals import union,difference,digest
KINDS={'pending_scope','pending_object','pending_owner','pending_link','pending_queue_end','pending_end'}

def owner_refs(r,vdevs,txg):
    require(r['error']==0 and r['payload_verifiable'] is False and r['movable_candidate'] is False,'pending owner eligibility/claims')
    w=words(r['bp_words']);prop=w[6];typ=(prop>>48)&255
    valid=(typ&31)<DMU_BSWAP_NUMFUNCS if typ&128 else typ<DMU_OT_NUMTYPES
    require(valid and typ not in (42,43,49) and not(prop & ((1<<61)|(1<<62))),'unsupported pending raw layout')
    require(w[10]<=txg,'future pending birth')
    if prop & (1<<39):
        require(r['status']=='embedded','pending embedded classification')
        return []
    require(r['status']=='ordinary' and (w[0] or w[1]) and not any(w[i]>>63 for i in (1,3,5)),'pending hole/gang/classification')
    require((w[9] or w[10])<=txg,'future pending physical birth')
    p=(((prop>>16)&65535)+1)*512;refs=[]
    for slot in range(3):
        a,b=w[2*slot:2*slot+2];size=(a&0xffffff)*512
        if not size:
            require(a==b==0,'pending empty DVA has residual words');continue
        vid=str(a>>32);require(vid in vdevs,'pending unknown vdev');v=vdevs[vid]
        require(v['type']=='raidz' and v['allocation_class']=='normal' and v['alloc_bias']==0 and v['is_log'] is False,'pending unsupported class')
        ashift=uint(v['ashift']);n=uint(v['children']);parity=uint(v['parity'])
        require(9<=ashift<=16 and 0<parity<n,'pending geometry dimensions')
        sec=1<<ashift;s=(p+sec-1)//sec;q=s*sec
        expected=((s+parity*((s+n-parity-1)//(n-parity))+parity)//(parity+1))*(parity+1)*sec
        off=(b&((1<<63)-1))*512
        require(size==expected and off%sec==0 and off+size<=uint(v['asize']) and off+size<2**64,'pending PQA/bounds')
        refs.append(dict(queue=r['queue'],root_object=r['root_object'],object=r['object'],entry_index=r['entry_index'],slot=slot,vdev_id=vid,vdev_guid=v['guid'],start=str(off),length=str(size),end=str(off+size),stored_P=str(p),Q=str(q),A=str(size),logical_birth=str(w[10]),physical_birth=str(w[9] or w[10]),payload_verifiable=False,movable_candidate=False))
    require(refs,'pending ordinary has no extents')
    return refs

def analyze(rows,result):
    require(len(rows)>=5 and rows[0]['record']=='pending_scope' and rows[-1]['record']=='pending_end','pending envelope')
    scope=rows[0];app=uint(scope['spa_version'])>=26
    require(scope['dsl_free_applicable'] is app,'pending version applicability')
    roots={'spa_deferred':uint(scope['spa_deferred_root']),'dsl_free':uint(scope['dsl_free_root'])}
    require(roots['spa_deferred']>0 and (roots['dsl_free']>0 if app else roots['dsl_free']==0),'pending roots')
    ids=set();todo=[];objects=[];owners=[];links=[];refs=[];read_bytes=0;pos=1
    vdevs={v['id']:v for v in result['cut']['topology']}
    def register(obj):
        require(obj>0 and obj not in ids and len(ids)<4096,'pending object alias/cycle/limit');ids.add(obj)
    for q,root in roots.items():
        if root:register(root);todo.append((q,root,0,0,0,root))
    head=0
    while head<len(todo):
        q,obj,parent,index,depth,root=todo[head];head+=1
        require(pos<len(rows),'missing pending object');r=rows[pos];pos+=1
        require(r['record']=='pending_object','pending object order')
        require((r['queue'],uint(r['object']),uint(r['parent_object']),uint(r['parent_index']),uint(r['depth']),uint(r['root_object']))==(q,obj,parent,index,depth,root),'pending object provenance')
        count=uint(r['entries']);sub=uint(r['subobjects']);lst=uint(r['subobject_list']);bonus=uint(r['bonus_bytes'])
        require(depth<=64 and count<=100000-len(owners) and sub<=4096-len(todo),'pending bounds')
        require(bonus in (16,32,48) and (bonus==48 or (lst==sub==0)),'pending bonus layout')
        require(bonus>=32 or uint(r['accounted_comp'])==uint(r['accounted_uncomp'])==0,'legacy pending accounting layout')
        require(not sub or lst>0,'missing pending subobject list')
        if not count and not sub:require(uint(r['accounted_bytes'])==uint(r['accounted_comp'])==uint(r['accounted_uncomp'])==0,'nonzero empty pending accounting')
        if lst:register(lst)
        read_bytes+=bonus+count*128+sub*8;require(read_bytes<=32<<20,'pending byte cap')
        objects.append(r)
        for i in range(count):
            require(pos<len(rows),'missing pending owner');o=rows[pos];pos+=1
            require(o['record']=='pending_owner' and (o['queue'],uint(o['root_object']),uint(o['object']),uint(o['entry_index']))==(q,root,obj,i),'pending owner provenance/order')
            refs.extend(owner_refs(o,vdevs,uint(result['cut']['txg'])));owners.append(o)
        for i in range(sub):
            require(pos<len(rows),'missing pending link');link=rows[pos];pos+=1
            require(link['record']=='pending_link' and (link['queue'],uint(link['root_object']),uint(link['object']),uint(link['subobject_list']),uint(link['entry_index']))==(q,root,obj,lst,i),'pending link provenance/order')
            child=uint(link['child_object']);register(child);require(depth<64,'pending depth cap')
            todo.append((q,child,obj,i,depth+1,root));links.append(link)
    for q,root in roots.items():
        require(pos<len(rows),'missing pending queue end');end=rows[pos];pos+=1
        counts={k:sum(r['queue']==q for r in source) for k,source in [('objects',objects),('entries',owners),('links',links)]}
        status='not_applicable' if not root else 'enumerated' if counts['entries'] else 'empty'
        require(end['record']=='pending_queue_end' and end['queue']==q and uint(end['root_object'])==root and end['status']==status and end['error']==0,'pending queue terminal')
        require(all(uint(end[k])==n for k,n in counts.items()),'pending queue counts')
    require(pos==len(rows)-1,'extra pending records');end=rows[pos]
    require(end['pending_complete'] is True and end['error']==0,'pending incomplete/error')
    require(uint(end['objects'])==len(objects) and uint(end['entries'])==len(owners) and uint(end['links'])==len(links) and uint(end['read_bytes'])==read_bytes,'pending terminal counts')
    explained=[];residual=[];outside=[];live_overlap=[];owner_overlap=[]
    def emit(dest,vid,spans,**extra):
        dest.extend(dict(vdev_id=vid,vdev_guid=vdevs[vid]['guid'],start=str(s),length=str(e-s),**extra) for s,e in spans)
    for vid in vdevs:
        rr=[r for r in refs if r['vdev_id']==vid];pending=union([[uint(r['start']),uint(r['end'])] for r in rr])
        aa=union([[uint(r['start']),uint(r['start'])+uint(r['length'])] for r in result['allocation_records'] if r['vdev_id']==vid])
        live=union([[uint(r['start']),uint(r['end'])] for r in result['observed_interval_references'] if r['vdev_id']==vid])
        missing=difference(aa,live)
        emit(explained,vid,difference(missing,difference(missing,pending)))
        emit(residual,vid,difference(missing,pending))
        emit(outside,vid,difference(pending,aa))
        emit(live_overlap,vid,difference(pending,difference(pending,live)))
        events=collections.Counter()
        for r in rr:events[uint(r['start'])]+=1;events[uint(r['end'])]-=1
        active=0;previous=None
        for point,delta in sorted(events.items()):
            if previous is not None and previous<point and active>1:emit(owner_overlap,vid,[[previous,point]],references=active)
            active+=delta;previous=point
    return dict(pending_free_enumeration_complete=True,pending_owner_records=owners,pending_owner_records_sha256=digest(owners),pending_object_records=objects,pending_links=links,pending_interval_references=refs,pending_explained_A_minus_observed=explained,pending_residual_A_minus_observed=residual,pending_outside_A=outside,pending_vs_observed_overlaps=live_overlap,pending_owner_overlaps=owner_overlap,pending_accounting_note='Stored bpobj counters are preserved, not equated with physical A bytes; RAID-Z deflation differs. Pending-free records are not payload-verifiable or movable candidates.')
