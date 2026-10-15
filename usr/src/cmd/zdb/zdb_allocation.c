/* CDDL: see usr/src/OPENSOLARIS.LICENSE. Included by zdb_inventory.c.
 * Private diagnostic state only; never modify allocator trees or claim BPs.
 */
#include <stdlib.h>
#include <sys/metaslab_impl.h>
#include <sys/space_map.h>
#include <sys/spa_log_spacemap.h>
#include <sys/zfeature.h>

#define ZA_MS_LIMIT 4096
#define ZA_BYTES_LIMIT (64ULL << 20)
#define ZA_WORD_LIMIT 1000000ULL
#define ZA_BLOCK_LIMIT (16ULL << 20)
static uint64_t za_records, za_intervals, za_total, za_words, za_read_bytes;

typedef struct za_ms {
 vdev_t *vd;
 metaslab_t *ms;
 range_tree_t *tree;
} za_ms_t;
typedef struct za_ctx {
 za_ms_t *maps;
 size_t count;
 za_ms_t *base;
 uint64_t txg, selected, object, ordinal;
 boolean_t log;
} za_ctx_t;

/* Validated block parser; two-word records may NOT cross a block boundary. */
static int
za_decode(const uint64_t *words, size_t count, uint64_t start,
    uint64_t size, unsigned shift, sm_cb_t cb, void *arg)
{
 if (shift > 63 || size == 0 || start > UINT64_MAX - size ||
     (start & ((1ULL << shift) - 1))) return (EINVAL);
 for (size_t i = 0; i < count; i++) {
  uint64_t w = words[i], off, run;
  space_map_entry_t e;
  if (++za_words > ZA_WORD_LIMIT) return (E2BIG);
  if (SM_PREFIX_DECODE(w) == SM_DEBUG_PREFIX) continue;
  if ((w >> 63) == 0) {
   off = SM_OFFSET_DECODE(w); run = SM_RUN_DECODE(w);
   e.sme_type = SM_TYPE_DECODE(w); e.sme_vdev = SM_NO_VDEVID;
  } else {
   if (SM_PREFIX_DECODE(w) != SM2_PREFIX || ((w >> 60) & 3) ||
       i + 1 >= count) return (EINVAL);
   if (++za_words > ZA_WORD_LIMIT) return (E2BIG);
   run = SM2_RUN_DECODE(w); e.sme_vdev = SM2_VDEV_DECODE(w);
   w = words[++i]; off = SM2_OFFSET_DECODE(w);
   e.sme_type = SM2_TYPE_DECODE(w);
  }
  if (off > (UINT64_MAX >> shift) || run > (UINT64_MAX >> shift))
   return (EOVERFLOW);
  off <<= shift; run <<= shift;
  if (run == 0 || off >= size || run > size - off ||
      off > UINT64_MAX - start) return (ERANGE);
  e.sme_offset = start + off; e.sme_run = run;
  int err = cb(&e, arg);
  if (err) return (err);
 }
 return (0);
}

static int
za_finish_record(void)
{
 if (++za_records > ZI_LIMIT) return (E2BIG);
 return (fflush(zi_out) == 0 && !ferror(zi_out) ? 0 : EIO);
}

static int
za_apply(range_tree_t *rt, uint64_t start, uint64_t size, maptype_t type)
{
 uint64_t os, ol;
 if (!size || start > UINT64_MAX - size) return (ERANGE);
 if (type == SM_ALLOC) {
  if (range_tree_find_in(rt, start, size, &os, &ol)) return (EEXIST);
  range_tree_add(rt, start, size);
 } else if (type == SM_FREE) {
  if (!range_tree_contains(rt, start, size)) return (ENOENT);
  range_tree_remove(rt, start, size);
 } else return (EINVAL);
 return (0);
}

