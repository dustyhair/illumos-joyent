/* No pools/devices opened: synthetic in-memory BPs exercise real serializer. */
#include <stdio.h>
static int fail_flush, fail_close;
static int test_fflush(FILE *);
static int test_fclose(FILE *);
#define fflush test_fflush
#define fclose test_fclose
#include "zdb_inventory.c"
#undef fflush
#undef fclose
static int test_fflush(FILE *f) {
 if (fail_flush) { errno = ENOSPC; return (EOF); }
 return (fflush(f));
}
static int test_fclose(FILE *f) {
 int rc = fclose(f);
 if (fail_close) { errno = EIO; return (EOF); }
 return (rc);
}
static int discovered, imported, import_error;
int zpool_find_config(void *h, const char *target, nvlist_t **cfg,
    importargs_t *args, const pool_config_ops_t *ops) {
 (void) h; (void) target; (void) cfg; (void) ops;
 if (args->can_be_active || args->paths != 1 ||
     strcmp(args->path[0], "/fixture-only") != 0) return (EINVAL);
 discovered++;
 return (ENOENT);
}
int spa_import(const char *name, nvlist_t *cfg, nvlist_t *props, uint64_t flags) {
 (void) name; (void) cfg;
 if (props != NULL || flags != 0) return (EINVAL);
 imported++;
 return (import_error);
}

#include <stdlib.h>

/* Override only in this test executable; no real traversal or pool I/O. */
int
traverse_pool(spa_t *spa, uint64_t txg, int flags, blkptr_cb_t func, void *arg)
{
 (void) spa; (void) txg; (void) func; (void) arg;
 if (!(flags & TRAVERSE_NO_ZIL) || (flags & TRAVERSE_HARD)) return (EINVAL);
 return (EIO);
}

