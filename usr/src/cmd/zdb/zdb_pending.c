/* CDDL: see usr/src/OPENSOLARIS.LICENSE. Included by zdb_inventory.c.
 * Checked read-only pending-free ownership; never payload verification/free.
 */
#include <sys/bpobj.h>
#include <sys/dsl_pool.h>
#define ZP_OBJECT_LIMIT 4096
#define ZP_ENTRY_LIMIT 100000ULL
#define ZP_BYTES_LIMIT (32ULL << 20)
#define ZP_BLOCK_LIMIT (16ULL << 20)
#define ZP_DEPTH_LIMIT 64

typedef struct zp_node {
 uint64_t object, parent, index, depth, root;
 unsigned queue;
} zp_node_t;
typedef struct zp_ctx {
 spa_t *spa;
 zp_node_t *nodes;
 uint64_t *ids;
 size_t count, head, id_count;
 uint64_t objects, entries, links, bytes, active_object, active_index;
 uint64_t qobjects[2], qentries[2], qlinks[2];
} zp_ctx_t;
static const char *zp_names[] = { "spa_deferred", "dsl_free" };

static int
zp_flush(void)
{
 return (fflush(zi_out) == 0 && !ferror(zi_out) ? 0 : EIO);
}

static int
zp_register(zp_ctx_t *c, uint64_t object)
{
 if (!object) return (EINVAL);
 if (c->id_count >= ZP_OBJECT_LIMIT) return (E2BIG);
 for (size_t i = 0; i < c->id_count; i++)
  if (c->ids[i] == object) return (EEXIST);
 c->ids[c->id_count++] = object;
 return (0);
}

/* Reserve a bounded read before it happens, including bonus bytes. */
static int
zp_budget(zp_ctx_t *c, uint64_t n)
{
 if (n > ZP_BYTES_LIMIT - c->bytes) return (E2BIG);
 c->bytes += n;
 return (0);
}

static int
zp_extent(zp_ctx_t *c, uint64_t obj, dmu_object_type_t type,
    uint64_t count, uint64_t unit, dmu_object_info_t *info)
{
 int e = dmu_object_info(c->spa->spa_meta_objset, obj, info);
 if (e) return (e);
 if (info->doi_type != type || !info->doi_data_block_size ||
     info->doi_data_block_size % unit ||
     info->doi_data_block_size > ZP_BLOCK_LIMIT) return (EINVAL);
 if (count > UINT64_MAX / unit || count * unit > info->doi_max_offset)
  return (EOVERFLOW);
 return (0);
}

static int
zp_read(zp_ctx_t *c, uint64_t obj, uint64_t off, uint64_t n, void *buf)
{
 uint64_t data = off;
 int e = zp_budget(c, n);
 if (e) return (e);
 e = dmu_offset_next(c->spa->spa_meta_objset, obj, B_FALSE, &data);
 if (e) return (e);
 if (data != off) return (EINVAL);
 return (dmu_read(c->spa->spa_meta_objset, obj, off, n, buf,
     DMU_READ_NO_PREFETCH));
}