static int
za_entry(space_map_entry_t *e, void *arg)
{
 za_ctx_t *ctx = arg;
 za_ms_t *m = ctx->base;
 if (ctx->log) {
  m = NULL;
  if (e->sme_vdev == SM_NO_VDEVID || ctx->txg == 0 ||
      ctx->txg > ctx->selected) return (EINVAL);
  for (size_t i = 0; i < ctx->count; i++) {
   za_ms_t *p = &ctx->maps[i];
   if (p->vd->vdev_id == e->sme_vdev &&
       e->sme_offset >= p->ms->ms_start &&
       e->sme_offset - p->ms->ms_start < p->ms->ms_size) {
    m = p; break;
   }
  }
 } else if (e->sme_vdev != SM_NO_VDEVID && m &&
     e->sme_vdev != m->vd->vdev_id) return (EINVAL);
 if (!m || e->sme_offset < m->ms->ms_start ||
     e->sme_run > m->ms->ms_size ||
     e->sme_offset - m->ms->ms_start > m->ms->ms_size - e->sme_run ||
     (e->sme_offset & ((1ULL << m->vd->vdev_ashift)-1)) ||
     (e->sme_run & ((1ULL << m->vd->vdev_ashift)-1))) return (ERANGE);
 boolean_t applied = !ctx->log ||
     ctx->txg >= metaslab_unflushed_txg(m->ms);
 int err = applied ? za_apply(m->tree, e->sme_offset, e->sme_run,
     e->sme_type) : 0;
 if (err) return (err);
 (void) fprintf(zi_out, "{\"record\":\"allocation_op\",\"source\":\"%s\","
     "\"map_object\":\"%llu\",\"log_txg\":\"%llu\",\"ordinal\":\"%llu\","
     "\"vdev_id\":\"%llu\",\"metaslab\":\"%llu\",\"start\":\"%llu\","
     "\"length\":\"%llu\",\"op\":\"%s\",\"applied\":%s,%s}\n",
     ctx->log ? "log" : "base", (u_longlong_t)ctx->object,
     (u_longlong_t)ctx->txg, (u_longlong_t)++ctx->ordinal,
     (u_longlong_t)m->vd->vdev_id, (u_longlong_t)m->ms->ms_id,
     (u_longlong_t)e->sme_offset, (u_longlong_t)e->sme_run,
     e->sme_type == SM_ALLOC ? "alloc" : "free",
     applied ? "true" : "false", zi_flags);
 return (za_finish_record());
}

static int
za_read_map(space_map_t *sm, za_ctx_t *ctx)
{
 uint64_t length = space_map_length(sm);
 dmu_object_info_t info;
 int info_err = dmu_object_info(sm->sm_os, space_map_object(sm), &info);
 if (info_err) return (info_err);
 if (length > info.doi_max_offset || info.doi_data_block_size != sm->sm_blksz)
  return (EINVAL);
 if (!sm->sm_blksz || sm->sm_blksz % 8 || length % 8 ||
     sm->sm_blksz > ZA_BLOCK_LIMIT ||
     length > ZA_BYTES_LIMIT - za_read_bytes) return (E2BIG);
 za_read_bytes += length;
 uint64_t *buf = malloc(sm->sm_blksz);
 if (!buf) return (ENOMEM);
 int err = 0;
 for (uint64_t pos = 0; pos < length; ) {
  uint64_t n = MIN(length - pos, sm->sm_blksz);
  uint64_t data_offset = pos;
  err = dmu_offset_next(sm->sm_os, space_map_object(sm), B_FALSE, &data_offset);
  if (err) break;
  if (data_offset != pos) { err = EINVAL; break; }
  err = dmu_read(sm->sm_os, space_map_object(sm), pos, n, buf,
      DMU_READ_NO_PREFETCH);
  if (err) break;
  err = za_decode(buf, n / 8, sm->sm_start, sm->sm_size,
      sm->sm_shift, za_entry, ctx);
  if (err) break;
  pos += n;
 }
 free(buf);
 return (err);
}

