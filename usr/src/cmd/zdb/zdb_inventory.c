/* CDDL: see usr/src/OPENSOLARIS.LICENSE. Research diagnostic only. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/zfs_context.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/vdev_impl.h>
#include <sys/dmu_traverse.h>
#include <sys/dmu.h>
#include <libzutil.h>
#include "zdb_inventory.h"

#define ZI_LIMIT 1000000ULL
static FILE *zi_out;
static uint64_t zi_count, zi_unsupported;
static boolean_t zi_paired, zi_verify;
void zdb_inventory_verify_mode(void) { zi_verify = zi_paired = B_TRUE; }
void zdb_inventory_pair_mode(void) { zi_paired = B_TRUE; }
static const char *zi_flags = "\"coverage_complete\":false,"
    "\"payload_validation_performed\":false,\"evacuation_authorized\":false";

/* Byte escaping is reversible even for a non-UTF8 pool name. */
static void
zi_string(FILE *f, const char *s)
{
 (void) fputc('"', f);
 for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
  if (*p < 32 || *p >= 127 || *p == '"' || *p == '\\')
   (void) fprintf(f, "\\u%04x", *p);
  else (void) fputc(*p, f);
 }
 (void) fputc('"', f);
}

static void
zi_words(FILE *f, const blkptr_t *bp)
{
 uint64_t words[16];
 memcpy(words, bp, sizeof (words));
 (void) fputc('[', f);
 for (unsigned i = 0; i < 16; i++)
  (void) fprintf(f, "%s\"%016llx\"", i ? "," : "",
      (u_longlong_t)words[i]);
 (void) fputc(']', f);
}

#include "zdb_allocation.c"
#include "zdb_verify.c"
#include "zdb_pending.c"

/* Raw bias is retained even if the enum gains unsupported values. */
static void
zi_class(FILE *f, const vdev_t *v)
{
 const char *name = "unknown";
 if (v->vdev_islog) name = "log";
 else switch (v->vdev_alloc_bias) {
 case VDEV_BIAS_NONE: name = "normal"; break;
 case VDEV_BIAS_LOG: name = "log"; break;
 case VDEV_BIAS_SPECIAL: name = "special"; break;
 case VDEV_BIAS_DEDUP: name = "dedup"; break;
 }
 (void) fprintf(f, ",\"allocation_class\":");
 zi_string(f, name);
 (void) fprintf(f, ",\"alloc_bias\":%u,\"is_log\":%s",
     (unsigned)v->vdev_alloc_bias, v->vdev_islog ? "true" : "false");
}