static int
zp_owner(zp_ctx_t *c, const zp_node_t *node, uint64_t index,
    const blkptr_t *bp)
{
 int error = 0;
 const char *status = "ordinary";
 if (c->entries >= ZP_ENTRY_LIMIT) return (E2BIG);
 c->entries++; c->qentries[node->queue]++;
 /* bpobj records have erased checksums/fill (and embedded payloads). */
 if (!DMU_OT_IS_VALID(BP_GET_TYPE(bp)) || BP_USES_CRYPT(bp) ||
     BP_GET_DEDUP(bp) || BP_GET_TYPE(bp) == DMU_OT_DEDUP ||
     BP_GET_TYPE(bp) == DMU_OT_DDT_ZAP || BP_GET_TYPE(bp) == DMU_OT_DDT_STATS)
  error = ENOTSUP;
 else if (BP_IS_EMBEDDED(bp)) status = "embedded";
 else if (BP_IS_HOLE(bp) || BP_COUNT_GANG(bp)) error = ENOTSUP;
 if (!error && (bp->blk_birth > c->spa->spa_ubsync.ub_txg ||
     (!BP_IS_EMBEDDED(bp) && BP_PHYSICAL_BIRTH(bp) > c->spa->spa_ubsync.ub_txg)))
  error = EINVAL;
 if (!error && !BP_IS_EMBEDDED(bp)) {
  for (unsigned i = 0; i < SPA_DVAS_PER_BP; i++) {
   const dva_t *d = &bp->blk_dva[i];
   uint64_t a = DVA_GET_ASIZE(d), id = DVA_GET_VDEV(d);
   if (!a) {
    if (d->dva_word[0] || d->dva_word[1]) error = EINVAL;
    continue;
   }
   vdev_t *v = id < c->spa->spa_root_vdev->vdev_children ?
       c->spa->spa_root_vdev->vdev_child[id] : NULL;
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
 if (error) status = "unsupported";
 (void) fprintf(zi_out, "{\"record\":\"pending_owner\",\"queue\":\"%s\","
     "\"root_object\":\"%llu\",\"object\":\"%llu\",\"entry_index\":\"%llu\","
     "\"status\":\"%s\",\"error\":%d,\"payload_verifiable\":false,"
     "\"movable_candidate\":false,\"bp_words\":", zp_names[node->queue],
     (u_longlong_t)node->root, (u_longlong_t)node->object,
     (u_longlong_t)index, status, error);
 zi_words(zi_out, bp);
 (void) fprintf(zi_out, ",%s}\n", zi_flags);
 int e = zp_flush();
 return (e ? e : error);
}

static int
zp_object(zp_ctx_t *c, const zp_node_t *node)
{
 objset_t *os = c->spa->spa_meta_objset;
 dmu_object_info_t info;
 bpobj_phys_t hdr = { 0 };
 dmu_buf_t *bonus = NULL;
 c->active_object = node->object; c->active_index = 0;
 int e = zp_extent(c, node->object, DMU_OT_BPOBJ, 0, sizeof (blkptr_t), &info);
 if (e) return (e);
 if (info.doi_bonus_type != DMU_OT_BPOBJ_HDR ||
     (info.doi_bonus_size != BPOBJ_SIZE_V0 &&
     info.doi_bonus_size != BPOBJ_SIZE_V1 &&
     info.doi_bonus_size != sizeof (hdr))) return (EINVAL);
 e = zp_budget(c, info.doi_bonus_size);
 if (e) return (e);
 e = dmu_bonus_hold(os, node->object, c, &bonus);
 if (e) return (e);
 if (bonus->db_size < info.doi_bonus_size) e = EINVAL;
 else memcpy(&hdr, bonus->db_data, info.doi_bonus_size);
 dmu_buf_rele(bonus, c);
 if (e) return (e);
 if (hdr.bpo_num_blkptrs > ZP_ENTRY_LIMIT - c->entries ||
     hdr.bpo_num_subobjs > ZP_OBJECT_LIMIT - c->count) return (E2BIG);
 if (hdr.bpo_num_blkptrs > UINT64_MAX / sizeof (blkptr_t) ||
     hdr.bpo_num_blkptrs * sizeof (blkptr_t) > info.doi_max_offset ||
     (hdr.bpo_num_subobjs && !hdr.bpo_subobjs)) return (EINVAL);
 if (!hdr.bpo_num_blkptrs && !hdr.bpo_num_subobjs &&
     (hdr.bpo_bytes || hdr.bpo_comp || hdr.bpo_uncomp)) return (EINVAL);
 dmu_object_info_t subinfo = { 0 };
 if (hdr.bpo_subobjs) {
  e = zp_register(c, hdr.bpo_subobjs);
  if (!e) e = zp_extent(c, hdr.bpo_subobjs, DMU_OT_BPOBJ_SUBOBJ,
      hdr.bpo_num_subobjs, sizeof (uint64_t), &subinfo);
  if (e) return (e);
 }
 c->objects++; c->qobjects[node->queue]++;
 (void) fprintf(zi_out, "{\"record\":\"pending_object\",\"queue\":\"%s\","
     "\"root_object\":\"%llu\",\"object\":\"%llu\",\"parent_object\":\"%llu\","
     "\"parent_index\":\"%llu\",\"depth\":\"%llu\",\"entries\":\"%llu\","
     "\"subobject_list\":\"%llu\",\"subobjects\":\"%llu\",\"bonus_bytes\":\"%llu\","
     "\"accounted_bytes\":\"%llu\",\"accounted_comp\":\"%llu\","
     "\"accounted_uncomp\":\"%llu\",%s}\n", zp_names[node->queue],
     (u_longlong_t)node->root, (u_longlong_t)node->object,
     (u_longlong_t)node->parent, (u_longlong_t)node->index,
     (u_longlong_t)node->depth, (u_longlong_t)hdr.bpo_num_blkptrs,
     (u_longlong_t)hdr.bpo_subobjs, (u_longlong_t)hdr.bpo_num_subobjs,
     (u_longlong_t)info.doi_bonus_size, (u_longlong_t)hdr.bpo_bytes,
     (u_longlong_t)hdr.bpo_comp, (u_longlong_t)hdr.bpo_uncomp, zi_flags);
 e = zp_flush();
 if (e) return (e);
 /* One bounded block buffer; never recursively retain DMU buffers/locks. */
 uint64_t block = MAX(info.doi_data_block_size, subinfo.doi_data_block_size);
 void *buf = malloc(block);
 if (!buf) return (ENOMEM);
 for (unsigned pass = 0; pass < 2 && !e; pass++) {
  uint64_t count = pass ? hdr.bpo_num_subobjs : hdr.bpo_num_blkptrs;
  uint64_t unit = pass ? sizeof (uint64_t) : sizeof (blkptr_t);
  uint64_t obj = pass ? hdr.bpo_subobjs : node->object;
  uint64_t blksz = pass ? subinfo.doi_data_block_size : info.doi_data_block_size;
  for (uint64_t index = 0; index < count && !e; ) {
   uint64_t n = MIN(count - index, blksz / unit);
   c->active_object = obj; c->active_index = index;
   e = zp_read(c, obj, index * unit, n * unit, buf);
   if (e) break;
   for (uint64_t j = 0; j < n && !e; j++) {
    c->active_index = index + j;
    if (!pass) e = zp_owner(c, node, index + j, &((blkptr_t *)buf)[j]);
    else {
     uint64_t child = ((uint64_t *)buf)[j];
     if (node->depth >= ZP_DEPTH_LIMIT || c->count >= ZP_OBJECT_LIMIT) e = E2BIG;
     else e = zp_register(c, child);
     if (e) break;
     c->nodes[c->count++] = (zp_node_t){ child, node->object,
         index + j, node->depth + 1, node->root, node->queue };
     c->links++; c->qlinks[node->queue]++;
     (void) fprintf(zi_out, "{\"record\":\"pending_link\",\"queue\":\"%s\","
         "\"root_object\":\"%llu\",\"object\":\"%llu\",\"subobject_list\":\"%llu\","
         "\"entry_index\":\"%llu\",\"child_object\":\"%llu\",%s}\n",
         zp_names[node->queue], (u_longlong_t)node->root,
         (u_longlong_t)node->object, (u_longlong_t)hdr.bpo_subobjs,
         (u_longlong_t)(index + j), (u_longlong_t)child, zi_flags);
     e = zp_flush();
    }
   }
   index += n;
  }
 }
 free(buf);
 return (e);
}

static int
zp_run(spa_t *spa)
{
 zp_ctx_t c = { 0 }; c.spa = spa;
 uint64_t roots[2] = { spa->spa_deferred_bpobj.bpo_object, 0 };
 boolean_t applicable = spa_version(spa) >= SPA_VERSION_DEADLISTS;
 int e = 0;
 if (applicable && spa->spa_dsl_pool) roots[1] = spa->spa_dsl_pool->dp_free_bpobj.bpo_object;
 (void) fprintf(zi_out, "{\"record\":\"pending_scope\",\"spa_version\":\"%llu\","
     "\"spa_deferred_root\":\"%llu\",\"dsl_free_root\":\"%llu\","
     "\"dsl_free_applicable\":%s,%s}\n", (u_longlong_t)spa_version(spa),
     (u_longlong_t)roots[0], (u_longlong_t)roots[1], applicable ? "true" : "false", zi_flags);
 e = zp_flush();
 c.nodes = calloc(ZP_OBJECT_LIMIT, sizeof (*c.nodes));
 c.ids = calloc(ZP_OBJECT_LIMIT, sizeof (*c.ids));
 if (!e && (!c.nodes || !c.ids)) e = ENOMEM;
 if (!e && (spa_mode(spa) != FREAD || spa_writeable(spa))) e = EROFS;
 for (unsigned q = 0; q < (applicable ? 2 : 1) && !e; q++) {
  e = zp_register(&c, roots[q]);
  if (!e) c.nodes[c.count++] = (zp_node_t){ roots[q], 0, 0, 0, roots[q], q };
 }
 while (c.head < c.count && !e) {
  zp_node_t node = c.nodes[c.head++];
  e = zp_object(&c, &node);
 }
 if (!e && (spa_mode(spa) != FREAD || spa_writeable(spa))) e = EROFS;
 for (unsigned q = 0; q < 2; q++) {
  const char *status = q == 1 && !applicable ? "not_applicable" :
      e ? "incomplete" : c.qentries[q] ? "enumerated" : "empty";
  (void) fprintf(zi_out, "{\"record\":\"pending_queue_end\",\"queue\":\"%s\","
      "\"root_object\":\"%llu\",\"status\":\"%s\",\"error\":%d,"
      "\"objects\":\"%llu\",\"entries\":\"%llu\",\"links\":\"%llu\",%s}\n",
      zp_names[q], (u_longlong_t)roots[q], status, e,
      (u_longlong_t)c.qobjects[q], (u_longlong_t)c.qentries[q],
      (u_longlong_t)c.qlinks[q], zi_flags);
 }
 (void) fprintf(zi_out, "{\"record\":\"pending_end\",\"pending_complete\":%s,"
     "\"error\":%d,\"objects\":\"%llu\",\"entries\":\"%llu\",\"links\":\"%llu\","
     "\"read_bytes\":\"%llu\",\"active_object\":\"%llu\",\"active_index\":\"%llu\",%s}\n",
     e ? "false" : "true", e, (u_longlong_t)c.objects, (u_longlong_t)c.entries,
     (u_longlong_t)c.links, (u_longlong_t)c.bytes,
     (u_longlong_t)c.active_object, (u_longlong_t)c.active_index, zi_flags);
 free(c.nodes); free(c.ids);
 int output = zp_flush();
 return (output ? output : e);
}