static int
za_map_record(za_ctx_t *ctx, space_map_t *sm)
{
 (void) fprintf(zi_out, "{\"record\":\"allocation_map\",\"source\":\"%s\","
     "\"map_object\":\"%llu\",\"log_txg\":\"%llu\",\"length\":\"%llu\","
     "\"shift\":%u,%s}\n", ctx->log ? "log" : "base",
     (u_longlong_t)ctx->object, (u_longlong_t)ctx->txg,
     (u_longlong_t)(sm ? space_map_length(sm) : 0),
     sm ? sm->sm_shift : 0, zi_flags);
 return (za_finish_record());
}

static int
za_emit(za_ms_t *m)
{
 zfs_btree_index_t where;
 for (range_seg_t *rs = zfs_btree_first(&m->tree->rt_root, &where);
     rs; rs = zfs_btree_next(&m->tree->rt_root, &where, &where)) {
  uint64_t start = rs_get_start(rs, m->tree);
  uint64_t size = rs_get_end(rs, m->tree) - start;
  if (za_total > UINT64_MAX - size) return (EOVERFLOW);
  za_total += size; za_intervals++;
  (void) fprintf(zi_out, "{\"record\":\"allocation\",\"vdev_id\":\"%llu\","
      "\"vdev_guid\":\"%llu\",\"metaslab\":\"%llu\",\"start\":\"%llu\","
      "\"length\":\"%llu\",%s}\n", (u_longlong_t)m->vd->vdev_id,
      (u_longlong_t)m->vd->vdev_guid, (u_longlong_t)m->ms->ms_id,
      (u_longlong_t)start, (u_longlong_t)size, zi_flags);
  int err = za_finish_record();
  if (err) return (err);
 }
 return (0);
}

