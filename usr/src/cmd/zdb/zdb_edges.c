/* CDDL: see usr/src/OPENSOLARIS.LICENSE. Additive read-only diagnostic walker.
 * No snapshot birth cutoff; offsets address decoded AMD64 ARC bytes only.
 */
#include <stddef.h>
#define ZE_VISITS 100000ULL
#define ZE_READS 100000ULL
#define ZE_BYTES (64ULL<<20)
#define ZE_OUTPUT (512ULL<<20)
#define ZE_ROOTS 4096ULL
#define ZE_DEPTH 64
#define ZE_FLAGS "\"reverse_references_complete\":false,\"pointer_rewrite_ready\":false,\"placement_performed\":false"
typedef struct ze_root {
 blkptr_t bp,parent;
 zbookmark_phys_t parent_z;
 uint64_t object,parent_id,offset;
} ze_root_t;
typedef struct ze_ctx {
 spa_t *spa;
 uint64_t visits,reads,bytes,output,roots;
 ze_root_t *root;
 blkptr_t ancestor[ZE_DEPTH];
} ze_ctx_t;
static int
ze_charge(ze_ctx_t *c,uint64_t n)
{
 if(n>ZE_OUTPUT-c->output)return(E2BIG);
 c->output+=n;return(0);
}
static int ze_visit(ze_ctx_t *,const dnode_phys_t *,const blkptr_t *,
 const zbookmark_phys_t *,const blkptr_t *,const zbookmark_phys_t *,
 uint64_t,uint64_t,uint64_t,uint64_t,const char *,unsigned);
