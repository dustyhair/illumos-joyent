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

# Constants are pinned to da512a7e's sys/dmu.h; native tests check these values.
DMU_OT_NUMTYPES=54
DMU_BSWAP_NUMFUNCS=10

def sint(x):
    require(isinstance(x,str) and re.fullmatch(r'0|-?[1-9][0-9]*',x) is not None,'noncanonical int64')
    n=int(x);require(-(1<<63)<=n<(1<<63),'int64 overflow');return n

def words(x):
    require(isinstance(x,list) and len(x)==16 and all(isinstance(w,str) and re.fullmatch('[0-9a-f]{16}',w) for w in x),'raw BP encoding')
    return [int(w,16) for w in x]

# All integer/provenance fields, including those on records not used in arithmetic,
# pass here before cut identity, classification or interval construction.
UINT_FIELDS=set('next_snap_object snapnames_object dataset_flags directory_entries datasets heads snapshots cached_ddt_slots begin end birth_cutoff owners visits reads guid txg id ashift children parity asize metaslab_count sequence objset object blkid logical_birth physical_birth lsize psize embedded_payload_bytes record_limit observed unsupported_count vdev_id vdev_guid offset qsize expected_asize metaslab start length unflushed_txg map_object log_txg ordinal records intervals bytes map_words map_bytes root_object parent_object parent_index entry_index child_object depth entries objects links subobject_list subobjects bonus_bytes accounted_bytes accounted_comp accounted_uncomp read_bytes active_object active_index spa_version spa_deferred_root dsl_free_root requested_p completed_p verified_p seen submitted completed passed failed not_applicable outstanding'.split())
INT_FIELDS={'level'}
NUMBER_LIMITS={'object_type':255,'bonus_type':255,'bp_type':255,'bp_level':31,'checksum_id':255,'compression_id':127,'dnode_type':255,'dnode_indblkshift':255,'dnode_datablkszsec':65535,'slot':2,'alloc_bias':(1<<32)-1,'shift':63,'error':(1<<31)-1,'time_limit_seconds':(1<<31)-1}
BOOL_FIELDS=set('snapshot ddt_enumerated zil_enumerated profile_metadata_complete ddt_absence_checked active completed_sentinel async_complete coverage_complete payload_validation_performed evacuation_authorized dnode_present children_skipped geometry_valid gang is_log export_complete checkpoint_unsupported removal_unsupported applied measured verification_complete pending_complete dsl_free_applicable payload_verifiable movable_candidate'.split())
REQUIRED={
 'begin':{'schema','bp_encoding','source_base','record_limit'},
 'pool':{'guid','txg','root_bp'},
 'vdev':{'id','guid','ashift','children','parity','type','allocation_class','alloc_bias','is_log'},
 'bp':{'sequence','objset','object','level','blkid','bp_words','classification','bp_type','bp_level','logical_birth','physical_birth','lsize','psize','checksum_id','compression_id','metadata','dvas','unsupported','children_skipped'},
 'dnode':{'sequence','objset','object','dnode_type'},
 'end':{'export_complete','error','observed','unsupported'},
 'allocation_scope':{'checkpoint_unsupported','removal_unsupported','owner_obligations','allocation_adjustments'},
 'metaslab':{'vdev_id','id','start','length','unflushed_txg','map_object'},
 'allocation_map':{'source','map_object','log_txg','length','shift'},
 'allocation_op':{'source','map_object','log_txg','ordinal','vdev_id','metaslab','start','length','op','applied'},
 'allocation':{'vdev_id','vdev_guid','metaslab','start','length'},
 'cut_end':{'txg','root_bp'},
 'allocation_end':{'records','intervals','bytes','map_words','map_bytes'},
 'verification':{'sequence','status','measured','error','requested_p','completed_p','objset','object','level','blkid','bp_words'},
 'verification_end':{'verification_complete','seen','submitted','completed','passed','failed','unsupported','not_applicable','outstanding','requested_p','verified_p'},
 'profile_scope':{'profile','ddt_enumerated','zil_enumerated'},
 'profile_directory':{'name'},
 'profile_object':{'object','object_type','bonus_type'},
 'profile_dataset':{'object','snapshot','children','dataset_flags','next_snap_object','snapnames_object','zil_state','root_bp','zil_header_words'},
 'profile_end':{'profile_metadata_complete','error','objects','directory_entries','datasets','heads','snapshots','reads','read_bytes','active_object','cached_ddt_slots','ddt_absence_checked'},
 'async_scope':{'object','active'},
 'async_queue':{'object','begin','end','accounted_bytes','accounted_comp','accounted_uncomp'},
 'async_entry':{'object','entry_index','birth_cutoff','completed_sentinel','resume','root_bp'},
 'async_owner':{'object','entry_index','queue','depth','status','error','payload_verifiable','movable_candidate','bookmark','bp_words'},
 'async_end':{'object','entry_index','entries','owners','visits','reads','read_bytes','async_complete','error'},
 'pending_scope':{'spa_version','spa_deferred_root','dsl_free_root','dsl_free_applicable'},
 'pending_object':{'queue','root_object','object','parent_object','parent_index','depth','entries','subobject_list','subobjects','bonus_bytes','accounted_bytes','accounted_comp','accounted_uncomp'},
 'pending_owner':{'queue','root_object','object','entry_index','status','error','payload_verifiable','movable_candidate','bp_words'},
 'pending_link':{'queue','root_object','object','subobject_list','entry_index','child_object'},
 'pending_queue_end':{'queue','root_object','status','error','objects','entries','links'},
 'pending_end':{'pending_complete','error','objects','entries','links','read_bytes','active_object','active_index'},
 'dva':{'slot','vdev_id','offset','asize','gang','geometry_valid'},
}
TEXT_FIELDS=set('profile zil_state schema bp_encoding source_base integer_encoding quiescence name type allocation_class classification source op allocation_adjustments status queue'.split())
LIST_FIELDS={'attempted','excluded','owner_obligations'}
def record_schema(r,kind=None):
    require(type(r) is dict,'record must be object')
    kind=kind or r.get('record');require(kind in REQUIRED,'unknown record kind')
    require(REQUIRED[kind]<=r.keys(),'missing '+str(kind)+' fields')
    for key,value in r.items():
        if key=='record':require(value==kind,'record kind mismatch')
        elif key=='unsupported':
            if kind in ('end','verification_end'):uint(value)
            else:require(type(value) is bool,'invalid unsupported flag')
        elif key=='zil_header_words':
            require(value is None or (isinstance(value,list) and len(value)==24 and all(isinstance(w,str) and re.fullmatch('[0-9a-f]{16}',w) for w in value)),'ZIL header encoding')
        elif key in ('bookmark','resume'):
            require(type(value) is list and len(value)==4,'bookmark encoding')
            uint(value[0]);uint(value[1]);sint(value[2]);uint(value[3])
        elif key in UINT_FIELDS:uint(value)
        elif key in INT_FIELDS:sint(value)
        elif key in NUMBER_LIMITS:require(type(value) is int and 0<=value<=NUMBER_LIMITS[key],'invalid '+key)
        elif key in BOOL_FIELDS:require(type(value) is bool,'invalid '+key)
        elif key=='metadata':require(value is None or type(value) is bool,'invalid metadata')
        elif key in ('root_bp','bp_words'):words(value)
        elif key=='dvas':
            require(type(value) is list and len(value)<=3,'invalid DVAs')
            for d in value:record_schema(d,'dva')
        elif key in TEXT_FIELDS:require(type(value) is str,'invalid text '+key)
        elif key in LIST_FIELDS:require(type(value) is list and all(type(v) is str for v in value),'invalid list '+key)
        else:raise ValueError('unknown field '+key)
    if kind in ('pool','vdev'):require(uint(r['guid'])>0,'zero GUID')
    if 'vdev_guid' in r:require(uint(r['vdev_guid'])>0,'zero vdev GUID')

