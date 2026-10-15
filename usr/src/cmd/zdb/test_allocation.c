/* Pool-free native tests of the production checked parser and replay. */
#include <stdio.h>
static int output_error;
static int alloc_test_flush(FILE *);
#define fflush alloc_test_flush
#include "zdb_inventory.c"
#undef fflush
static int alloc_test_flush(FILE *f) {
 if (output_error) return (EOF);
 return (fflush(f));
}
static uint64_t input_words[8];
static int read_error, hole_error;
int dmu_offset_next(objset_t *os, uint64_t object, boolean_t hole, uint64_t *off) {
 (void) os; (void) object; (void) hole;
 if (hole_error) *off += 16;
 return (0);
}
int dmu_object_info(objset_t *os, uint64_t object, dmu_object_info_t *info) {
 (void) os; (void) object; memset(info, 0, sizeof (*info));
 info->doi_max_offset = sizeof (input_words); info->doi_data_block_size = 16;
 return (0);
}
int dmu_read(objset_t *os, uint64_t object, uint64_t off, uint64_t size,
    void *buf, uint32_t flags) {
 (void) os; (void) object; (void) flags;
 if (read_error) return (read_error);
 if (off > sizeof (input_words) || size > sizeof (input_words)-off)
  return (EIO);
 memcpy(buf, (char *)input_words + off, size);
 return (0);
}
static int features, open_error;
static space_map_t mock_log;
boolean_t spa_feature_is_active(spa_t *spa, spa_feature_t f) {
 (void) spa;
 return ((f == SPA_FEATURE_LOG_SPACEMAP && (features & 1)) ||
     (f == SPA_FEATURE_POOL_CHECKPOINT && (features & 2)) ||
     (f == SPA_FEATURE_DEVICE_REMOVAL && (features & 4)));
}
int space_map_open(space_map_t **sm, objset_t *os, uint64_t obj,
    uint64_t start, uint64_t size, uint8_t shift) {
 (void) os; (void) obj; (void) start; (void) size; (void) shift;
 if (open_error) return (open_error);
 *sm = &mock_log; return (0);
}
void space_map_close(space_map_t *sm) { (void) sm; }
static int log_compare(const void *left, const void *right) {
 const spa_log_sm_t *a = left, *b = right;
 return ((a->sls_txg > b->sls_txg) - (a->sls_txg < b->sls_txg));
}
#define CHECK(x) do { if (!(x)) { (void) fprintf(stderr, \
    "FAIL line %d: %s\n", __LINE__, #x); return (1); } passed++; } while (0)
