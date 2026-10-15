#!/usr/bin/env python3
"""Read-only parent-edge graph validation. ARC authentication is producer evidence."""
import argparse,hashlib,json,pathlib
from observed_intervals import parse,require,uint,sint,words,FLAGS,DMU_OT_NUMTYPES,DMU_BSWAP_NUMFUNCS
from paired_intervals import canonical
from profile_intervals import analyze as profile
KINDS={'parent_scope','parent_edge','parent_container','parent_end'}
SAFETY=dict(FLAGS,pointer_rewrite_ready=False,placement_performed=False,reverse_references_complete=False)

def valid_type(t):return (t&31)<DMU_BSWAP_NUMFUNCS if t&128 else t<DMU_OT_NUMTYPES

def raw(w):return b''.join(x.to_bytes(8,'little') for x in words(w))
def bpwords(data):return [f'{int.from_bytes(data[i:i+8],"little"):016x}' for i in range(0,128,8)]
def bookmark(z):
    require(type(z) is list and len(z)==4,'bookmark');uint(z[0]);uint(z[1]);sint(z[2]);uint(z[3]);return z

def metadata(rows,root_bp,datasets,verified_words=None,dataset_records=None):
    require(rows and rows[0]['record']=='parent_scope' and rows[-1]['record']=='parent_end','parent envelope')
    scope,end=rows[0],rows[-1]
    require(scope['encoding']=='decoded-amd64-little-endian' and scope['snapshot_birth_pruning'] is False,'decoded encoding/pruning')
    require(scope['scope']=='current_MOS_and_dataset_tree_pointer_slots' and scope['semantic_BP_payloads_enumerated'] is scope['historical_roots_enumerated'] is False,'parent scope limit')
    require(scope['anchor']=='selected_import_uberblock_not_independent_label_proof','anchor qualification')
    require(end['parent_capture_complete'] is True and type(end['error']) is int and end['error']==0,'incomplete parent capture')
    fields={
        'parent_scope':{'encoding','snapshot_birth_pruning','anchor','scope','semantic_BP_payloads_enumerated','historical_roots_enumerated'},
        'parent_edge':{'node','parent_node','kind','decoded_offset','dnode_offset','slot','bookmark','child_bp','parent_bp','parent_bookmark','disposition'},
        'parent_container':{'node','decoded_size','authentication','decoded_hex'},
        'parent_end':{'parent_capture_complete','error','nodes','containers','decoded_bytes','output_charge','dataset_roots'}}
    for r in rows:
        require(set(r)==fields.get(r['record'],set())|set(SAFETY)|{'record'},'parent record schema')
        require(all(r.get(k) is False for k in SAFETY),'parent safety flag')
    nodes={};containers={};expected={};actual={};roots={};decoded_total=0
    for r in rows[1:-1]:
        kind=r['record'];node=uint(r['node'])
        if kind=='parent_edge':
            require(node==len(nodes)+1 and node<=100000,'node order/budget')
            z=bookmark(r['bookmark']);bp=words(r['child_bp']);parent=uint(r['parent_node'])
            offset=uint(r['decoded_offset']);base=uint(r['dnode_offset']);slot=uint(r['slot'])
            if parent==0:
                require(node==1 and r['kind']=='selected_uberblock' and r['child_bp']==root_bp and z==['0','0','-1','0'],'MOS anchor')
                require(r['parent_bp'] is None and r['parent_bookmark'] is None and offset==base==slot==0,'MOS anchor fields')
            else:
                require(parent in containers and parent<node,'missing authenticated parent container')
                require(r['parent_bp']==nodes[parent]['child_bp'] and r['parent_bookmark']==nodes[parent]['bookmark'],'parent identity')
                data=containers[parent];require(offset%8==0 and offset+128<=len(data),'decoded slot bounds')
                require(data[offset:offset+128]==raw(r['child_bp']),'child byte membership')
                key=(parent,offset);require(key in expected and key not in actual,'unexpected/duplicate physical parent slot observation')
                k,b,s,bm=expected[key];require((r['kind'],base,slot,z)==(k,b,s,bm),'slot layout/bookmark')
                actual[key]=node
                if r['kind']=='dataset_bonus':
                    require(z[0] not in roots,'duplicate dataset root');roots[z[0]]=r['child_bp']
                    if dataset_records is not None:
                        require(z[0] in dataset_records and offset>=128 and offset-128+320<=len(data),'dataset bonus provenance')
                        ds=dataset_records[z[0]];bonus=data[offset-128:offset-128+320]
                        u=lambda i:int.from_bytes(bonus[8*i:8*i+8],'little')
                        for key,i in [('dir_object',0),('prev_snap_object',1),('prev_snap_txg',2),('next_snap_object',3),('snapnames_object',4),('children',5),('creation_txg',7),('dataset_flags',15),('next_clones_object',32),('props_object',33),('userrefs_object',34)]:
                            require(uint(ds[key])==u(i),'profile/MOS bonus disagreement: '+key)
                        require([uint(x) for x in ds['space_bytes']]==[u(i) for i in range(9,13)],'profile/MOS space disagreement')
            embedded=bool(bp[6]&(1<<39));hole=not embedded and all((bp[i]&0xffffff)==0 for i in (0,2,4))
            typ=(bp[6]>>48)&255;level=(bp[6]>>56)&31
            require(valid_type(typ) and typ not in (42,43,49) and not bp[6]&((1<<61)|(1<<62)) and level<=16,'unsupported edge BP')
            require(not embedded or (not level and typ not in (10,11)),'embedded pointer-bearing metadata')
            disposition='hole' if hole else 'embedded' if embedded else 'container' if level or typ in (10,11) else 'leaf'
            require(r['disposition']==disposition,'edge disposition')
            if not hole and not embedded and verified_words is not None:require(tuple(r['child_bp']) in verified_words,'edge BP lacks accepted original-BP verification')
            nodes[node]=r
        elif kind=='parent_container':
            require(node in nodes and node not in containers and node==len(nodes) and nodes[node]['disposition']=='container','container ordering')
            require(r['authentication']=='original_bp_arc_checksum','parent authentication')
            n=uint(r['decoded_size']);require(n<=16<<20 and n>0 and len(r['decoded_hex'])==2*n,'decoded size')
            require(all(c in '0123456789abcdef' for c in r['decoded_hex']),'decoded hex')
            data=bytes.fromhex(r['decoded_hex']);containers[node]=data;decoded_total+=n
            require(len(containers)<=100000 and decoded_total<=64<<20,'parent read budget')
            edge=nodes[node];w=words(edge['child_bp']);typ=(w[6]>>48)&255;level=(w[6]>>56)&31;z=edge['bookmark']
            require(n==((w[6]&65535)+1)*512,'BP decoded size')
            def add(off,k,base,slot,bm):
                require((node,off) not in expected and off+128<=n,'overlapping/invalid pointer slot');expected[node,off]=(k,base,slot,bm)
            def dnode(base,obj):
                require(base+512<=n,'dnode bounds');d=data[base:base+512]
                t,shift,levels,count,bonus=d[:5];flags=d[7];bonuslen=int.from_bytes(d[10:12],'little')
                require(valid_type(t) and valid_type(bonus) and d[12]==0 and count<=3 and levels<=16 and flags&~15==0,'unsupported dnode')
                b=64+count*128;end=512-(128 if flags&4 else 0);require(b<=end and bonuslen<=end-b,'bonus bounds')
                if t==0:
                    require(count==bonuslen==bonus==flags==0,'unused dnode state');return
                require(not count or (levels and 7<=shift<=24 and int.from_bytes(d[8:10],'little')),'dnode tree layout')
                for i in range(count):add(base+64+i*128,'dnode',base,i,[z[0],str(obj),str(levels-1),str(i)])
                if flags&4:add(base+384,'spill',base,0,[z[0],str(obj),'0',str((1<<64)-2)])
                if bonus==16:
                    require(z[0]=='0' and obj and t in (16,196) and bonuslen==320,'dataset bonus layout')
                    add(base+b+128,'dataset_bonus',0,0,[str(obj),'0','-1','0'])
                else:require(t!=16,'dataset bonus missing')
            if level:
                require(n%128==0,'indirect layout');count=n//128
                for i in range(count):add(i*128,'indirect',0,i,[z[0],z[1],str(int(z[2])-1),str(uint(z[3])*count+i)])
            elif typ==10:
                require(n%512==0,'dnode block size')
                for i in range(n//512):dnode(i*512,uint(z[3])*(n//512)+i)
            else:
                require(typ==11 and n in (1024,2048,4096),'objset size');dnode(0,0)
                if n>=2048:dnode(1024,(1<<64)-1);dnode(1536,(1<<64)-2)
                if n>=4096:dnode(2048,(1<<64)-3)
        else:raise ValueError('unexpected parent record')
    require(set(expected)==set(actual),'missing inbound edge/slot')
    require({i for i,r in nodes.items() if r['disposition']=='container'}==set(containers),'missing decoded container')
    require(roots==datasets and roots,'dataset-root coverage')
    require(uint(end['nodes'])==len(nodes) and uint(end['containers'])==len(containers) and uint(end['decoded_bytes'])==decoded_total and uint(end['dataset_roots'])==len(roots),'parent counters')
    require(uint(end['output_charge'])==4096*len(nodes)+2048*len(containers)+2*decoded_total<=512<<20,'parent output budget')
    return dict(nodes=len(nodes),containers=len(containers),decoded_bytes=decoded_total,dataset_roots=len(roots),parent_edges=rows,**SAFETY)

def analyze(data,status,sha):
    require(status.strip()=='0' and hashlib.sha256(data).hexdigest()==sha and data.endswith(b'\n'),'parent process/hash/truncation')
    rows=[parse(s) for s in data.splitlines()];require(rows[0]['schema']=='zdb-parent-edges-v1','parent schema')
    indices=[i for i,r in enumerate(rows) if r['record'] in KINDS];cut=next(i for i,r in enumerate(rows) if r['record']=='cut_end')
    require(indices and indices==list(range(indices[0],cut)),'parent section order')
    base=[dict(r) for r in rows if r['record'] not in KINDS];base[0]['schema']='zdb-observed-profile-v1'
    projected=b''.join(canonical(r)+b'\n' for r in base);accepted=profile(projected,status,hashlib.sha256(projected).hexdigest())
    datasets={r['object']:r['root_bp'] for r in accepted['profile_metadata_records'] if r['record']=='profile_dataset'}
    result=metadata([rows[i] for i in indices],accepted['cut']['root_bp'],datasets,{tuple(r['bp_words']) for r in accepted['verification_results'] if r['status']=='verified'},{r['object']:r for r in accepted['profile_metadata_records'] if r['record']=='profile_dataset'})
    result.update(schema='parent-edge-analysis-v1',input_sha256=sha,cut=accepted['cut'],parent_evidence_validated=True,limits=['Decoded offsets are not physical disk addresses.','ARC checksum authentication and selected-uberblock identity are producer evidence, not independent label/raw-byte proof.','Current supported-profile roots only; no exhaustive historical root, COW, allocation or recovery proof.'])
    return result

def main():
    p=argparse.ArgumentParser();p.add_argument('capture',type=pathlib.Path);p.add_argument('--sha256',required=True);p.add_argument('--exit-status',type=pathlib.Path,required=True);p.add_argument('--output',type=pathlib.Path,required=True);a=p.parse_args()
    from relocation_demand import read_regular
    r=analyze(read_regular(a.capture,512<<20),read_regular(a.exit_status,64).decode('ascii'),a.sha256)
    with a.output.open('x') as f:json.dump(r,f,indent=2);f.write('\n')
if __name__=='__main__':main()