def raw_classification(r):
    w=words(r['bp_words']);prop=w[6];typ=(prop>>48)&255
    embedded=bool((prop>>39)&1);crypt=bool((prop>>61)&1)
    valid=(typ&31)<DMU_BSWAP_NUMFUNCS if typ&128 else typ<DMU_OT_NUMTYPES
    hole=not embedded and w[0]==w[1]==0
    gang=not embedded and any(w[i]>>63 for i in (1,3,5))
    kind=('type_unsupported' if not valid else 'protected_unsupported' if crypt else
          'embedded' if embedded else 'hole' if hole else 'gang_unsupported' if gang else
          'dedup_unsupported' if (prop>>62)&1 else 'ordinary')
    require(r['classification']==kind,'classification contradicts raw BP')
    require(r['bp_type']==typ and r['bp_level']==((prop>>56)&31),'raw type/level')
    require(r['compression_id']==((prop>>32)&127),'raw compression')
    require(r['checksum_id']==(2 if embedded else (prop>>40)&255),'raw checksum')
    require(uint(r['logical_birth'])==w[10],'raw logical birth')
    require(uint(r['physical_birth'])==(0 if embedded else (w[9] or w[10])),'raw physical birth')
    require(uint(r['psize'])==(0 if embedded else (((prop>>16)&65535)+1)*512),'raw psize')
    logical=((prop&((1<<25)-1))+1 if ((prop>>40)&255)==0 else 0) if embedded else ((prop&65535)+1)*512
    require(uint(r['lsize'])==logical,'raw lsize')
    if 'embedded_payload_bytes' in r:require(uint(r['embedded_payload_bytes'])==(((prop>>25)&127)+1 if embedded else 0),'raw embedded size')
    if not valid:require(r['metadata'] is None,'invalid type metadata')
    else:require(type(r['metadata']) is bool,'valid type missing metadata')
    if kind in ('type_unsupported','protected_unsupported','gang_unsupported','dedup_unsupported'):
        require(r['unsupported'] is True and r['children_skipped'] is True,'unsupported raw layout admitted')
    if kind in ('hole','embedded'):
        require(r['unsupported'] is False and r['children_skipped'] is False,'nonallocated classification flags')
        require(r['dvas']==[],'nonallocated DVAs')
    if hole and valid and not crypt:
        require(all(x==0 for x in w[:6]),'hole contains secondary DVA words')
    if not valid or crypt or embedded or hole:require(r['dvas']==[],'uninterpretable DVAs')
    return kind

def validate(data,exit_status):
    require(exit_status.strip()=='0','process failed')
    require(data.endswith(b'\n'),'unterminated final line')
    rows=[parse(s) for s in data.splitlines()]
    for r in rows:record_schema(r)
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
            require(i not in vdevs,'duplicate vdev')
            require(all(v['guid']!=r['guid'] for v in vdevs.values()),'duplicate vdev GUID');vdevs[i]=r
        else:
            require(r['record'] in ('bp','dnode'),'unexpected middle record');started=True;events.append(r)
            require(uint(r['sequence'])==len(events),'sequence gap')
    require(len(events)==uint(end['observed'])<=uint(begin['record_limit']),'count mismatch')
    bps=[r for r in events if r['record']=='bp']
    require(all(type(r['unsupported']) is bool and type(r['children_skipped']) is bool for r in bps),'invalid bool')
    require(sum(r['unsupported'] for r in bps)==uint(end['unsupported']),'unsupported count')
    for r in bps:raw_classification(r)
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
