/* Pool-free production walker fixtures. Real ZFS layouts; fake DMU and ARC. */
#include "zdb_inventory.c"
static dmu_buf_t bonus;
static objset_phys_t os;
static dnode_phys_t nodes[8];
static blkptr_t indirect[4];
static int active=1,tiny,fail_arc,size_bad;
static unsigned arcs,releases,holds,destroys;
static arc_buf_t buffers[64];static int sizes[64];
boolean_t spa_feature_is_active(spa_t *s,spa_feature_t f){(void)s;return(f==SPA_FEATURE_ASYNC_DESTROY&&active);}
static dsl_dataset_phys_t datasets[4];
static uint64_t object_count=3,directory_count=1,cursor_index;
static int bad_next,bad_info,bad_bonus,bad_directory,ddt_object,unknown_type,ambiguous_ds;
static unsigned infos,nexts;
static int extended_dataset,wrong_dataset_type;
int dmu_object_next(objset_t *os,uint64_t *obj,boolean_t hole,uint64_t txg){
 (void)os;assert(!hole && txg==0);nexts++;
 if(bad_next==1)return(EIO);
 if(bad_next==2)return(0);
 if(*obj>=object_count)return(ESRCH);
 (*obj)++;return(0);
}
int dmu_object_info(objset_t *o,uint64_t id,dmu_object_info_t *i){
 (void)o;infos++;if(bad_info)return(EIO);memset(i,0,sizeof(*i));
 i->doi_type=DMU_OT_OBJECT_DIRECTORY;
 if(id==2 || id==3){i->doi_type=DMU_OT_DSL_DATASET;i->doi_bonus_type=DMU_OT_DSL_DATASET;i->doi_bonus_size=sizeof(dsl_dataset_phys_t);}
 if(i->doi_bonus_type==DMU_OT_DSL_DATASET && extended_dataset)i->doi_type=DMU_OTN_ZAP_METADATA;
 if(i->doi_bonus_type==DMU_OT_DSL_DATASET && wrong_dataset_type)i->doi_type=DMU_OT_PLAIN_FILE_CONTENTS;
 if(ddt_object)i->doi_type=DMU_OT_DDT_ZAP;
 if(unknown_type)i->doi_type=127;
 if(ambiguous_ds && id==2)i->doi_bonus_type=DMU_OT_NONE;
 return(0);
}
int dmu_bonus_hold(objset_t *o,uint64_t id,void *tag,dmu_buf_t **db){
 (void)o;(void)tag;if(bad_bonus)return(EIO);assert(id==2 || id==3);
 bonus.db_data=&datasets[id];bonus.db_size=tiny?8:sizeof(dsl_dataset_phys_t);*db=&bonus;holds++;return(0);
}
void dmu_buf_rele(dmu_buf_t *db,void *tag){(void)db;(void)tag;releases++;}
void zap_cursor_init_noprefetch(zap_cursor_t *c,objset_t *os,uint64_t obj){(void)c;(void)os;assert(obj==DMU_POOL_DIRECTORY_OBJECT);cursor_index=0;}
int zap_cursor_retrieve(zap_cursor_t *c,zap_attribute_t *a){
 (void)c;if(bad_directory==1)return(EIO);if(cursor_index>=directory_count)return(ENOENT);
 memset(a,0,sizeof(*a));
 if(bad_directory==2)strlcpy(a->za_name,"DDT-future-format",sizeof(a->za_name));
 else if(bad_directory==3)memset(a->za_name,'x',sizeof(a->za_name));
 else (void)snprintf(a->za_name,sizeof(a->za_name),"key-%llu",(u_longlong_t)cursor_index);
 return(0);
}
void zap_cursor_advance(zap_cursor_t *c){(void)c;cursor_index++;}
void zap_cursor_fini(zap_cursor_t *c){(void)c;}
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
int main(int argc,char **argv){
 if(argc!=3)return(99);
 const char *mode=argv[1];
 spa_t spa={0};dsl_pool_t dp={0};vdev_t root={0},v={0};vdev_ops_t ops={0};vdev_t *kids[]={&v};static ddt_t ddt[ZIO_CHECKSUM_FUNCTIONS];
 assert(ZIO_CHECKSUM_FUNCTIONS*DDT_TYPES*DDT_CLASSES==42 && sizeof(zil_header_t)==192 && DMU_OT_DSL_DATASET==16 && DMU_OT_DDT_ZAP==42 && DMU_OT_DDT_STATS==43 && DMU_OTN_ZAP_METADATA==196);
 strlcpy(ops.vdev_op_type,"raidz",sizeof(ops.vdev_op_type));v.vdev_ops=&ops;v.vdev_id=0;v.vdev_guid=7;v.vdev_ashift=12;v.vdev_children=3;v.vdev_nparity=1;v.vdev_asize=1<<20;root.vdev_children=1;root.vdev_child=kids;
 spa.spa_root_vdev=&root;spa.spa_mode=FREAD;spa.spa_trust_config=B_TRUE;spa.spa_ubsync.ub_txg=30;spa.spa_meta_objset=(objset_t *)(uintptr_t)1;spa.spa_dsl_pool=&dp;
 for(unsigned i=0;i<ZIO_CHECKSUM_FUNCTIONS;i++){ddt[i].ddt_spa=&spa;ddt[i].ddt_os=spa.spa_meta_objset;ddt[i].ddt_checksum=i;spa.spa_ddt[i]=&ddt[i];}
 datasets[2].ds_dir_obj=10;datasets[2].ds_bp=bp(DMU_OT_OBJSET,0,4096,4096);datasets[2].ds_creation_txg=10;
 datasets[3]=datasets[2];datasets[3].ds_num_children=1;
 if(!strcmp(mode,"extended-dataset"))extended_dataset=1;
 if(!strcmp(mode,"wrong-dataset-type"))wrong_dataset_type=1;
 if(!strcmp(mode,"clone-head")){datasets[3].ds_num_children=0;datasets[3].ds_prev_snap_obj=99;}
 if(!strcmp(mode,"birth-pruned-head"))datasets[2].ds_prev_snap_txg=25;
 if(!strcmp(mode,"ddt-stats"))spa.spa_ddt_stat_object=7;
 if(!strcmp(mode,"ddt-in-core"))ddt[0].ddt_tree.avl_numnodes=1;
 if(!strcmp(mode,"ddt-slot"))ddt[ZIO_CHECKSUM_FUNCTIONS-1].ddt_object[DDT_TYPES-1][DDT_CLASSES-1]=9;
 if(!strcmp(mode,"ddt-cache-missing"))spa.spa_ddt[0]=NULL;
 if(!strcmp(mode,"ddt-cache-identity"))ddt[0].ddt_os=NULL;
 if(!strcmp(mode,"ddt-directory"))bad_directory=2;
 if(!strcmp(mode,"directory-error"))bad_directory=1;
 if(!strcmp(mode,"directory-name"))bad_directory=3;
 if(!strcmp(mode,"directory-exact"))directory_count=ZG_DIRECTORY_LIMIT;
 if(!strcmp(mode,"directory-limit"))directory_count=ZG_DIRECTORY_LIMIT+1;
 if(!strcmp(mode,"object-exact"))object_count=ZG_OBJECT_LIMIT;
 if(!strcmp(mode,"object-limit"))object_count=ZG_OBJECT_LIMIT+1;
 if(!strcmp(mode,"orphan-ddt"))ddt_object=1;
 if(!strcmp(mode,"unknown-type"))unknown_type=1;
 if(!strcmp(mode,"ambiguous-dataset"))ambiguous_ds=1;
 if(!strcmp(mode,"next-error"))bad_next=1;
 if(!strcmp(mode,"next-repeat"))bad_next=2;
 if(!strcmp(mode,"info-error"))bad_info=1;
 if(!strcmp(mode,"bonus-error"))bad_bonus=1;
 if(!strcmp(mode,"bonus-short"))tiny=1;
 if(!strcmp(mode,"inconsistent"))datasets[2].ds_flags=DS_FLAG_INCONSISTENT;
 if(!strcmp(mode,"unknown-flags"))datasets[2].ds_flags=1ULL<<63;
 if(!strcmp(mode,"future-dataset"))datasets[2].ds_creation_txg=31;
 if(!strcmp(mode,"snapshot-state"))datasets[3].ds_snapnames_zapobj=9;
 if(!strcmp(mode,"head-state"))datasets[2].ds_next_snap_obj=9;
 if(!strcmp(mode,"root-type"))BP_SET_TYPE(&datasets[2].ds_bp,DMU_OT_PLAIN_FILE_CONTENTS);
 if(!strcmp(mode,"root-hole"))memset(datasets[2].ds_bp.blk_dva,0,sizeof(datasets[2].ds_bp.blk_dva));
 if(!strcmp(mode,"zil-log"))os.os_zil_header.zh_log=datasets[2].ds_bp;
 if(!strcmp(mode,"zil-claim"))os.os_zil_header.zh_claim_txg=1;
 if(!strcmp(mode,"zil-padding"))os.os_zil_header.zh_pad[0]=1;
 if(!strcmp(mode,"arc-error"))fail_arc=1;
 if(!strcmp(mode,"arc-size"))size_bad=1;
 if(!strcmp(mode,"no-heads"))datasets[2].ds_num_children=1;
 if(!strcmp(mode,"writable"))spa.spa_mode=FREAD|FWRITE;
 dsl_dataset_phys_t before[4];memcpy(before,datasets,sizeof(before));zil_header_t saved=os.os_zil_header;
 boolean_t mode_header=!strcmp(mode,"mode-profile");
 if(mode_header){zdb_inventory_profile_mode();if(zdb_inventory_begin(argv[2]))return(98);}
 else {zi_out=fopen(argv[2],"w");if(!zi_out)return(98);}
 int e;
 if(!strncmp(mode,"budget-",7)){
  zg_ctx_t c={0};c.spa=&spa;dmu_object_info_t i={0};i.doi_bonus_size=sizeof(dsl_dataset_phys_t);i.doi_type=DMU_OT_DSL_DATASET;
  if(!strcmp(mode,"budget-reads"))c.reads=ZG_READ_LIMIT;
  if(!strcmp(mode,"budget-bytes"))c.bytes=ZG_BYTE_LIMIT;
  if(!strcmp(mode,"budget-datasets"))c.datasets=ZG_DATASET_LIMIT;
  if(!strcmp(mode,"budget-exact"))c.bytes=ZG_BYTE_LIMIT-sizeof(dsl_dataset_phys_t)-4096;
  if(!strcmp(mode,"budget-read-exact"))c.reads=ZG_READ_LIMIT-2;
  if(!strcmp(mode,"budget-dataset-exact"))c.datasets=ZG_DATASET_LIMIT-1;
  e=zg_dataset(&c,2,&i);
 }else e=zg_run(&spa);
 assert(holds==releases && destroys==arcs-(fail_arc?1:0));assert(!memcmp(before,datasets,sizeof(before)) && !memcmp(&saved,&os.os_zil_header,sizeof(saved)));
 if(mode_header)e=zdb_inventory_end(e);else fclose(zi_out);
 printf("error=%d infos=%u nexts=%u metadata_reads=%u holds=%u writes=0\n",e,infos,nexts,arcs,holds);return(e?1:0);
}
