/* CDDL: see usr/src/OPENSOLARIS.LICENSE. Included by zdb_inventory.c.
 * Bounded read-only destroyed-tree ownership; no bptree_iterate/free calls.
 */
#include <sys/bptree.h>
#include <sys/dnode.h>
#include <sys/dmu_objset.h>
#include <sys/zfeature.h>
#define ZB_QUEUE_LIMIT 4096ULL
#define ZB_VISIT_LIMIT 100000ULL
#define ZB_READ_LIMIT 100000ULL
#define ZB_BYTE_LIMIT (64ULL << 20)
#define ZB_DEPTH_LIMIT 64
#define ZB_BLOCK_LIMIT (16ULL << 20)
typedef struct zb_ctx {
 spa_t *spa;
 uint64_t object, index, entries, owners, visits, reads, bytes, cutoff;
 zbookmark_phys_t resume;
 blkptr_t ancestors[ZB_DEPTH_LIMIT];
} zb_ctx_t;
static int
zb_valid(spa_t *spa, const blkptr_t *bp, const char **status)
{
 int error = 0;
 if (!bp->blk_birth) return (EINVAL);

 /* Full destroyed-tree BPs are retained, but never payload-verified here. */
 if (!DMU_OT_IS_VALID(BP_GET_TYPE(bp)) || BP_USES_CRYPT(bp) ||
     BP_GET_DEDUP(bp) || BP_GET_TYPE(bp) == DMU_OT_DEDUP ||
     BP_GET_TYPE(bp) == DMU_OT_DDT_ZAP || BP_GET_TYPE(bp) == DMU_OT_DDT_STATS)
  error = ENOTSUP;
 else if (BP_IS_EMBEDDED(bp)) *status = "embedded";
 else if (BP_IS_HOLE(bp) || BP_COUNT_GANG(bp)) error = ENOTSUP;
 if (!error && (bp->blk_birth > spa->spa_ubsync.ub_txg ||
     (!BP_IS_EMBEDDED(bp) && BP_PHYSICAL_BIRTH(bp) > spa->spa_ubsync.ub_txg)))
  error = EINVAL;
 if (!error && !BP_IS_EMBEDDED(bp)) {
  for (unsigned i = 0; i < SPA_DVAS_PER_BP; i++) {
   const dva_t *d = &bp->blk_dva[i];
   uint64_t a = DVA_GET_ASIZE(d), id = DVA_GET_VDEV(d);
   if (!a) {
    if (d->dva_word[0] || d->dva_word[1]) error = EINVAL;
    continue;
   }
   vdev_t *v = id < spa->spa_root_vdev->vdev_children ?
       spa->spa_root_vdev->vdev_child[id] : NULL;
   if (!v || v->vdev_id != id || v->vdev_alloc_bias != VDEV_BIAS_NONE ||
       v->vdev_islog || v->vdev_removing ||
       strcmp(v->vdev_ops->vdev_op_type, "raidz") ||
       v->vdev_ashift < SPA_MINBLOCKSHIFT || v->vdev_ashift > 16 ||
       !v->vdev_nparity || v->vdev_nparity >= v->vdev_children) {
    error = ENOTSUP; continue;
   }
   uint64_t sec = 1ULL << v->vdev_ashift, p = BP_GET_PSIZE(bp);
   uint64_t n = (p + sec - 1) / sec, data = v->vdev_children - v->vdev_nparity;
   uint64_t sectors = n + v->vdev_nparity * ((n + data - 1) / data);
   uint64_t unit = v->vdev_nparity + 1;
   uint64_t expected = ((sectors + unit - 1) / unit) * unit * sec;
   uint64_t off = DVA_GET_OFFSET(d);
   if (!p || a != expected || off % sec || off > UINT64_MAX - a ||
       off + a > v->vdev_asize) error = ERANGE;
  }
 }
 return (error);
}
static int
zb_budget(zb_ctx_t *c, uint64_t n)
{
 if (c->reads >= ZB_READ_LIMIT || n > ZB_BYTE_LIMIT - c->bytes)
  return (E2BIG);
 c->reads++; c->bytes += n;
 return (0);
}
static void
zb_bookmark(const zbookmark_phys_t *z)
{
 (void)fprintf(zi_out, "[\"%llu\",\"%llu\",\"%lld\",\"%llu\"]",
     (u_longlong_t)z->zb_objset, (u_longlong_t)z->zb_object,
     (longlong_t)z->zb_level, (u_longlong_t)z->zb_blkid);
}
static int
zb_owner(zb_ctx_t *c, const blkptr_t *bp, const zbookmark_phys_t *z,
    unsigned depth)
{
 const char *status = "ordinary";
 int e = zb_valid(c->spa, bp, &status);
 if (e) return (e);
 c->owners++;
 (void)fprintf(zi_out, "{\"record\":\"async_owner\",\"queue\":\"async_destroy\","
     "\"object\":\"%llu\",\"entry_index\":\"%llu\",\"depth\":\"%u\","
     "\"status\":\"%s\",\"error\":0,\"payload_verifiable\":false,"
     "\"movable_candidate\":false,\"bookmark\":", (u_longlong_t)c->object,
     (u_longlong_t)c->index, depth, status);
 zb_bookmark(z); (void)fprintf(zi_out, ",\"bp_words\":"); zi_words(zi_out,bp);
 (void)fprintf(zi_out, ",%s}\n",zi_flags);
 return (zp_flush());
}
static int zb_visit(zb_ctx_t *, const dnode_phys_t *, const blkptr_t *,
    const zbookmark_phys_t *, unsigned);
