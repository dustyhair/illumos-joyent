/* CDDL: see usr/src/OPENSOLARIS.LICENSE. Included by zdb_inventory.c.
 * Strict absence evidence, not DDT/ZIL enumeration or evacuation clearance.
 */
#include <sys/ddt.h>
#include <sys/dsl_dataset.h>
#include <sys/zap.h>
#define ZG_OBJECT_LIMIT 100000ULL
#define ZG_DIRECTORY_LIMIT 4096ULL
#define ZG_DATASET_LIMIT 4096ULL
#define ZG_READ_LIMIT 250000ULL
#define ZG_BYTE_LIMIT (32ULL << 20)
typedef struct zg_ctx {
 spa_t *spa;
 uint64_t objects,directory,datasets,heads,snapshots,reads,bytes,active;
} zg_ctx_t;
static int
zg_budget(zg_ctx_t *c,uint64_t n)
{
 if(c->reads>=ZG_READ_LIMIT || n>ZG_BYTE_LIMIT-c->bytes)return(E2BIG);
 c->reads++;c->bytes+=n;return(0);
}
static int
zg_ddt(zg_ctx_t *c)
{
 spa_t *s=c->spa;
 if(s->spa_ddt_stat_object)return(ENOTSUP);
 for(unsigned sum=0;sum<ZIO_CHECKSUM_FUNCTIONS;sum++) {
  ddt_t *d=s->spa_ddt[sum];
  if(!d || d->ddt_spa!=s || d->ddt_os!=s->spa_meta_objset ||
      d->ddt_checksum!=(enum zio_checksum)sum)return(EINVAL);
  if(d->ddt_stat_object || avl_numnodes(&d->ddt_tree) ||
      avl_numnodes(&d->ddt_repair_tree))return(ENOTSUP);
  for(unsigned t=0;t<DDT_TYPES;t++)for(unsigned k=0;k<DDT_CLASSES;k++)
   if(d->ddt_object[t][k])return(ENOTSUP);
 }
 int e=zg_budget(c,0);if(e)return(e);
 zap_cursor_t cursor;zap_attribute_t attribute;
 zap_cursor_init_noprefetch(&cursor,s->spa_meta_objset,DMU_POOL_DIRECTORY_OBJECT);
 for(;;) {
  e=zg_budget(c,sizeof(attribute));if(e)break;
  e=zap_cursor_retrieve(&cursor,&attribute);
  if(e==ENOENT){e=0;break;}
  if(e)break;
  if(c->directory>=ZG_DIRECTORY_LIMIT){e=E2BIG;break;}
  c->directory++;
  if(!memchr(attribute.za_name,0,sizeof(attribute.za_name))){e=EINVAL;break;}
  /* All current DDT names, and future names in this namespace, refused. */
  if(!strncmp(attribute.za_name,"DDT",3)){e=ENOTSUP;break;}
  (void)fprintf(zi_out,"{\"record\":\"profile_directory\",\"name\":");
  zi_string(zi_out,attribute.za_name);(void)fprintf(zi_out,",%s}\n",zi_flags);
  e=zp_flush();if(e)break;
  e=zg_budget(c,0);if(e)break;
  zap_cursor_advance(&cursor);
 }
 zap_cursor_fini(&cursor);return(e);
}
static int
zg_dataset(zg_ctx_t *c,uint64_t object,const dmu_object_info_t *info)
{
 if(c->datasets>=ZG_DATASET_LIMIT)return(E2BIG);
 if((info->doi_type!=DMU_OT_DSL_DATASET && info->doi_type!=DMU_OTN_ZAP_METADATA) ||
     info->doi_bonus_size!=sizeof(dsl_dataset_phys_t))return(ENOTSUP);
 int e=zg_budget(c,sizeof(dsl_dataset_phys_t));if(e)return(e);
 dmu_buf_t *db=NULL;dsl_dataset_phys_t ds;
 e=dmu_bonus_hold(c->spa->spa_meta_objset,object,c,&db);if(e)return(e);
 if(db->db_size<sizeof(ds))e=EINVAL;else memcpy(&ds,db->db_data,sizeof(ds));
 dmu_buf_rele(db,c);if(e)return(e);
 /* Refuse inconsistent, deferred-destroy and unknown dataset state. */
 if(!ds.ds_dir_obj || ds.ds_flags & ~(DS_FLAG_NOPROMOTE|DS_FLAG_UNIQUE_ACCURATE|DS_FLAG_CI_DATASET) ||
     ds.ds_creation_txg>c->spa->spa_ubsync.ub_txg || ds.ds_prev_snap_txg>c->spa->spa_ubsync.ub_txg)
  return(ENOTSUP);
 boolean_t snapshot=ds.ds_num_children!=0;
 if((snapshot && ds.ds_snapnames_zapobj) || (!snapshot && ds.ds_next_snap_obj))return(EINVAL);
 zil_header_t zh={0};const char *state="snapshot_not_applicable";
 if(!snapshot) {
  const blkptr_t *bp=&ds.ds_bp;const char *status="ordinary";
  if(BP_GET_TYPE(bp)!=DMU_OT_OBJSET || BP_GET_LEVEL(bp) || BP_IS_EMBEDDED(bp))return(ENOTSUP);
  e=zb_valid(c->spa,bp,&status);if(e)return(e);
  uint64_t size=BP_GET_LSIZE(bp);
  if((size!=OBJSET_PHYS_SIZE_V1 && size!=OBJSET_PHYS_SIZE_V2 && size!=sizeof(objset_phys_t)) ||
      BP_GET_CHECKSUM(bp)<=ZIO_CHECKSUM_ON || BP_GET_CHECKSUM(bp)>=ZIO_CHECKSUM_FUNCTIONS ||
      BP_GET_COMPRESS(bp)<=ZIO_COMPRESS_ON || BP_GET_COMPRESS(bp)>=ZIO_COMPRESS_FUNCTIONS ||
      BP_GET_PSIZE(bp)>size)return(ENOTSUP);
  e=zg_budget(c,size);if(e)return(e);
  arc_buf_t *buf=NULL;arc_flags_t flags=ARC_FLAG_WAIT;zbookmark_phys_t z;
  SET_BOOKMARK(&z,object,ZB_ROOT_OBJECT,ZB_ROOT_LEVEL,ZB_ROOT_BLKID);
  e=arc_read(NULL,c->spa,bp,arc_getbuf_func,&buf,ZIO_PRIORITY_ASYNC_READ,ZIO_FLAG_CANFAIL,&flags,&z);
  if(e)return(e);
  if(!buf || (uint64_t)arc_buf_size(buf)!=size){if(buf)arc_buf_destroy(buf,&buf);return(EIO);}
  zh=((objset_phys_t *)buf->b_data)->os_zil_header;
  arc_buf_destroy(buf,&buf);
  zil_header_t zero={0};
  if(memcmp(&zh,&zero,sizeof(zh)))return(ENOTSUP);
  state="canonical_zero_header";
 }
 c->datasets++;if(snapshot)c->snapshots++;else c->heads++;
 (void)fprintf(zi_out,"{\"record\":\"profile_dataset\",\"object\":\"%llu\","
     "\"snapshot\":%s,\"children\":\"%llu\",\"dataset_flags\":\"%llu\","
     "\"zil_state\":\"%s\",\"root_bp\":",(u_longlong_t)object,snapshot?"true":"false",
     (u_longlong_t)ds.ds_num_children,(u_longlong_t)ds.ds_flags,state);
 zi_words(zi_out,&ds.ds_bp);
 (void)fprintf(zi_out,",\"zil_header_words\":");
 if(snapshot)(void)fprintf(zi_out,"null");
 else {
  const uint64_t *words=(const uint64_t *)&zh;
  (void)fputc('[',zi_out);
  for(unsigned i=0;i<sizeof(zh)/sizeof(*words);i++)
   (void)fprintf(zi_out,"%s\"%016llx\"",i?",":"",(u_longlong_t)words[i]);
  (void)fputc(']',zi_out);
 }
 (void)fprintf(zi_out,",%s}\n",zi_flags);return(zp_flush());
}
static int
zg_scan(zg_ctx_t *c)
{
 uint64_t object=0;
 for(;;) {
  int e=zg_budget(c,0);if(e)return(e);
  uint64_t previous=object;
  e=dmu_object_next(c->spa->spa_meta_objset,&object,B_FALSE,0);
  if(e==ESRCH)return(0);
  if(e)return(e);
  if(object<=previous)return(EINVAL);
  c->active=object;
  if(c->objects>=ZG_OBJECT_LIMIT)return(E2BIG);
  c->objects++;
  e=zg_budget(c,sizeof(dmu_object_info_t));if(e)return(e);
  dmu_object_info_t info;
  e=dmu_object_info(c->spa->spa_meta_objset,object,&info);if(e)return(e);
  if(!DMU_OT_IS_VALID(info.doi_type) || !DMU_OT_IS_VALID(info.doi_bonus_type))return(ENOTSUP);
  if(info.doi_type==DMU_OT_DDT_ZAP || info.doi_type==DMU_OT_DDT_STATS ||
      info.doi_bonus_type==DMU_OT_DDT_ZAP || info.doi_bonus_type==DMU_OT_DDT_STATS)return(ENOTSUP);
  if(info.doi_type==DMU_OT_DSL_DATASET && info.doi_bonus_type!=DMU_OT_DSL_DATASET)return(EINVAL);
  (void)fprintf(zi_out,"{\"record\":\"profile_object\",\"object\":\"%llu\",\"object_type\":%u,\"bonus_type\":%u,%s}\n",(u_longlong_t)object,(unsigned)info.doi_type,(unsigned)info.doi_bonus_type,zi_flags);
  e=zp_flush();if(e)return(e);
  if(info.doi_bonus_type==DMU_OT_DSL_DATASET) {
   e=zg_dataset(c,object,&info);if(e)return(e);
  }
 }
}
static int
zg_run(spa_t *spa)
{
 zg_ctx_t c={0};c.spa=spa;
 int e=spa_writeable(spa)||spa_mode(spa)!=FREAD ? EROFS:0;
 (void)fprintf(zi_out,"{\"record\":\"profile_scope\",\"profile\":\"ddt-absent-zil-zero-v1\","
     "\"ddt_enumerated\":false,\"zil_enumerated\":false,%s}\n",zi_flags);
 if(!e)e=zp_flush();
 if(!e)e=zg_ddt(&c);
 if(!e)e=zg_scan(&c);
 if(!e && (!c.heads || spa_writeable(spa) || spa_mode(spa)!=FREAD))e=EINVAL;
 (void)fprintf(zi_out,"{\"record\":\"profile_end\",\"profile_metadata_complete\":%s,"
     "\"error\":%d,\"objects\":\"%llu\",\"directory_entries\":\"%llu\","
     "\"datasets\":\"%llu\",\"heads\":\"%llu\",\"snapshots\":\"%llu\","
     "\"reads\":\"%llu\",\"read_bytes\":\"%llu\",\"active_object\":\"%llu\","
     "\"cached_ddt_slots\":\"%u\",\"ddt_absence_checked\":%s,%s}\n",
     e?"false":"true",e,(u_longlong_t)c.objects,(u_longlong_t)c.directory,
     (u_longlong_t)c.datasets,(u_longlong_t)c.heads,(u_longlong_t)c.snapshots,
     (u_longlong_t)c.reads,(u_longlong_t)c.bytes,(u_longlong_t)c.active,
     (unsigned)(ZIO_CHECKSUM_FUNCTIONS*DDT_TYPES*DDT_CLASSES),e?"false":"true",zi_flags);
 int f=zp_flush();return(f?f:e);
}