static int
za_run(spa_t *spa)
{
 za_ctx_t ctx = { 0 };
 int err = 0;
 ctx.selected = spa->spa_ubsync.ub_txg;
 if (spa_writeable(spa) || (spa_mode(spa) & FWRITE)) return (EROFS);
 /* Do not subtract checkpoint/removal allocations to disguise omissions. */
 boolean_t checkpoint = spa_feature_is_active(spa,
     SPA_FEATURE_POOL_CHECKPOINT) || spa->spa_checkpoint_txg != 0 ||
     spa_get_checkpoint_space(spa) != 0;
 boolean_t removal = spa_feature_is_active(spa, SPA_FEATURE_DEVICE_REMOVAL) ||
     spa->spa_vdev_removal != NULL;
 (void) fprintf(zi_out, "{\"record\":\"allocation_scope\","
     "\"checkpoint_unsupported\":%s,\"removal_unsupported\":%s,"
     "\"owner_obligations\":[\"DDT:not_enumerated\",\"deferred_free:not_enumerated\","
     "\"async_destroy:not_enumerated\",\"ZIL:not_enumerated\"],"
     "\"allocation_adjustments\":\"none\",%s}\n",
     checkpoint ? "true" : "false", removal ? "true" : "false", zi_flags);
 if ((err = za_finish_record()) != 0) return (err);
 if (checkpoint || removal) return (ENOTSUP);
 ctx.maps = calloc(ZA_MS_LIMIT, sizeof (*ctx.maps));
 if (!ctx.maps) return (ENOMEM);
 for (uint64_t i = 0; i < spa->spa_root_vdev->vdev_children; i++) {
  vdev_t *v = spa->spa_root_vdev->vdev_child[i];
  if (!v || v->vdev_id != i || v->vdev_islog || v->vdev_removing ||
      v->vdev_alloc_bias != VDEV_BIAS_NONE ||
      strcmp(v->vdev_ops->vdev_op_type, "raidz") != 0 ||
      v->vdev_ashift < 9 || v->vdev_ashift > 16 ||
      v->vdev_nparity == 0 || v->vdev_nparity >= v->vdev_children ||
      !v->vdev_ms_count || !v->vdev_ms) { err = ENOTSUP; goto out; }
  uint64_t last = 0;
  for (uint64_t j = 0; j < v->vdev_ms_count; j++) {
   metaslab_t *ms = v->vdev_ms[j];
   if (ctx.count == ZA_MS_LIMIT) { err = E2BIG; goto out; }
   if (!ms || ms->ms_id != j || !ms->ms_size ||
       ms->ms_start > v->vdev_asize || ms->ms_size > v->vdev_asize-ms->ms_start ||
       ms->ms_start < last ||
       (ms->ms_start & ((1ULL << v->vdev_ashift)-1)) ||
       (ms->ms_size & ((1ULL << v->vdev_ashift)-1)) ||
       metaslab_unflushed_txg(ms) > ctx.selected) {
    err = EINVAL; goto out;
   }
   last = ms->ms_start + ms->ms_size;
   za_ms_t *m = &ctx.maps[ctx.count++]; m->vd = v; m->ms = ms;
   m->tree = range_tree_create(NULL, RANGE_SEG64, NULL, 0, 0);
   ctx.base = m; ctx.object = ms->ms_sm ? space_map_object(ms->ms_sm) : 0;
   ctx.ordinal = 0;
   (void) fprintf(zi_out, "{\"record\":\"metaslab\",\"vdev_id\":\"%llu\","
       "\"id\":\"%llu\",\"start\":\"%llu\",\"length\":\"%llu\","
       "\"unflushed_txg\":\"%llu\",\"map_object\":\"%llu\",%s}\n",
       (u_longlong_t)i, (u_longlong_t)j, (u_longlong_t)ms->ms_start,
       (u_longlong_t)ms->ms_size, (u_longlong_t)metaslab_unflushed_txg(ms),
       (u_longlong_t)ctx.object, zi_flags);
   if ((err = za_finish_record()) || (err = za_map_record(&ctx, ms->ms_sm))) goto out;
   if (ms->ms_sm) {
    space_map_t *sm = ms->ms_sm;
    if (sm->sm_start != ms->ms_start || sm->sm_size != ms->ms_size ||
        sm->sm_shift != v->vdev_ashift) { err = EINVAL; goto out; }
    if ((err = za_read_map(sm, &ctx))) goto out;
    int64_t allocated = space_map_allocated(sm);
    if (allocated < 0 || (uint64_t)allocated != range_tree_space(m->tree)) {
     err = EINVAL; goto out;
    }
   }
  }
 }
 ctx.log = B_TRUE; ctx.base = NULL;
 uint64_t previous = 0;
 if (spa_feature_is_active(spa, SPA_FEATURE_LOG_SPACEMAP)) {
  for (spa_log_sm_t *s = avl_first(&spa->spa_sm_logs_by_txg); s;
      s = AVL_NEXT(&spa->spa_sm_logs_by_txg, s)) {
   space_map_t *sm = NULL;
   if (s->sls_txg <= previous || s->sls_txg > ctx.selected || !s->sls_sm_obj) {
    err = EINVAL; goto out;
   }
   previous = ctx.txg = s->sls_txg; ctx.object = s->sls_sm_obj; ctx.ordinal = 0;
   err = space_map_open(&sm, spa_meta_objset(spa), s->sls_sm_obj,
       0, UINT64_MAX, SPA_MINBLOCKSHIFT);
   if (err) goto out;
   err = za_map_record(&ctx, sm);
   if (!err) err = za_read_map(sm, &ctx);
   space_map_close(sm);
   if (err) goto out;
  }
 }
 for (size_t i = 0; i < ctx.count && !err; i++) err = za_emit(&ctx.maps[i]);
out:
 for (size_t i = 0; i < ctx.count; i++) {
  range_tree_vacate(ctx.maps[i].tree, NULL, NULL);
  range_tree_destroy(ctx.maps[i].tree);
 }
 free(ctx.maps);
 return (err);
}