static int
ze_dnode(ze_ctx_t *c,const dnode_phys_t *d,uint64_t object,uint64_t base,
 const blkptr_t *parent,const zbookmark_phys_t *pz,uint64_t parent_id,unsigned depth)
{
 if(d->dn_extra_slots || d->dn_nblkptr>3 || d->dn_nlevels>16 ||
    !DMU_OT_IS_VALID(d->dn_type) || !DMU_OT_IS_VALID(d->dn_bonustype) ||
    (d->dn_flags & ~(DNODE_FLAG_USED_BYTES|DNODE_FLAG_USERUSED_ACCOUNTED|
      DNODE_FLAG_SPILL_BLKPTR|DNODE_FLAG_USEROBJUSED_ACCOUNTED)))return(ENOTSUP);
 uint64_t bonus=offsetof(dnode_phys_t,dn_blkptr)+d->dn_nblkptr*sizeof(blkptr_t);
 uint64_t end=sizeof(*d)-(d->dn_flags & DNODE_FLAG_SPILL_BLKPTR?sizeof(blkptr_t):0);
 if(bonus>end || d->dn_bonuslen>end-bonus)return(EINVAL);
 if(d->dn_type==DMU_OT_NONE) {
  if(d->dn_nblkptr || d->dn_bonuslen || d->dn_bonustype || d->dn_flags)return(EINVAL);
  return(0);
 }
 if(d->dn_nblkptr && (!d->dn_nlevels || !d->dn_datablkszsec ||
    d->dn_indblkshift<SPA_BLKPTRSHIFT || d->dn_indblkshift>24))return(EINVAL);
 if(d->dn_bonustype==DMU_OT_DSL_DATASET) {
  if(pz->zb_objset!=0 || !object || c->roots>=ZE_ROOTS ||
     (d->dn_type!=DMU_OT_DSL_DATASET && d->dn_type!=DMU_OTN_ZAP_METADATA) ||
     d->dn_bonuslen!=sizeof(dsl_dataset_phys_t))return(ENOTSUP);
  for(uint64_t j=0;j<c->roots;j++)if(c->root[j].object==object)return(EINVAL);
  const dsl_dataset_phys_t *ds=(const void *)((const char *)d+bonus);
  ze_root_t *r=&c->root[c->roots++];r->bp=ds->ds_bp;r->parent=*parent;
  r->parent_z=*pz;r->object=object;r->parent_id=parent_id;
  r->offset=base+bonus+offsetof(dsl_dataset_phys_t,ds_bp);
  if(!BP_IS_HOLE(&r->bp) && (BP_GET_TYPE(&r->bp)!=DMU_OT_OBJSET || BP_GET_LEVEL(&r->bp)))return(ENOTSUP);
 } else if(d->dn_type==DMU_OT_DSL_DATASET)return(EINVAL);
 zbookmark_phys_t z;int e=0;
 for(unsigned i=0;i<d->dn_nblkptr && !e;i++) {
  SET_BOOKMARK(&z,pz->zb_objset,object,d->dn_nlevels-1,i);
  e=ze_visit(c,d,&d->dn_blkptr[i],&z,parent,pz,parent_id,
      base+offsetof(dnode_phys_t,dn_blkptr)+i*sizeof(blkptr_t),base,i,"dnode",depth);
 }
 if(!e && (d->dn_flags & DNODE_FLAG_SPILL_BLKPTR)) {
  SET_BOOKMARK(&z,pz->zb_objset,object,0,DMU_SPILL_BLKID);
  e=ze_visit(c,d,DN_SPILL_BLKPTR(d),&z,parent,pz,parent_id,
      base+sizeof(*d)-sizeof(blkptr_t),base,0,"spill",depth);
 }
 return(e);
}
static int
ze_visit(ze_ctx_t *c,const dnode_phys_t *d,const blkptr_t *bp,
 const zbookmark_phys_t *z,const blkptr_t *parent,const zbookmark_phys_t *pz,
 uint64_t parent_id,uint64_t offset,uint64_t dnode_offset,uint64_t slot,
 const char *kind,unsigned depth)
{
 if(depth>=ZE_DEPTH || c->visits>=ZE_VISITS)return(E2BIG);
 if(!DMU_OT_IS_VALID(BP_GET_TYPE(bp)) || BP_GET_TYPE(bp)==DMU_OT_DDT_ZAP ||
    BP_GET_TYPE(bp)==DMU_OT_DDT_STATS || BP_GET_TYPE(bp)==DMU_OT_DEDUP || BP_USES_CRYPT(bp) || BP_GET_DEDUP(bp))return(ENOTSUP);
 boolean_t embedded=BP_IS_EMBEDDED(bp),hole=!embedded && BP_IS_HOLE(bp);
 if(!hole) {const char *status="ordinary";int e=zb_valid(c->spa,bp,&status);if(e)return(e);}
 unsigned type=BP_GET_TYPE(bp),level=BP_GET_LEVEL(bp);
 if(level>16 || (!hole && d && (int64_t)level!=z->zb_level))return(EINVAL);
 boolean_t metadata=!hole && (level || type==DMU_OT_DNODE || type==DMU_OT_OBJSET);
 if(metadata && embedded)return(ENOTSUP);
 int e=ze_charge(c,4096);if(e)return(e);
 uint64_t id=++c->visits;
 (void)fprintf(zi_out,"{\"record\":\"parent_edge\",\"node\":\"%llu\",\"parent_node\":\"%llu\",\"kind\":\"%s\",\"decoded_offset\":\"%llu\",\"dnode_offset\":\"%llu\",\"slot\":\"%llu\",\"bookmark\":",
     (u_longlong_t)id,(u_longlong_t)parent_id,kind,(u_longlong_t)offset,
     (u_longlong_t)dnode_offset,(u_longlong_t)slot);
 zb_bookmark(z);(void)fprintf(zi_out,",\"child_bp\":");zi_words(zi_out,bp);
 (void)fprintf(zi_out,",\"parent_bp\":");if(parent)zi_words(zi_out,parent);else (void)fprintf(zi_out,"null");
 (void)fprintf(zi_out,",\"parent_bookmark\":");if(pz)zb_bookmark(pz);else (void)fprintf(zi_out,"null");
 (void)fprintf(zi_out,",\"disposition\":\"%s\",%s,%s}\n",hole?"hole":embedded?"embedded":metadata?"container":"leaf",ZE_FLAGS,zi_flags);
 e=zp_flush();if(e || !metadata)return(e);
 for(unsigned i=0;i<depth;i++)if(!memcmp(bp,&c->ancestor[i],sizeof(*bp)))return(ELOOP);
 c->ancestor[depth]=*bp;
 uint64_t n=BP_GET_LSIZE(bp);
 if((BP_GET_CHECKSUM(bp)!=ZIO_CHECKSUM_FLETCHER_4 && BP_GET_CHECKSUM(bp)!=ZIO_CHECKSUM_SHA256) ||
    BP_GET_COMPRESS(bp)<=ZIO_COMPRESS_ON || BP_GET_COMPRESS(bp)>=ZIO_COMPRESS_FUNCTIONS ||
    BP_GET_PSIZE(bp)>n)return(ENOTSUP);
 if(!n || n>(16ULL<<20) || (level && (!d || n!=(1ULL<<d->dn_indblkshift) || n%sizeof(blkptr_t))) ||
    (!level && type==DMU_OT_DNODE && n%sizeof(dnode_phys_t)) ||
    (!level && type==DMU_OT_OBJSET && n!=OBJSET_PHYS_SIZE_V1 && n!=OBJSET_PHYS_SIZE_V2 && n!=sizeof(objset_phys_t)))return(EINVAL);
 if(c->reads>=ZE_READS || n>ZE_BYTES-c->bytes)return(E2BIG);
 e=ze_charge(c,2048+2*n);if(e)return(e);
 c->reads++;c->bytes+=n;
 arc_buf_t *buf=NULL;arc_flags_t flags=ARC_FLAG_WAIT;
 e=arc_read(NULL,c->spa,bp,arc_getbuf_func,&buf,ZIO_PRIORITY_ASYNC_READ,ZIO_FLAG_CANFAIL,&flags,z);
 if(e)return(e);
 if(!buf || (uint64_t)arc_buf_size(buf)!=n){if(buf)arc_buf_destroy(buf,&buf);return(EIO);}
 (void)fprintf(zi_out,"{\"record\":\"parent_container\",\"node\":\"%llu\",\"decoded_size\":\"%llu\",\"authentication\":\"original_bp_arc_checksum\",\"decoded_hex\":\"",(u_longlong_t)id,(u_longlong_t)n);
 const unsigned char *bytes=buf->b_data;
 for(uint64_t i=0;i<n;i++)(void)fprintf(zi_out,"%02x",bytes[i]);
 (void)fprintf(zi_out,"\",%s,%s}\n",ZE_FLAGS,zi_flags);
 e=zp_flush();
 if(!e && level) {
  const blkptr_t *children=buf->b_data;uint64_t count=n/sizeof(*children);
  if(z->zb_blkid>(UINT64_MAX-(count-1))/count)e=EOVERFLOW;
  for(uint64_t i=0;i<count && !e;i++) {
   zbookmark_phys_t next;SET_BOOKMARK(&next,z->zb_objset,z->zb_object,z->zb_level-1,z->zb_blkid*count+i);
   e=ze_visit(c,d,&children[i],&next,bp,z,id,i*sizeof(blkptr_t),0,i,"indirect",depth+1);
  }
 } else if(!e && type==DMU_OT_DNODE) {
  const dnode_phys_t *children=buf->b_data;uint64_t count=n/sizeof(*children);
  if(z->zb_blkid>(UINT64_MAX-(count-1))/count)e=EOVERFLOW;
  for(uint64_t i=0;i<count && !e;i++)e=ze_dnode(c,&children[i],z->zb_blkid*count+i,i*sizeof(*children),bp,z,id,depth+1);
 } else if(!e) {
  objset_phys_t *os=buf->b_data;
  e=ze_dnode(c,&os->os_meta_dnode,DMU_META_DNODE_OBJECT,offsetof(objset_phys_t,os_meta_dnode),bp,z,id,depth+1);
  if(!e && OBJSET_BUF_HAS_USERUSED(buf)) {
   e=ze_dnode(c,&os->os_userused_dnode,DMU_USERUSED_OBJECT,offsetof(objset_phys_t,os_userused_dnode),bp,z,id,depth+1);
   if(!e)e=ze_dnode(c,&os->os_groupused_dnode,DMU_GROUPUSED_OBJECT,offsetof(objset_phys_t,os_groupused_dnode),bp,z,id,depth+1);
   if(!e && OBJSET_BUF_HAS_PROJECTUSED(buf))e=ze_dnode(c,&os->os_projectused_dnode,DMU_PROJECTUSED_OBJECT,offsetof(objset_phys_t,os_projectused_dnode),bp,z,id,depth+1);
  }
 }
 arc_buf_destroy(buf,&buf);return(e);
}
static int
ze_run(spa_t *spa)
{
 ze_ctx_t *c=calloc(1,sizeof(*c));if(!c)return(ENOMEM);c->spa=spa;
 c->root=calloc(ZE_ROOTS,sizeof(*c->root));if(!c->root){free(c);return(ENOMEM);}
 int e=spa_mode(spa)!=FREAD || spa_writeable(spa)?EROFS:0;
 zbookmark_phys_t z;SET_BOOKMARK(&z,0,ZB_ROOT_OBJECT,ZB_ROOT_LEVEL,ZB_ROOT_BLKID);
 (void)fprintf(zi_out,"{\"record\":\"parent_scope\",\"encoding\":\"decoded-amd64-little-endian\",\"snapshot_birth_pruning\":false,\"scope\":\"current_MOS_and_dataset_tree_pointer_slots\",\"semantic_BP_payloads_enumerated\":false,\"historical_roots_enumerated\":false,\"anchor\":\"selected_import_uberblock_not_independent_label_proof\",%s,%s}\n",ZE_FLAGS,zi_flags);
 if(!e)e=zp_flush();
 if(!e)e=ze_visit(c,NULL,spa_get_rootblkptr(spa),&z,NULL,NULL,0,0,0,0,"selected_uberblock",0);
 uint64_t roots=c->roots;
 for(uint64_t i=0;i<roots && !e;i++) {
  ze_root_t *r=&c->root[i];SET_BOOKMARK(&z,r->object,ZB_ROOT_OBJECT,ZB_ROOT_LEVEL,ZB_ROOT_BLKID);
  e=ze_visit(c,NULL,&r->bp,&z,&r->parent,&r->parent_z,r->parent_id,r->offset,0,0,"dataset_bonus",0);
 }
 if(!e && (!roots || roots!=c->roots))e=EINVAL;
 (void)fprintf(zi_out,"{\"record\":\"parent_end\",\"parent_capture_complete\":%s,\"error\":%d,\"nodes\":\"%llu\",\"containers\":\"%llu\",\"decoded_bytes\":\"%llu\",\"output_charge\":\"%llu\",\"dataset_roots\":\"%llu\",%s,%s}\n",e?"false":"true",e,(u_longlong_t)c->visits,(u_longlong_t)c->reads,(u_longlong_t)c->bytes,(u_longlong_t)c->output,(u_longlong_t)c->roots,ZE_FLAGS,zi_flags);
 int f=zp_flush();free(c->root);free(c);return(f?f:e);
}