static int
zi_record(spa_t *spa, zilog_t *zil, const blkptr_t *bp,
    const zbookmark_phys_t *zb, const dnode_phys_t *dn, void *arg)
{
 FILE *f = zi_out;
 const char *kind = "ordinary";
 boolean_t reject = B_FALSE;
 (void) zil; (void) arg;
 if (zi_count >= ZI_LIMIT) return (E2BIG);
 zi_count++;
 if (bp == NULL) {
  (void) fprintf(f, "{\"record\":\"dnode\",\"sequence\":\"%llu\","
      "\"objset\":\"%llu\",\"object\":\"%llu\",\"dnode_type\":%u,%s}\n",
      (u_longlong_t)zi_count, (u_longlong_t)zb->zb_objset,
      (u_longlong_t)zb->zb_object, dn ? dn->dn_type : 0, zi_flags);
  return (fflush(f) == 0 ? 0 : EIO);
 }
 /* Validate the type before ANY macro which may index dmu_ot[]. */
 boolean_t type_valid = DMU_OT_IS_VALID(BP_GET_TYPE(bp));
 boolean_t crypt = BP_USES_CRYPT(bp);
 if (!type_valid) { kind = "type_unsupported"; reject = B_TRUE; }
 else if (crypt) { kind = "protected_unsupported"; reject = B_TRUE; }
 else if (BP_IS_EMBEDDED(bp)) kind = "embedded";
 else if (BP_IS_HOLE(bp)) kind = "hole";
 else if (BP_COUNT_GANG(bp)) { kind = "gang_unsupported"; reject = B_TRUE; }
 else if (BP_GET_DEDUP(bp)) { kind = "dedup_unsupported"; reject = B_TRUE; }
 /* A failed slot must not suppress geometry on later independent slots. */
 const boolean_t bp_reject = reject;
 (void) fprintf(f, "{\"record\":\"bp\",\"sequence\":\"%llu\",\"classification\":",
     (u_longlong_t)zi_count);
 zi_string(f, kind);
 (void) fprintf(f, ",\"objset\":\"%llu\",\"object\":\"%llu\","
     "\"level\":\"%lld\",\"blkid\":\"%llu\",\"bp_words\":",
     (u_longlong_t)zb->zb_objset, (u_longlong_t)zb->zb_object,
     (longlong_t)zb->zb_level, (u_longlong_t)zb->zb_blkid);
 zi_words(f, bp);
 (void) fprintf(f, ",\"dnode_present\":%s,\"bp_type\":%u,\"bp_level\":%u,"
     "\"logical_birth\":\"%llu\",\"physical_birth\":\"%llu\","
     "\"lsize\":\"%llu\",\"psize\":\"%llu\",\"checksum_id\":%u,"
     "\"compression_id\":%u,\"metadata\":%s,\"dvas\":[",
     dn ? "true" : "false", (unsigned)BP_GET_TYPE(bp),
     (unsigned)BP_GET_LEVEL(bp), (u_longlong_t)bp->blk_birth,
     (u_longlong_t)BP_PHYSICAL_BIRTH(bp),
     (u_longlong_t)BP_GET_LSIZE(bp), (u_longlong_t)BP_GET_PSIZE(bp),
     (unsigned)BP_GET_CHECKSUM(bp), (unsigned)BP_GET_COMPRESS(bp),
     !type_valid ? "null" : (BP_IS_METADATA(bp) ? "true" : "false"));
 /* Protected DVA slots may contain non-DVA metadata. Raw words are retained. */
 if (type_valid && !crypt && !BP_IS_EMBEDDED(bp) && !BP_IS_HOLE(bp)) {
  unsigned emitted = 0;
  for (unsigned i = 0; i < SPA_DVAS_PER_BP; i++) {
   const dva_t *d = &bp->blk_dva[i];
   uint64_t a = DVA_GET_ASIZE(d), id = DVA_GET_VDEV(d);
   uint64_t off = DVA_GET_OFFSET(d), p = BP_GET_PSIZE(bp);
   vdev_t *vd = NULL;
   if (a == 0) continue;
   if (id < spa->spa_root_vdev->vdev_children)
    vd = spa->spa_root_vdev->vdev_child[id];
   (void) fprintf(f, "%s{\"slot\":%u,\"vdev_id\":\"%llu\","
       "\"offset\":\"%llu\",\"asize\":\"%llu\",\"gang\":%s",
       emitted++ ? "," : "", i, (u_longlong_t)id,
       (u_longlong_t)off, (u_longlong_t)a, DVA_GET_GANG(d) ? "true" : "false");
   boolean_t geometry = !bp_reject && vd && vd->vdev_id == id &&
       vd->vdev_alloc_bias == VDEV_BIAS_NONE && !vd->vdev_islog &&
       strcmp(vd->vdev_ops->vdev_op_type, "raidz") == 0 &&
       vd->vdev_ashift >= SPA_MINBLOCKSHIFT && vd->vdev_ashift <= 16 &&
       vd->vdev_nparity > 0 && vd->vdev_nparity < vd->vdev_children;
   if (vd) (void) fprintf(f, ",\"vdev_guid\":\"%llu\",\"ashift\":\"%llu\"",
       (u_longlong_t)vd->vdev_guid, (u_longlong_t)vd->vdev_ashift);
   if (vd) zi_class(f, vd);
   if (geometry) {
    uint64_t sec = 1ULL << vd->vdev_ashift;
    uint64_t n = (p + sec - 1) / sec, q = n * sec;
    uint64_t data = vd->vdev_children - vd->vdev_nparity;
    uint64_t sectors = n + vd->vdev_nparity * ((n + data - 1) / data);
    uint64_t unit = vd->vdev_nparity + 1;
    uint64_t expected = ((sectors + unit - 1) / unit) * unit * sec;
    boolean_t valid = p > 0 && a == expected && off % sec == 0 &&
        off <= UINT64_MAX - a && off + a <= vd->vdev_asize;
    (void) fprintf(f, ",\"qsize\":\"%llu\",\"expected_asize\":\"%llu\","
        "\"geometry_valid\":%s", (u_longlong_t)q,
        (u_longlong_t)expected, valid ? "true" : "false");
    if (!valid) reject = B_TRUE;
   } else {
    (void) fprintf(f, ",\"geometry_valid\":false");
    reject = B_TRUE;
   }
   /* No interval is exported: gang ASIZE is an aggregate, not a range. */
   (void) fputc('}', f);
  }
 }
 if (reject) zi_unsupported++;
 (void) fprintf(f, "],\"embedded_payload_bytes\":\"%llu\",\"dnode_type\":%u,"
     "\"dnode_indblkshift\":%u,\"dnode_datablkszsec\":%u,",
     (u_longlong_t)(BP_IS_EMBEDDED(bp) ? BPE_GET_PSIZE(bp) : 0),
     dn ? dn->dn_type : 0, dn ? dn->dn_indblkshift : 0,
     dn ? dn->dn_datablkszsec : 0);
 (void) fprintf(f, "\"unsupported\":%s,\"children_skipped\":%s,%s}\n",
     reject ? "true" : "false", reject ? "true" : "false", zi_flags);
 if (fflush(f) != 0) return (EIO);
 if (zi_verify) return (zv_record(spa, bp, zb, reject));
 return (reject ? TRAVERSE_VISIT_NO_CHILDREN : 0);
}

