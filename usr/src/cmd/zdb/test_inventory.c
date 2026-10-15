/* No pools/devices opened: synthetic in-memory BPs exercise real serializer. */
#include "zdb_inventory.c"
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
