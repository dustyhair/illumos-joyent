/* Pool-free real-layout parent-edge fixtures; no pool/device operations. */
#include "zdb_inventory.c"
static objset_phys_t mos,os;
static dnode_phys_t mosnodes[4],nodes[2];
static blkptr_t indirect[4];
static arc_buf_t buffers[64];static int sizes[64];
static unsigned reads,destroys;static int fail,sizebad;
static blkptr_t bp(unsigned type,unsigned level,unsigned n,uint64_t off) {
 blkptr_t b={0};BP_SET_TYPE(&b,type);BP_SET_LEVEL(&b,level);BP_SET_LSIZE(&b,n);BP_SET_PSIZE(&b,n);
 BP_SET_CHECKSUM(&b,ZIO_CHECKSUM_FLETCHER_4);BP_SET_COMPRESS(&b,ZIO_COMPRESS_OFF);b.blk_birth=20;
 DVA_SET_ASIZE(&b.blk_dva[0],8192);DVA_SET_OFFSET(&b.blk_dva[0],off);return(b);
}
static void dn(dnode_phys_t *d,unsigned type,blkptr_t b) {
 d->dn_type=type;d->dn_nblkptr=1;d->dn_nlevels=BP_GET_LEVEL(&b)+1;
 d->dn_indblkshift=9;d->dn_datablkszsec=BP_GET_LSIZE(&b)/512;d->dn_blkptr[0]=b;
}
int arc_read(zio_t *pio,spa_t *s,const blkptr_t *b,arc_read_done_func_t *done,void *priv,zio_priority_t pri,int flags,arc_flags_t *af,const zbookmark_phys_t *z) {
 (void)pio;(void)s;(void)done;(void)pri;(void)z;assert(flags==ZIO_FLAG_CANFAIL && *af==ARC_FLAG_WAIT);
 reads++;if(fail && reads==(unsigned)fail)return(ECKSUM);assert(reads<=64);
 arc_buf_t *buf=&buffers[reads-1];sizes[reads-1]=BP_GET_LSIZE(b);
 switch(DVA_GET_OFFSET(&b->blk_dva[0])) {
 case 0x10000:buf->b_data=&mos;break;
 case 0x12000:buf->b_data=mosnodes;break;
 case 0x1000:buf->b_data=&os;break;
 case 0x16000:buf->b_data=nodes;break;
 case 0x18000:buf->b_data=indirect;break;
 default:abort();
 }
 *(arc_buf_t **)priv=buf;return(0);
}
int arc_buf_size(arc_buf_t *b){return(sizebad?1:sizes[b-buffers]);}
void arc_buf_destroy(arc_buf_t *b,void *tag){(void)b;(void)tag;destroys++;}
void dmu_buf_will_dirty(dmu_buf_t *b,dmu_tx_t *t){(void)b;(void)t;abort();}
int main(int argc,char **argv) {
 if(argc!=3)return(99);
 const char *mode=argv[1];
 spa_t spa={0};vdev_t root={0},v={0};vdev_ops_t ops={0};vdev_t *kids[]={&v};
 strlcpy(ops.vdev_op_type,"raidz",sizeof(ops.vdev_op_type));v.vdev_ops=&ops;v.vdev_id=0;v.vdev_guid=7;v.vdev_ashift=12;v.vdev_children=3;v.vdev_nparity=1;v.vdev_asize=1<<20;root.vdev_children=1;root.vdev_child=kids;
 spa.spa_root_vdev=&root;spa.spa_mode=FREAD;spa.spa_trust_config=B_TRUE;spa.spa_ubsync.ub_txg=30;
 spa.spa_ubsync.ub_rootbp=bp(DMU_OT_OBJSET,0,4096,0x10000);
 dn(&mos.os_meta_dnode,DMU_OT_DNODE,bp(DMU_OT_DNODE,0,sizeof(mosnodes),0x12000));
 dn(&os.os_meta_dnode,DMU_OT_DNODE,bp(DMU_OT_DNODE,0,sizeof(nodes),0x16000));
 dn(&nodes[1],DMU_OT_PLAIN_FILE_CONTENTS,bp(DMU_OT_PLAIN_FILE_CONTENTS,1,512,0x18000));
 nodes[1].dn_flags=DNODE_FLAG_SPILL_BLKPTR;*DN_SPILL_BLKPTR(&nodes[1])=bp(DMU_OT_PLAIN_FILE_CONTENTS,0,512,0x1c000);
 indirect[0]=bp(DMU_OT_PLAIN_FILE_CONTENTS,0,512,0x1a000);
 for(unsigned i=2;i<=3;i++) {
  dn(&mosnodes[i],DMU_OT_DSL_DATASET,bp(DMU_OT_OBJECT_DIRECTORY,0,512,0x20000));memset(&mosnodes[i].dn_blkptr[0],0,sizeof(blkptr_t));
  mosnodes[i].dn_bonustype=DMU_OT_DSL_DATASET;mosnodes[i].dn_bonuslen=sizeof(dsl_dataset_phys_t);
  dsl_dataset_phys_t *ds=DN_BONUS(&mosnodes[i]);ds->ds_dir_obj=10;ds->ds_bp=bp(DMU_OT_OBJSET,0,4096,0x1000);
  ds->ds_prev_snap_txg=25; /* Root birth 20: new walker must not prune. */
  if(i==3){ds->ds_num_children=1;ds->ds_next_snap_obj=2;}
 }
 if(!strcmp(mode,"large-dnode"))nodes[1].dn_extra_slots=1;
 if(!strcmp(mode,"bonus-bounds"))nodes[1].dn_bonuslen=400;
 if(!strcmp(mode,"dataset-bonus"))mosnodes[2].dn_bonuslen=8;
 if(!strcmp(mode,"dataset-type"))mosnodes[2].dn_type=DMU_OT_PLAIN_FILE_CONTENTS;
 if(!strcmp(mode,"indirect-size"))nodes[1].dn_indblkshift=10;
 if(!strcmp(mode,"level"))nodes[1].dn_nlevels=1;
 if(!strcmp(mode,"checksum"))BP_SET_CHECKSUM(&os.os_meta_dnode.dn_blkptr[0],ZIO_CHECKSUM_OFF);
 if(!strcmp(mode,"arc-error"))fail=3;
 if(!strcmp(mode,"arc-size"))sizebad=1;
 if(!strcmp(mode,"embedded-parent"))os.os_meta_dnode.dn_blkptr[0].blk_prop|=1ULL<<39;
 if(!strcmp(mode,"future-birth"))os.os_meta_dnode.dn_blkptr[0].blk_birth=31;
 if(!strcmp(mode,"unknown-flags"))nodes[1].dn_flags|=128;
 if(!strcmp(mode,"writable"))spa.spa_mode|=FWRITE;
 if(!strcmp(mode,"cycle"))mos.os_meta_dnode.dn_blkptr[0]=spa.spa_ubsync.ub_rootbp;
 if(!strcmp(mode,"root-hole"))memset(&((dsl_dataset_phys_t *)DN_BONUS(&mosnodes[3]))->ds_bp,0,sizeof(blkptr_t));
 unsigned char saved[sizeof(mos)+sizeof(os)+sizeof(mosnodes)+sizeof(nodes)+sizeof(indirect)],after[sizeof(saved)];
 size_t off=0;
 #define SAVE(dst,obj) memcpy(dst+off,&obj,sizeof(obj));off+=sizeof(obj)
 SAVE(saved,mos);SAVE(saved,os);SAVE(saved,mosnodes);SAVE(saved,nodes);SAVE(saved,indirect);
 zi_out=fopen(argv[2],"w");if(!zi_out)return(98);
 int e;
 if(!strncmp(mode,"budget-",7)) {
  ze_ctx_t c={0};c.spa=&spa;zbookmark_phys_t z;SET_BOOKMARK(&z,0,0,-1,0);
  if(!strcmp(mode,"budget-visits"))c.visits=ZE_VISITS;
  if(!strcmp(mode,"budget-reads"))c.reads=ZE_READS;
  if(!strcmp(mode,"budget-bytes"))c.bytes=ZE_BYTES;
  if(!strcmp(mode,"budget-output"))c.output=ZE_OUTPUT;
  e=ze_visit(&c,NULL,&spa.spa_ubsync.ub_rootbp,&z,NULL,NULL,0,0,0,0,"selected_uberblock",!strcmp(mode,"budget-depth")?ZE_DEPTH:0);
 } else e=ze_run(&spa);
 fclose(zi_out);off=0;SAVE(after,mos);SAVE(after,os);SAVE(after,mosnodes);SAVE(after,nodes);SAVE(after,indirect);
 assert(!memcmp(saved,after,sizeof(saved)));assert(reads==destroys+(fail?1:0));
 printf("error=%d reads=%u writes=0\n",e,reads);return(e?1:0);
}