int
zdb_inventory_begin(const char *path)
{
 if (!traverse_no_zil_supported()) return (ENOTSUP);
 int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
 if (fd < 0) return (errno);
 zi_out = fdopen(fd, "w");
 if (!zi_out) { int e = errno; (void) close(fd); return (e); }
 (void) fprintf(zi_out, "{\"record\":\"begin\",\"schema\":\"%s\","
     "\"bp_encoding\":\"host-normalized-u64-hex\",\"integer_encoding\":"
     "\"64-bit values are decimal strings; BP words are hex\","
     "\"record_limit\":\"1000000\",\"time_limit_seconds\":300,"
     "\"source_base\":\"da512a7e2e6716980271582e436faef06a575f71\","
     "\"quiescence\":\"externally required; not proven by this diagnostic\","
     "\"attempted\":[\"birth-pruned MOS/dataset tree references\"],"
     "\"excluded\":[\"ZIL\",\"DDT ownership\",\"%s\","
     "\"async destroy\",\"checkpoint-only ownership\",\"removal accounting\","
     "\"allocation maps in -J only; paired -K captures independently\"],%s}\n",
     zi_verify ? "zdb-observed-verified-v2" :
     zi_paired ? "zdb-observed-allocation-v2" : "zdb-observed-bp-v1",
     zi_paired ? "deferred-free payload validation" : "deferred-free ownership and payload validation",
     zi_flags);
 return (fflush(zi_out) == 0 ? 0 : EIO);
}