int
main(int argc, char **argv)
{
 if (argc != 3) return (99);
 spa_t spa = { 0 };
 vdev_t root = { 0 }, vd = { 0 };
 extern vdev_ops_t vdev_raidz_ops;
 vdev_ops_t ops = vdev_raidz_ops;
 vdev_t *children[] = { &vd };
 root.vdev_children = 1; root.vdev_child = children;
 spa.spa_root_vdev = &root;
 (void) strcpy(ops.vdev_op_type, "raidz");
 vd.vdev_ops = &ops; vd.vdev_top = &vd;
 vd.vdev_guid = UINT64_MAX; vd.vdev_children = 3;
 vd.vdev_nparity = 1; vd.vdev_ashift = 12; vd.vdev_asize = 1ULL << 40;
 blkptr_t bp = { 0 };
 zbookmark_phys_t zb = { UINT64_MAX, UINT64_MAX, 0, UINT64_MAX };
 BP_SET_LSIZE(&bp, 131072); BP_SET_PSIZE(&bp, 512);
 BP_SET_TYPE(&bp, DMU_OT_PLAIN_FILE_CONTENTS);
 bp.blk_birth = UINT64_MAX; bp.blk_phys_birth = UINT64_MAX - 1;
 DVA_SET_ASIZE(&bp.blk_dva[0], 8192);
 DVA_SET_OFFSET(&bp.blk_dva[0], 4096);
 int e = zdb_inventory_begin(argv[2]);
 if (e) return (e);
 if (strcmp(argv[1], "flush-failure") == 0) {
  fail_flush = 1;
  return (zdb_inventory_end(0));
 }
 if (strcmp(argv[1], "close-failure") == 0) {
  fail_close = 1;
  return (zdb_inventory_end(0));
 }
 if (strcmp(argv[1], "import-policy") == 0) {
  nvlist_t *cfg; char *paths[] = { "/fixture-only" };
  if (nvlist_alloc(&cfg, NV_UNIQUE_NAME, 0) != 0) return (85);
  if (zdb_inventory_discover("fixture", paths, 1, &cfg) != ENOENT ||
      discovered != 1 || imported != 0) return (84);
  if (zdb_inventory_import("fixture", cfg) != EPROTO || imported) return (83);
  if (nvlist_add_uint64(cfg, ZPOOL_CONFIG_POOL_STATE, POOL_STATE_ACTIVE)) return (82);
  if (zdb_inventory_import("fixture", cfg) != EBUSY || imported) return (81);
  (void) nvlist_remove_all(cfg, ZPOOL_CONFIG_POOL_STATE);
  if (nvlist_add_uint64(cfg, ZPOOL_CONFIG_POOL_STATE, POOL_STATE_EXPORTED)) return (80);
  if (zdb_inventory_import("fixture", cfg) != 0 || imported != 1) return (79);
  import_error = EIO;
  if (zdb_inventory_import("fixture", cfg) != EIO || imported != 2) return (78);
  nvlist_free(cfg);
  return (zdb_inventory_end(0));
 }
 if (strcmp(argv[1], "invalid-types") == 0 ||
     strcmp(argv[1], "protected-layouts") == 0) {
  boolean_t invalid = strcmp(argv[1], "invalid-types") == 0;
  for (unsigned crypt = 0; crypt < 2; crypt++)
   for (unsigned embedded = 0; embedded < 2; embedded++)
    for (unsigned hole = 0; hole < 2; hole++)
     for (unsigned level = 0; level < 3; level++) {
      blkptr_t x = bp;
      BP_SET_TYPE(&x, invalid ? 0x7f :
          (level == 0 ? DMU_OT_PLAIN_FILE_CONTENTS : DMU_OT_DNODE));
      BP_SET_LEVEL(&x, level == 2 ? 31 : level);
      BP_SET_CRYPT(&x, invalid ? crypt : 1);
      BP_SET_EMBEDDED(&x, embedded);
      if (hole) memset(x.blk_dva, 0, sizeof (x.blk_dva));
      if (zi_record(&spa, NULL, &x, &zb, NULL, NULL) != TRAVERSE_VISIT_NO_CHILDREN)
       return (77);
     }
  return (zdb_inventory_end(0));
 }
 if (strcmp(argv[1], "mixed-slots") == 0) {
  for (unsigned reverse = 0; reverse < 2; reverse++) {
   bp.blk_dva[1] = bp.blk_dva[0];
   DVA_SET_OFFSET(&bp.blk_dva[0], reverse ? 4096 : 512);
   DVA_SET_OFFSET(&bp.blk_dva[1], reverse ? 512 : 4096);
   if (zi_record(&spa, NULL, &bp, &zb, NULL, NULL) != TRAVERSE_VISIT_NO_CHILDREN)
    return (76);
  }
  return (zdb_inventory_end(0));
 }
 if (strcmp(argv[1], "allocation-classes") == 0) {
  for (unsigned bias = 0; bias < 5; bias++)
   for (unsigned log = 0; log < 2; log++) {
    vd.vdev_alloc_bias = bias; vd.vdev_islog = log;
    int expected = bias == 0 && log == 0 ? 0 : TRAVERSE_VISIT_NO_CHILDREN;
    if (zi_record(&spa, NULL, &bp, &zb, NULL, NULL) != expected) return (75);
    if (zdb_inventory_run(&spa) != EIO) return (74);
   }
  return (zdb_inventory_end(0));
 }
 if (strcmp(argv[1], "traversal-error") == 0) {
  int err = zdb_inventory_run(&spa);
  if (err != EIO) return (89);
  return (zdb_inventory_end(err) == EIO ? 0 : 88);
 }
 if (strcmp(argv[1], "errors") == 0) {
  zi_count = ZI_LIMIT;
  if (zi_record(&spa, NULL, &bp, &zb, NULL, NULL) != E2BIG) return (90);
  zi_count = 0;
  (void) close(fileno(zi_out));
  if (zi_record(&spa, NULL, &bp, &zb, NULL, NULL) != EIO) return (91);
  return (zdb_inventory_end(EIO) == EIO ? 0 : 92);
 }
 if (strcmp(argv[1], "terminal-error") == 0)
  return (zdb_inventory_end(EIO) == EIO ? 0 : 93);
 (void) fprintf(zi_out, "{\"record\":\"escape-test\",\"text\":");
 zi_string(zi_out, "quote\" slash\\ newline\n\001\377");
 (void) fprintf(zi_out, "}\n");
 for (uint64_t p = 512; p <= 131072; p += 512) {
  uint64_t expected = vdev_psize_to_asize(&vd, p);
  BP_SET_PSIZE(&bp, p); DVA_SET_ASIZE(&bp.blk_dva[0], expected);
  if (zi_record(&spa, NULL, &bp, &zb, NULL, NULL) != 0) return (94);
 }
 BP_SET_PSIZE(&bp, 512); DVA_SET_ASIZE(&bp.blk_dva[0], 8192);
 zb.zb_object = 123;
 (void) zi_record(&spa, NULL, &bp, &zb, NULL, NULL);
 BP_SET_TYPE(&bp, DMU_OT_DNODE);
 if (zi_record(&spa, NULL, &bp, &zb, NULL, NULL) != 0) return (95);
 BP_SET_TYPE(&bp, DMU_OT_PLAIN_FILE_CONTENTS);
 DVA_SET_GANG(&bp.blk_dva[0], 1);
 if (zi_record(&spa, NULL, &bp, &zb, NULL, NULL) != TRAVERSE_VISIT_NO_CHILDREN) return (96);
 DVA_SET_GANG(&bp.blk_dva[0], 0);
 BP_SET_DEDUP(&bp, 1); (void) zi_record(&spa, NULL, &bp, &zb, NULL, NULL);
 BP_SET_DEDUP(&bp, 0); BP_SET_CRYPT(&bp, 1);
 (void) zi_record(&spa, NULL, &bp, &zb, NULL, NULL);
 BP_SET_CRYPT(&bp, 0); DVA_SET_ASIZE(&bp.blk_dva[0], 4096);
 (void) zi_record(&spa, NULL, &bp, &zb, NULL, NULL);
 DVA_SET_ASIZE(&bp.blk_dva[0], 8192); DVA_SET_OFFSET(&bp.blk_dva[0], 512);
 (void) zi_record(&spa, NULL, &bp, &zb, NULL, NULL);
 DVA_SET_OFFSET(&bp.blk_dva[0], UINT64_MAX - 4095);
 (void) zi_record(&spa, NULL, &bp, &zb, NULL, NULL);
 DVA_SET_VDEV(&bp.blk_dva[0], 17);
 (void) zi_record(&spa, NULL, &bp, &zb, NULL, NULL);
 (void) memset(&bp, 0, sizeof (bp));
 (void) zi_record(&spa, NULL, &bp, &zb, NULL, NULL);
 BP_SET_EMBEDDED(&bp, 1); BPE_SET_LSIZE(&bp, 32); BPE_SET_PSIZE(&bp, 16);
 (void) zi_record(&spa, NULL, &bp, &zb, NULL, NULL);
 dnode_phys_t dn = { 0 };
 dn.dn_type = DMU_OT_DNODE;
 (void) zi_record(&spa, NULL, NULL, &zb, &dn, NULL);
 return (zdb_inventory_end(0));
}