static int visits;
static space_map_entry_t last_entry;
static int collect(space_map_entry_t *e, void *arg) {
 (void) arg; visits++; last_entry = *e; return (0);
}
static int reject_entry(space_map_entry_t *e, void *arg) {
 (void) e; (void) arg; return (EIO);
}
int main(int argc, char **argv) {
 if (argc != 2) return (99);
 unsigned passed = 0;
 zfs_btree_init();
 uint64_t w[4] = { SM_OFFSET_ENCODE(2) | SM_RUN_ENCODE(3), 0, 0, 0 };
 CHECK(za_decode(w, 1, 4096, 32768, 9, collect, NULL) == 0);
 CHECK(visits == 1 && last_entry.sme_offset == 5120 && last_entry.sme_run == 1536);
 w[0] = SM_PREFIX_ENCODE(SM2_PREFIX) | SM2_RUN_ENCODE(8) | SM2_VDEV_ENCODE(7);
 w[1] = SM2_OFFSET_ENCODE(16) | SM2_TYPE_ENCODE(SM_FREE);
 CHECK(za_decode(w, 2, 0, 32768, 9, collect, NULL) == 0);
 CHECK(last_entry.sme_vdev == 7 && last_entry.sme_run == 4096 && last_entry.sme_type == SM_FREE);
 CHECK(za_decode(w, 1, 0, 32768, 9, collect, NULL) == EINVAL);
 w[0] |= 1ULL << 60;
 CHECK(za_decode(w, 2, 0, 32768, 9, collect, NULL) == EINVAL);
 w[0] &= ~(1ULL << 60); w[1] = UINT64_MAX;
 CHECK(za_decode(w, 2, 0, UINT64_MAX, 9, collect, NULL) == EOVERFLOW);
 CHECK(za_decode(w, 2, UINT64_MAX, 4096, 9, collect, NULL) == EINVAL);
 CHECK(za_decode(w, 2, 0, 4096, 64, collect, NULL) == EINVAL);
 w[0] = SM_OFFSET_ENCODE(7) | SM_RUN_ENCODE(2);
 CHECK(za_decode(w, 1, 0, 4096, 9, collect, NULL) == ERANGE);
 w[0] = 0;
 CHECK(za_decode(w, 1, 0, 4096, 9, reject_entry, NULL) == EIO);
 w[0] = SM_PREFIX_ENCODE(SM_DEBUG_PREFIX);
 int old_visits = visits;
 CHECK(za_decode(w, 1, 0, 4096, 9, collect, NULL) == 0 && visits == old_visits);
 za_words = ZA_WORD_LIMIT;
 CHECK(za_decode(w, 1, 0, 4096, 9, collect, NULL) == E2BIG);
 za_words = 0;
 range_tree_t *tree = range_tree_create(NULL, RANGE_SEG64, NULL, 0, 0);
 CHECK(za_apply(tree, 4096, 8192, SM_ALLOC) == 0);
 CHECK(za_apply(tree, 8192, 8192, SM_ALLOC) == EEXIST);
 CHECK(za_apply(tree, 0, 8192, SM_FREE) == ENOENT);
 CHECK(za_apply(tree, 4096, 4096, SM_FREE) == 0);
 CHECK(range_tree_space(tree) == 4096);
 CHECK(za_apply(tree, UINT64_MAX, 2, SM_ALLOC) == ERANGE);
 CHECK(za_apply(tree, 0, 0, SM_ALLOC) == ERANGE);
 CHECK(za_apply(tree, 0, 4096, (maptype_t)8) == EINVAL);
 range_tree_vacate(tree, NULL, NULL);
 zdb_inventory_pair_mode(); CHECK(zdb_inventory_begin(argv[1]) == 0);
 vdev_t vd = { 0 }; metaslab_t ms = { 0 };
 vd.vdev_id = 7; vd.vdev_guid = 123; vd.vdev_ashift = 12;
 ms.ms_start = 0; ms.ms_size = 32768; ms.ms_unflushed_txg = 20;
 za_ms_t m = { &vd, &ms, tree };
 za_ctx_t ctx = { 0 }; ctx.maps = &m; ctx.count = 1; ctx.base = &m;
 ctx.selected = 30; ctx.object = 99;
 space_map_entry_t e = { SM_ALLOC, SM_NO_VDEVID, 4096, 4096 };
 CHECK(za_entry(&e, &ctx) == 0);
 ctx.base = NULL; ctx.log = B_TRUE; ctx.txg = 19; e.sme_vdev = 7;
 CHECK(za_entry(&e, &ctx) == 0 && range_tree_space(tree) == 4096);
 ctx.txg = 20; e.sme_offset = 8192;
 CHECK(za_entry(&e, &ctx) == 0 && range_tree_space(tree) == 8192);
 e.sme_type = SM_FREE;
 CHECK(za_entry(&e, &ctx) == 0 && range_tree_space(tree) == 4096);
 CHECK(za_entry(&e, &ctx) == ENOENT);
 e.sme_type = SM_ALLOC; ctx.txg = 31;
 CHECK(za_entry(&e, &ctx) == EINVAL);
 ctx.txg = 0;
 CHECK(za_entry(&e, &ctx) == EINVAL);
 ctx.txg = 21; e.sme_vdev = 8;
 CHECK(za_entry(&e, &ctx) == ERANGE);
 e.sme_vdev = SM_NO_VDEVID;
 CHECK(za_entry(&e, &ctx) == EINVAL);
 e.sme_vdev = 7; e.sme_offset = 28672; e.sme_run = 8192;
 CHECK(za_entry(&e, &ctx) == ERANGE);
 e.sme_run = 512; e.sme_offset = 512;
 CHECK(za_entry(&e, &ctx) == ERANGE);
 CHECK(za_emit(&m) == 0 && za_intervals == 1 && za_total == 4096);
 space_map_phys_t phys = { 0 }; space_map_t sm = { 0 };
 sm.sm_phys = &phys; sm.sm_blksz = 16; sm.sm_size = 32768; sm.sm_shift = 9;
 phys.smp_length = 7;
 CHECK(za_read_map(&sm, &ctx) == E2BIG);
 phys.smp_length = 8; input_words[0] = SM_PREFIX_ENCODE(SM2_PREFIX);
 CHECK(za_read_map(&sm, &ctx) == EINVAL);
 phys.smp_length = 16; hole_error = 1;
 CHECK(za_read_map(&sm, &ctx) == EINVAL); hole_error = 0;
 read_error = EIO;
 CHECK(za_read_map(&sm, &ctx) == EIO);
 read_error = 0; sm.sm_blksz = ZA_BLOCK_LIMIT + 8;
 CHECK(za_read_map(&sm, &ctx) == EINVAL);
 sm.sm_blksz = 16; za_read_bytes = ZA_BYTES_LIMIT;
 CHECK(za_read_map(&sm, &ctx) == E2BIG);
 output_error = 1;
 CHECK(za_finish_record() == EIO);
 output_error = 0;
 /* Execute the complete allocation driver with fake metadata and stub reads. */
 za_read_bytes = za_words = 0;
 spa_t spa = { 0 }; vdev_t root = { 0 };
 vdev_t *children[] = { &vd }; metaslab_t *metaslabs[] = { &ms };
 extern vdev_ops_t vdev_raidz_ops;
 root.vdev_children = 1; root.vdev_child = children;
 spa.spa_root_vdev = &root; spa.spa_mode = FREAD; spa.spa_ubsync.ub_txg = 30;
 vd.vdev_id = 0; vd.vdev_ops = &vdev_raidz_ops; vd.vdev_children = 3;
 vd.vdev_nparity = 1; vd.vdev_asize = 32768; vd.vdev_ms = metaslabs;
 vd.vdev_ms_count = 1;
 CHECK(za_run(&spa) == 0);
 features = 2; CHECK(za_run(&spa) == ENOTSUP);
 features = 4; CHECK(za_run(&spa) == ENOTSUP);
 features = 0; vd.vdev_islog = B_TRUE; CHECK(za_run(&spa) == ENOTSUP);
 vd.vdev_islog = B_FALSE; spa.spa_mode = FREAD | FWRITE;
 CHECK(za_run(&spa) == EROFS); spa.spa_mode = FREAD;
 features = 1;
 avl_create(&spa.spa_sm_logs_by_txg, log_compare, sizeof (spa_log_sm_t),
     offsetof(spa_log_sm_t, sls_node));
 spa_log_sm_t log = { 0 }; log.sls_txg = 20; log.sls_sm_obj = 99;
 avl_add(&spa.spa_sm_logs_by_txg, &log);
 mock_log = sm; mock_log.sm_start = 0; mock_log.sm_size = UINT64_MAX;
 input_words[0] = SM_PREFIX_ENCODE(SM2_PREFIX) | SM2_RUN_ENCODE(8);
 input_words[1] = SM2_OFFSET_ENCODE(8);
 uint64_t old_total = za_total;
 CHECK(za_run(&spa) == 0 && za_total == old_total + 4096);
 open_error = EIO; CHECK(za_run(&spa) == EIO); open_error = 0;
 read_error = EIO; CHECK(za_run(&spa) == EIO); read_error = 0;
 avl_remove(&spa.spa_sm_logs_by_txg, &log); log.sls_txg = 31;
 avl_add(&spa.spa_sm_logs_by_txg, &log);
 CHECK(za_run(&spa) == EINVAL);
 avl_remove(&spa.spa_sm_logs_by_txg, &log); avl_destroy(&spa.spa_sm_logs_by_txg);
 CHECK(zdb_inventory_end(0) == 0);
 range_tree_vacate(tree, NULL, NULL); range_tree_destroy(tree);
 zfs_btree_fini();
 (void) printf("PASS %u native allocation checks; no pools opened\n", passed);
 return (0);
}