int
zdb_inventory_run(spa_t *spa)
{
 if (spa_writeable(spa) || (spa_mode(spa) & FWRITE)) return (EROFS);
 uint64_t cut = spa->spa_ubsync.ub_txg;
 blkptr_t root = *spa_get_rootblkptr(spa);
 (void) fprintf(zi_out, "{\"record\":\"pool\",\"name\":");
 zi_string(zi_out, spa_name(spa));
 (void) fprintf(zi_out, ",\"guid\":\"%llu\",\"txg\":\"%llu\",\"root_bp\":",
     (u_longlong_t)spa_guid(spa), (u_longlong_t)spa->spa_ubsync.ub_txg);
 zi_words(zi_out, spa_get_rootblkptr(spa));
 (void) fprintf(zi_out, ",%s}\n", zi_flags);
 for (uint64_t i = 0; i < spa->spa_root_vdev->vdev_children; i++) {
  vdev_t *v = spa->spa_root_vdev->vdev_child[i];
  if (!v) continue;
  (void) fprintf(zi_out, "{\"record\":\"vdev\",\"id\":\"%llu\","
      "\"guid\":\"%llu\",\"ashift\":\"%llu\",\"children\":\"%llu\","
      "\"parity\":\"%llu\",\"type\":", (u_longlong_t)v->vdev_id,
      (u_longlong_t)v->vdev_guid, (u_longlong_t)v->vdev_ashift,
      (u_longlong_t)v->vdev_children, (u_longlong_t)v->vdev_nparity);
  zi_string(zi_out, v->vdev_ops->vdev_op_type);
  zi_class(zi_out, v);
  if (zi_paired) (void) fprintf(zi_out, ",\"asize\":\"%llu\",\"metaslab_count\":\"%llu\"",
      (u_longlong_t)v->vdev_asize, (u_longlong_t)v->vdev_ms_count);
  (void) fprintf(zi_out, ",%s}\n", zi_flags);
 }
 if (fflush(zi_out) != 0) return (EIO);
 int err = traverse_pool(spa, 0, TRAVERSE_PRE | TRAVERSE_NO_DECRYPT |
     TRAVERSE_NO_ZIL, zi_record, NULL);
 if (!err && zi_paired) err = za_run(spa);
 if (!err && zi_paired) err = zp_run(spa);
 if (!err && zi_paired && (cut != spa->spa_ubsync.ub_txg ||
     memcmp(&root, spa_get_rootblkptr(spa), sizeof (root)))) err = ESTALE;
 if (zi_paired) {
  (void) fprintf(zi_out, "{\"record\":\"cut_end\",\"txg\":\"%llu\",\"root_bp\":",
      (u_longlong_t)spa->spa_ubsync.ub_txg);
  zi_words(zi_out, spa_get_rootblkptr(spa));
  (void) fprintf(zi_out, ",%s}\n", zi_flags);
  if (fflush(zi_out)) err = EIO;
 }
 return (err);
}

int
zdb_inventory_end(int err)
{
 if (!zi_out) return (err ? err : EIO);
 if (zi_verify) err = zv_end(err);
 if (zi_paired) {
  (void) fprintf(zi_out, "{\"record\":\"allocation_end\",\"records\":\"%llu\",\"intervals\":\"%llu\",\"bytes\":\"%llu\",\"map_words\":\"%llu\",\"map_bytes\":\"%llu\",%s}\n",
      (u_longlong_t)za_records, (u_longlong_t)za_intervals,
      (u_longlong_t)za_total, (u_longlong_t)za_words,
      (u_longlong_t)za_read_bytes, zi_flags);
 }
 (void) fprintf(zi_out, "{\"record\":\"end\",\"export_complete\":%s,"
     "\"error\":%d,\"observed\":\"%llu\",\"unsupported\":\"%llu\",%s}\n",
     err ? "false" : "true", err, (u_longlong_t)zi_count,
     (u_longlong_t)zi_unsupported, zi_flags);
 if (fflush(zi_out) != 0 || ferror(zi_out)) err = EIO;
 if (fclose(zi_out) != 0) err = EIO;
 zi_out = NULL;
 return (err);
}

int
zdb_inventory_discover(const char *target, char **paths, int n, nvlist_t **cfg)
{
 importargs_t args = { 0 };
 args.paths = n;
 args.path = paths;
 args.can_be_active = B_FALSE;
 return (zpool_find_config(NULL, target, cfg, &args, &libzpool_config_ops));
}

int
zdb_inventory_import(const char *target, nvlist_t *cfg)
{
 uint64_t state;
 if (nvlist_lookup_uint64(cfg, ZPOOL_CONFIG_POOL_STATE, &state) != 0)
  return (EPROTO);
 if (state != POOL_STATE_EXPORTED) return (EBUSY);
 return (spa_import((char *)target, cfg, NULL, 0));
}