/* Restrict to small dnodes; large-dnode layouts need separate validation. */
static int
zb_dnode(zb_ctx_t *c, const dnode_phys_t *d, uint64_t object, unsigned depth)
{
 if (d->dn_extra_slots || d->dn_nblkptr > 3 || d->dn_nlevels > 16 ||
     (d->dn_nblkptr && (!d->dn_nlevels || !d->dn_datablkszsec ||
     d->dn_indblkshift < SPA_BLKPTRSHIFT || d->dn_indblkshift > 24)) ||
     !DMU_OT_IS_VALID(d->dn_type)) return (ENOTSUP);
 if (d->dn_type == DMU_OT_NONE) {
  if (d->dn_nblkptr || (d->dn_flags & DNODE_FLAG_SPILL_BLKPTR)) return (EINVAL);
  return (0);
 }
 if (object != DMU_META_DNODE_OBJECT && !ZB_IS_ZERO(&c->resume) &&
     object < c->resume.zb_object) return (0);
 zbookmark_phys_t z;
 for (unsigned i=0; i<d->dn_nblkptr; i++) {
  SET_BOOKMARK(&z,ZB_DESTROYED_OBJSET,object,d->dn_nlevels-1,i);
  int e=zb_visit(c,d,&d->dn_blkptr[i],&z,depth);
  if(e) return(e);
 }
 if (d->dn_flags & DNODE_FLAG_SPILL_BLKPTR) {
  if (d->dn_nblkptr > 2) return (EINVAL);
  SET_BOOKMARK(&z,ZB_DESTROYED_OBJSET,object,0,DMU_SPILL_BLKID);
  return (zb_visit(c,d,DN_SPILL_BLKPTR(d),&z,depth));
 }
 return (0);
}
static int
zb_visit(zb_ctx_t *c, const dnode_phys_t *d, const blkptr_t *bp,
    const zbookmark_phys_t *z, unsigned depth)
{
 if (depth >= ZB_DEPTH_LIMIT || c->visits >= ZB_VISIT_LIMIT) return (E2BIG);
 c->visits++;
 /* Resume ordering is checked before dereferencing possibly freed blocks. */
 boolean_t skip_children=B_FALSE;
 if (!ZB_IS_ZERO(&c->resume)) {
  if (d != NULL) {
   if (z->zb_level < 0 || z->zb_level > 16 || z->zb_blkid==UINT64_MAX)
    return (ENOTSUP);
   unsigned shift=(d->dn_indblkshift-SPA_BLKPTRSHIFT)*z->zb_level;
   if (shift >= 64 || z->zb_blkid+1 > (UINT64_MAX >> shift)) return(EOVERFLOW);
   uint64_t end=(z->zb_blkid+1)<<shift;
   if (z->zb_object==DMU_META_DNODE_OBJECT &&
       end > UINT64_MAX/d->dn_datablkszsec) return(EOVERFLOW);
   if (zbookmark_subtree_completed(d,z,&c->resume)) return(0);
  }
  if (!memcmp(z,&c->resume,sizeof(*z))) {
   memset(&c->resume,0,sizeof(c->resume)); skip_children=B_TRUE;
  }
 }
 if (bp->blk_birth && bp->blk_birth <= c->cutoff)
  return (skip_children ? EINVAL : 0);
 if (BP_IS_HOLE(bp)) return (0);
 const char *status="ordinary";
 int e=zb_valid(c->spa,bp,&status);
 if(e) return(e);
 if (BP_GET_LEVEL(bp)>16 || (d && (int64_t)BP_GET_LEVEL(bp)!=z->zb_level)) return(EINVAL);
 if (skip_children) return(zb_owner(c,bp,z,depth));
 for(unsigned i=0;i<depth;i++) {
  if(!memcmp(&c->ancestors[i],bp,sizeof(*bp))) return(ELOOP);
  if(BP_IS_EMBEDDED(bp) || BP_IS_EMBEDDED(&c->ancestors[i]))continue;
  for(unsigned a=0;a<SPA_DVAS_PER_BP;a++) {
   const dva_t *x=&bp->blk_dva[a];uint64_t xs=DVA_GET_ASIZE(x),xo=DVA_GET_OFFSET(x);
   if(!xs)continue;
   for(unsigned b=0;b<SPA_DVAS_PER_BP;b++) {
    const dva_t *y=&c->ancestors[i].blk_dva[b];uint64_t ys=DVA_GET_ASIZE(y),yo=DVA_GET_OFFSET(y);
    if(ys && DVA_GET_VDEV(x)==DVA_GET_VDEV(y) && xo<yo+ys && yo<xo+xs)
     return(ELOOP);
   }
  }
 }
 c->ancestors[depth]=*bp;
 unsigned type=BP_GET_TYPE(bp),level=BP_GET_LEVEL(bp);
 boolean_t metadata=level || type==DMU_OT_DNODE || type==DMU_OT_OBJSET;
 if (metadata && BP_IS_EMBEDDED(bp)) return(ENOTSUP);
 if (metadata && (BP_GET_CHECKSUM(bp)<=ZIO_CHECKSUM_ON ||
     BP_GET_COMPRESS(bp)<=ZIO_COMPRESS_ON ||
     BP_GET_CHECKSUM(bp)>=ZIO_CHECKSUM_FUNCTIONS ||
     BP_GET_COMPRESS(bp)>=ZIO_COMPRESS_FUNCTIONS ||
     BP_GET_PSIZE(bp)>BP_GET_LSIZE(bp))) return(ENOTSUP);
 if (!metadata) return(zb_owner(c,bp,z,depth));
 uint64_t n=BP_GET_LSIZE(bp);
 if (!n || n>ZB_BLOCK_LIMIT || (level && n%sizeof(blkptr_t)) ||
     (!level && type==DMU_OT_DNODE && n%sizeof(dnode_phys_t)) ||
     (!level && type==DMU_OT_OBJSET && n!=OBJSET_PHYS_SIZE_V1 &&
     n!=OBJSET_PHYS_SIZE_V2 && n!=sizeof(objset_phys_t))) return(EINVAL);
 if (level && (!d || n != (1ULL<<d->dn_indblkshift))) return(EINVAL);
 e=zb_budget(c,n); if(e) return(e);
 arc_flags_t flags=ARC_FLAG_WAIT; arc_buf_t *buf=NULL;
 e=arc_read(NULL,c->spa,bp,arc_getbuf_func,&buf,ZIO_PRIORITY_ASYNC_READ,
     ZIO_FLAG_CANFAIL,&flags,z);
 if(e) return(e);
 if (!buf || (uint64_t)arc_buf_size(buf)!=n) { if(buf)arc_buf_destroy(buf,&buf);return(EIO); }
 if(level) {
  blkptr_t *child=buf->b_data; uint64_t count=n/sizeof(*child);
  if(z->zb_blkid > (UINT64_MAX-(count-1))/count) e=EOVERFLOW;
  for(uint64_t i=0;i<count && !e;i++) {
   zbookmark_phys_t next; SET_BOOKMARK(&next,z->zb_objset,z->zb_object,
       z->zb_level-1,z->zb_blkid*count+i);
   e=zb_visit(c,d,&child[i],&next,depth+1);
  }
 } else if(type==DMU_OT_DNODE) {
  dnode_phys_t *child=buf->b_data;uint64_t count=n/sizeof(*child);
  if(z->zb_blkid > (UINT64_MAX-(count-1))/count) e=EOVERFLOW;
  for(uint64_t i=0;i<count && !e;i++)
   e=zb_dnode(c,&child[i],z->zb_blkid*count+i,depth+1);
 } else {
  objset_phys_t *os=buf->b_data;
  e=zb_dnode(c,&os->os_meta_dnode,DMU_META_DNODE_OBJECT,depth+1);
  if(!e && OBJSET_BUF_HAS_USERUSED(buf)) {
   if(OBJSET_BUF_HAS_PROJECTUSED(buf))
    e=zb_dnode(c,&os->os_projectused_dnode,DMU_PROJECTUSED_OBJECT,depth+1);
   if(!e)e=zb_dnode(c,&os->os_groupused_dnode,DMU_GROUPUSED_OBJECT,depth+1);
   if(!e)e=zb_dnode(c,&os->os_userused_dnode,DMU_USERUSED_OBJECT,depth+1);
  }
 }
 arc_buf_destroy(buf,&buf);
 return(e ? e : zb_owner(c,bp,z,depth));
}
/* Split at data block boundaries: 168-byte entries need not divide a block. */
static int
zb_read(zb_ctx_t *c, uint64_t off, uint64_t n, uint64_t block, void *dst)
{
 while(n) {
  uint64_t part=MIN(n,block-off%block),data=off;
  int e=zb_budget(c,part); if(e)return(e);
  e=dmu_offset_next(c->spa->spa_meta_objset,c->object,B_FALSE,&data);
  if(e)return(e);
  if(data!=off)return(EINVAL);
  e=zb_budget(c,0);if(e)return(e);
  e=dmu_read(c->spa->spa_meta_objset,c->object,off,part,dst,DMU_READ_NO_PREFETCH);
  if(e)return(e);
  off+=part;dst=(char *)dst+part;n-=part;
 }
 return(0);
}
static int
zb_queue(zb_ctx_t *c)
{
 dmu_object_info_t info; bptree_phys_t hdr;dmu_buf_t *db=NULL;
 int e=zb_budget(c,0);if(e)return(e);
 e=dmu_object_info(c->spa->spa_meta_objset,c->object,&info);
 if(e)return(e);
 if(info.doi_type!=DMU_OTN_UINT64_METADATA ||
     info.doi_bonus_type!=DMU_OTN_UINT64_METADATA || info.doi_bonus_size!=sizeof(hdr) ||
     info.doi_data_block_size<SPA_MINBLOCKSIZE ||
     info.doi_data_block_size%SPA_MINBLOCKSIZE || info.doi_data_block_size>ZB_BLOCK_LIMIT)
  return(EINVAL);
 e=zb_budget(c,sizeof(hdr));if(e)return(e);
 e=dmu_bonus_hold(c->spa->spa_meta_objset,c->object,c,&db);if(e)return(e);
 if(db->db_size<sizeof(hdr))e=EINVAL;else memcpy(&hdr,db->db_data,sizeof(hdr));
 dmu_buf_rele(db,c);if(e)return(e);
 if(hdr.bt_begin>hdr.bt_end || hdr.bt_end>UINT64_MAX/sizeof(bptree_entry_phys_t)) return(EOVERFLOW);
 if(hdr.bt_end-hdr.bt_begin>ZB_QUEUE_LIMIT) return(E2BIG);
 if(hdr.bt_begin!=hdr.bt_end && hdr.bt_end*sizeof(bptree_entry_phys_t)>info.doi_max_offset) return(EINVAL);
 if(hdr.bt_begin==hdr.bt_end && (hdr.bt_bytes || hdr.bt_comp || hdr.bt_uncomp)) return(EINVAL);
 (void)fprintf(zi_out,"{\"record\":\"async_queue\",\"object\":\"%llu\","
     "\"begin\":\"%llu\",\"end\":\"%llu\",\"accounted_bytes\":\"%llu\","
     "\"accounted_comp\":\"%llu\",\"accounted_uncomp\":\"%llu\",%s}\n",
     (u_longlong_t)c->object,(u_longlong_t)hdr.bt_begin,(u_longlong_t)hdr.bt_end,
     (u_longlong_t)hdr.bt_bytes,(u_longlong_t)hdr.bt_comp,(u_longlong_t)hdr.bt_uncomp,zi_flags);
 e=zp_flush();if(e)return(e);
 for(c->index=hdr.bt_begin;c->index<hdr.bt_end;c->index++) {
  if(c->entries>=ZB_QUEUE_LIMIT)return(E2BIG);
  bptree_entry_phys_t entry;
  e=zb_read(c,c->index*sizeof(entry),sizeof(entry),info.doi_data_block_size,&entry);
  if(e)return(e);
  c->entries++;
  c->cutoff=entry.be_birth_txg;c->resume=entry.be_zb;
  boolean_t done=c->cutoff==UINT64_MAX;
  (void)fprintf(zi_out,"{\"record\":\"async_entry\",\"object\":\"%llu\","
      "\"entry_index\":\"%llu\",\"birth_cutoff\":\"%llu\",\"completed_sentinel\":%s,"
      "\"resume\":",(u_longlong_t)c->object,(u_longlong_t)c->index,
      (u_longlong_t)c->cutoff,done?"true":"false");
  zb_bookmark(&entry.be_zb);(void)fprintf(zi_out,",\"root_bp\":");zi_words(zi_out,&entry.be_bp);
  (void)fprintf(zi_out,",%s}\n",zi_flags);e=zp_flush();if(e)return(e);
  if(done)continue;
  if(c->cutoff>c->spa->spa_ubsync.ub_txg ||
      (!ZB_IS_ZERO(&c->resume) && (c->resume.zb_objset!=ZB_DESTROYED_OBJSET ||
      c->resume.zb_level!=0 || c->resume.zb_blkid==DMU_SPILL_BLKID ||
      (c->resume.zb_object==0 && c->resume.zb_blkid>UINT64_MAX/(1ULL << (DNODE_BLOCK_SHIFT-DNODE_SHIFT))))))return(EINVAL);
  if(BP_GET_TYPE(&entry.be_bp)!=DMU_OT_OBJSET || BP_GET_LEVEL(&entry.be_bp)) return(EINVAL);
  zbookmark_phys_t z;SET_BOOKMARK(&z,ZB_DESTROYED_OBJSET,ZB_ROOT_OBJECT,ZB_ROOT_LEVEL,ZB_ROOT_BLKID);
  e=zb_visit(c,NULL,&entry.be_bp,&z,0);if(e)return(e);
  if(!ZB_IS_ZERO(&c->resume))return(EINVAL);
 }
 return(0);
}
static int
zb_run(spa_t *spa)
{
 zb_ctx_t c={0};c.spa=spa;c.object=spa_get_dsl(spa)->dp_bptree_obj;
 boolean_t active=spa_feature_is_active(spa,SPA_FEATURE_ASYNC_DESTROY);
 int e=0;
 if(spa_writeable(spa)||spa_mode(spa)!=FREAD)e=EROFS;
 if(!e && (active != (c.object!=0)))e=EINVAL;
 (void)fprintf(zi_out,"{\"record\":\"async_scope\",\"object\":\"%llu\","
     "\"active\":%s,%s}\n",(u_longlong_t)c.object,active?"true":"false",zi_flags);
 if(!e)e=zp_flush();
 if(!e && active)e=zb_queue(&c);
 if(!e && (spa_writeable(spa)||spa_mode(spa)!=FREAD))e=EROFS;
 (void)fprintf(zi_out,"{\"record\":\"async_end\",\"object\":\"%llu\","
     "\"entry_index\":\"%llu\",\"entries\":\"%llu\",\"owners\":\"%llu\","
     "\"visits\":\"%llu\",\"reads\":\"%llu\",\"read_bytes\":\"%llu\","
     "\"async_complete\":%s,\"error\":%d,%s}\n",(u_longlong_t)c.object,
     (u_longlong_t)c.index,(u_longlong_t)c.entries,(u_longlong_t)c.owners,
     (u_longlong_t)c.visits,(u_longlong_t)c.reads,(u_longlong_t)c.bytes,e?"false":"true",e,zi_flags);
 int f=zp_flush();return(f?f:e);
}
