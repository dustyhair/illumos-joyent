/* Pool-free production walker fixtures. Real ZFS layouts; fake DMU and ARC. */
#include "zdb_inventory.c"
static bptree_phys_t header;
static bptree_entry_phys_t entries[8];
static dmu_object_info_t info;
static dmu_buf_t bonus;
static objset_phys_t os;
static dnode_phys_t nodes[8];
static blkptr_t indirect[4];
static int generated, active=1, fail_read, sparse, tiny, fail_arc, size_bad;
static unsigned reads,arcs,releases,holds,destroys;
static arc_buf_t buffers[64];static int sizes[64];
boolean_t spa_feature_is_active(spa_t *s,spa_feature_t f) {(void)s;return(f==SPA_FEATURE_ASYNC_DESTROY&&active);}
int dmu_object_info(objset_t *o,uint64_t id,dmu_object_info_t *i){(void)o;(void)id;*i=info;return(0);}
int dmu_bonus_hold(objset_t *o,uint64_t id,void *tag,dmu_buf_t **db){(void)o;(void)id;(void)tag;bonus.db_data=&header;bonus.db_size=tiny?8:sizeof(header);*db=&bonus;holds++;return(0);}
void dmu_buf_rele(dmu_buf_t *db,void *tag){(void)db;(void)tag;releases++;}
int dmu_offset_next(objset_t *o,uint64_t id,boolean_t hole,uint64_t *off){(void)o;(void)id;(void)hole;if(sparse)*off+=512;return(0);}
int dmu_read(objset_t *o,uint64_t id,uint64_t off,uint64_t n,void *dst,uint32_t flags){
 (void)o;(void)id;assert(flags==DMU_READ_NO_PREFETCH);reads++;
 if(fail_read && reads==(unsigned)fail_read)return(EIO);
 assert(off/512==(off+n-1)/512);if(generated){
  for(uint64_t j=0;j<n;j++)((char *)dst)[j]=((char *)&entries[3])[(off+j)%sizeof(entries[3])];
  return(0);
 }
 assert(off+n<=sizeof(entries));
 memcpy(dst,(char *)entries+off,n);return(0);
}
int arc_read(zio_t *pio,spa_t *s,const blkptr_t *bp,arc_read_done_func_t *done,
    void *priv,zio_priority_t pri,int flags,arc_flags_t *af,const zbookmark_phys_t *z){
 (void)pio;(void)s;(void)done;(void)pri;(void)z;
 assert(flags==ZIO_FLAG_CANFAIL && *af==ARC_FLAG_WAIT);arcs++;
 if(fail_arc && arcs==(unsigned)fail_arc)return(EIO);
 assert(arcs<=64);arc_buf_t *b=&buffers[arcs-1];sizes[arcs-1]=BP_GET_LSIZE(bp);
 if(BP_GET_LEVEL(bp))b->b_data=indirect;
 else if(BP_GET_TYPE(bp)==DMU_OT_OBJSET)b->b_data=&os;
 else {assert(BP_GET_TYPE(bp)==DMU_OT_DNODE);b->b_data=nodes;}
 *(arc_buf_t **)priv=b;return(0);
}
int arc_buf_size(arc_buf_t *b){return(size_bad?1:sizes[b-buffers]);}
void arc_buf_destroy(arc_buf_t *b,void *tag){(void)b;(void)tag;destroys++;}
void dmu_buf_will_dirty(dmu_buf_t *db,dmu_tx_t *tx){(void)db;(void)tx;abort();}
int dmu_free_range(objset_t *o,uint64_t id,uint64_t off,uint64_t n,dmu_tx_t *tx){(void)o;(void)id;(void)off;(void)n;(void)tx;abort();return(EIO);}
zio_t *zio_read(zio_t *p,spa_t *s,const blkptr_t *bp,abd_t *a,uint64_t n,zio_done_func_t *d,void *priv,zio_priority_t pri,enum zio_flag flags,const zbookmark_phys_t *z){(void)p;(void)s;(void)bp;(void)a;(void)n;(void)d;(void)priv;(void)pri;(void)flags;(void)z;abort();return(NULL);}
static blkptr_t bp(unsigned type,unsigned level,unsigned n,uint64_t off){
 blkptr_t b={0};BP_SET_TYPE(&b,type);BP_SET_LEVEL(&b,level);BP_SET_LSIZE(&b,n);BP_SET_PSIZE(&b,n);BP_SET_CHECKSUM(&b,ZIO_CHECKSUM_FLETCHER_4);BP_SET_COMPRESS(&b,ZIO_COMPRESS_OFF);b.blk_birth=20;
 DVA_SET_ASIZE(&b.blk_dva[0],8192);DVA_SET_OFFSET(&b.blk_dva[0],off);return(b);
}
static void dn(dnode_phys_t *d,unsigned type,unsigned level,blkptr_t b){d->dn_type=type;d->dn_nblkptr=1;d->dn_nlevels=level;d->dn_indblkshift=9;d->dn_datablkszsec=1;d->dn_blkptr[0]=b;}
int main(int argc,char **argv){
 if(argc!=3)return(99);
 const char *mode=argv[1];spa_t spa={0};dsl_pool_t dp={0};vdev_t root={0},v={0};vdev_ops_t ops={0};vdev_t *kids[]={&v};
 strlcpy(ops.vdev_op_type,"raidz",sizeof(ops.vdev_op_type));v.vdev_ops=&ops;v.vdev_id=0;v.vdev_guid=7;v.vdev_ashift=12;v.vdev_children=3;v.vdev_nparity=1;v.vdev_asize=1<<20;root.vdev_children=1;root.vdev_child=kids;
 spa.spa_root_vdev=&root;spa.spa_mode=FREAD;spa.spa_trust_config=B_TRUE;spa.spa_ubsync.ub_txg=30;spa.spa_meta_objset=(objset_t *)(uintptr_t)1;spa.spa_dsl_pool=&dp;dp.dp_bptree_obj=9;
 header.bt_begin=3;header.bt_end=4;header.bt_bytes=8192;
 info.doi_type=info.doi_bonus_type=DMU_OTN_UINT64_METADATA;info.doi_bonus_size=sizeof(header);info.doi_data_block_size=512;info.doi_max_offset=sizeof(entries);
 entries[3].be_bp=bp(DMU_OT_OBJSET,0,4096,32768);entries[3].be_birth_txg=10;
 dn(&os.os_meta_dnode,DMU_OT_DNODE,2,bp(DMU_OT_DNODE,1,512,24576));indirect[0]=bp(DMU_OT_DNODE,0,4096,16384);
 os.os_meta_dnode.dn_datablkszsec=8;
 dn(&nodes[1],DMU_OT_PLAIN_FILE_CONTENTS,1,bp(DMU_OT_PLAIN_FILE_CONTENTS,0,4096,4096));
 if(!strcmp(mode,"inactive")){active=0;dp.dp_bptree_obj=0;}
 if(!strcmp(mode,"empty")){header.bt_begin=header.bt_end=4;header.bt_bytes=0;}
 if(!strcmp(mode,"sentinel"))entries[3].be_birth_txg=UINT64_MAX;
 if(!strcmp(mode,"resumed") || !strcmp(mode,"unresolved"))SET_BOOKMARK(&entries[3].be_zb,ZB_DESTROYED_OBJSET,!strcmp(mode,"resumed")?1:99,0,0);
 if(!strcmp(mode,"resume-later")){nodes[2]=nodes[1];DVA_SET_OFFSET(&nodes[2].dn_blkptr[0].blk_dva[0],57344);SET_BOOKMARK(&entries[3].be_zb,ZB_DESTROYED_OBJSET,2,0,0);}
 if(!strcmp(mode,"resume-parent"))SET_BOOKMARK(&entries[3].be_zb,ZB_DESTROYED_OBJSET,0,0,0);
 if(!strcmp(mode,"cutoff-equal"))nodes[1].dn_blkptr[0].blk_birth=10;
 if(!strcmp(mode,"root-cutoff-equal"))entries[3].be_bp.blk_birth=10;
 if(!strcmp(mode,"reversed"))header.bt_begin=5;
 if(!strcmp(mode,"overflow"))header.bt_end=UINT64_MAX;
 if(!strcmp(mode,"queue-exact")){generated=1;header.bt_begin=0;header.bt_end=4096;info.doi_max_offset=4096*sizeof(entries[3]);entries[3].be_birth_txg=UINT64_MAX;}
 if(!strcmp(mode,"queue-limit"))header.bt_end=header.bt_begin+4097;
 if(!strcmp(mode,"read-first"))fail_read=1;
 if(!strcmp(mode,"read-second"))fail_read=2;
 if(!strcmp(mode,"arc-error"))fail_arc=2;
 if(!strcmp(mode,"arc-size"))size_bad=1;
 if(!strcmp(mode,"sparse"))sparse=1;
 if(!strcmp(mode,"tiny-bonus"))tiny=1;
 if(!strcmp(mode,"wrong-type"))info.doi_type=DMU_OT_BPOBJ;
 if(!strcmp(mode,"extent"))info.doi_max_offset=600;
 if(!strcmp(mode,"future-cutoff"))entries[3].be_birth_txg=31;
 if(!strcmp(mode,"future-birth"))nodes[1].dn_blkptr[0].blk_birth=31;
 if(!strcmp(mode,"bad-resume"))SET_BOOKMARK(&entries[3].be_zb,1,1,1,0);
 if(!strcmp(mode,"large-dnode"))nodes[1].dn_extra_slots=1;
 if(!strcmp(mode,"bad-level"))nodes[1].dn_nlevels=0;
 if(!strcmp(mode,"bad-indirect"))os.os_meta_dnode.dn_indblkshift=8;
 if(!strcmp(mode,"cycle"))indirect[0]=os.os_meta_dnode.dn_blkptr[0];
 if(!strcmp(mode,"ancestor-overlap"))DVA_SET_OFFSET(&indirect[0].blk_dva[0],24576);
 if(!strcmp(mode,"compression"))BP_SET_COMPRESS(&entries[3].be_bp,127);
 if(!strcmp(mode,"compression-inherit"))BP_SET_COMPRESS(&entries[3].be_bp,ZIO_COMPRESS_INHERIT);
 if(!strcmp(mode,"checksum-inherit"))BP_SET_CHECKSUM(&entries[3].be_bp,ZIO_CHECKSUM_INHERIT);
 if(!strcmp(mode,"zero-birth"))entries[3].be_bp.blk_birth=0;
 if(!strcmp(mode,"writable"))spa.spa_mode=FREAD|FWRITE;
 if(!strcmp(mode,"inactive-object"))active=0;
 bptree_phys_t saved=header;bptree_entry_phys_t before[8];memcpy(before,entries,sizeof(entries));
 zi_out=fopen(argv[2],"w");if(!zi_out)return(98);
 int e;
 if(!strncmp(mode,"budget-",7)) {
  zb_ctx_t c={0};c.spa=&spa;c.object=9;c.cutoff=10;zbookmark_phys_t z;SET_BOOKMARK(&z,ZB_DESTROYED_OBJSET,0,-1,0);
  if(!strcmp(mode,"budget-reads"))c.reads=ZB_READ_LIMIT;
  if(!strcmp(mode,"budget-bytes"))c.bytes=ZB_BYTE_LIMIT;
  if(!strcmp(mode,"budget-visits"))c.visits=ZB_VISIT_LIMIT;
  if(!strcmp(mode,"budget-visits-exact")){
   c.visits=ZB_VISIT_LIMIT-1;
   e=zb_visit(&c,NULL,&nodes[1].dn_blkptr[0],&z,0);assert(e==0 && c.visits==ZB_VISIT_LIMIT && c.owners==1);
   e=zb_visit(&c,NULL,&nodes[1].dn_blkptr[0],&z,0);assert(e==E2BIG && arcs==0);
   goto finished;
  }
  if(!strcmp(mode,"budget-exact"))c.bytes=ZB_BYTE_LIMIT-4096;
  if(!strcmp(mode,"budget-read-exact"))c.reads=ZB_READ_LIMIT-1;
  if(!strcmp(mode,"budget-cycle"))c.ancestors[0]=entries[3].be_bp;
  e=zb_visit(&c,NULL,&entries[3].be_bp,&z,!strcmp(mode,"budget-depth")?64:!strcmp(mode,"budget-depth-exact")?63:!strcmp(mode,"budget-cycle")?1:0);
 }else e=zb_run(&spa);
finished:
 assert(!memcmp(&saved,&header,sizeof(header)));assert(!memcmp(before,entries,sizeof(entries)));assert(holds==releases);assert(destroys==arcs-(fail_arc?1:0));
 fclose(zi_out);printf("error=%d queue_reads=%u metadata_reads=%u bonus_balanced=%u buffers_released=%u no_mutation=1\n",e,reads,arcs,holds,destroys);return(e?1:0);
}
