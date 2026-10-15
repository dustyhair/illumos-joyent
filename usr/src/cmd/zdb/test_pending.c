/* Pool-free tests of the production pending-owner walker, with fake DMU. */
#include <stdio.h>
static int bad_flush, bad_close;
static int pending_flush(FILE *), pending_close(FILE *);
#define fflush pending_flush
#define fclose pending_close
#include "zdb_inventory.c"
#undef fflush
#undef fclose
static int pending_flush(FILE *f) { return (bad_flush ? EOF : fflush(f)); }
static int pending_close(FILE *f) { int e = fclose(f); return (bad_close ? EOF : e); }
typedef struct fake_obj {
 dmu_object_info_t info;
 bpobj_phys_t hdr;
 blkptr_t bps[2];
 uint64_t children[2];
 dmu_buf_t bonus;
 int exists;
} fake_obj_t;
static fake_obj_t objects[256];
static uint64_t version = SPA_VERSION_DEADLISTS;
static int read_error, bonus_error, info_error, sparse, tiny_bonus;
static unsigned reads, holds, releases, writes;
uint64_t spa_version(spa_t *s) { (void) s; return (version); }
int dmu_object_info(objset_t *os, uint64_t obj, dmu_object_info_t *info) {
 (void) os;
 if (info_error) return (EIO);
 if (obj >= 256 || !objects[obj].exists) return (ENOENT);
 *info = objects[obj].info; return (0);
}
int dmu_bonus_hold(objset_t *os, uint64_t obj, void *tag, dmu_buf_t **db) {
 (void) os; (void) tag;
 if (bonus_error) return (EIO);
 if (obj >= 256 || !objects[obj].exists) return (ENOENT);
 fake_obj_t *o = &objects[obj];
 o->bonus.db_data = &o->hdr; o->bonus.db_size = tiny_bonus ? 8 : sizeof (o->hdr);
 *db = &o->bonus; holds++; return (0);
}
void dmu_buf_rele(dmu_buf_t *db, void *tag) { (void) db; (void) tag; releases++; }
int dmu_offset_next(objset_t *os, uint64_t obj, boolean_t hole, uint64_t *off) {
 (void) os; (void) obj; (void) hole;
 if (sparse) *off += 512;
 return (0);
}
int dmu_read(objset_t *os, uint64_t obj, uint64_t off, uint64_t n, void *buf, uint32_t flags) {
 (void) os; (void) flags; reads++;
 if (read_error) return (EIO);
 if (obj >= 256 || !objects[obj].exists) return (ENOENT);
 fake_obj_t *o = &objects[obj];
 void *data = o->info.doi_type == DMU_OT_BPOBJ ? (void *)o->bps : (void *)o->children;
 uint64_t size = o->info.doi_type == DMU_OT_BPOBJ ? sizeof (o->bps) : sizeof (o->children);
 if (off > size || n > size - off) return (EIO);
 memcpy(buf, (char *)data + off, n); return (0);
}
void dmu_buf_will_dirty(dmu_buf_t *db, dmu_tx_t *tx) { (void) db; (void) tx; writes++; abort(); }
int dmu_free_range(objset_t *os, uint64_t obj, uint64_t off, uint64_t n, dmu_tx_t *tx) {
 (void) os; (void) obj; (void) off; (void) n; (void) tx; writes++; abort(); return (EIO);
}
zio_t *zio_read(zio_t *parent, spa_t *spa, const blkptr_t *bp, abd_t *abd,
    uint64_t n, zio_done_func_t *done, void *private, zio_priority_t pri,
    enum zio_flag flags, const zbookmark_phys_t *zb) {
 (void) parent; (void) spa; (void) bp; (void) abd; (void) n; (void) done;
 (void) private; (void) pri; (void) flags; (void) zb;
 /* Pending entries must never enter the original-payload verifier. */
 abort(); return (NULL);
}
static void init_obj(unsigned id, unsigned entries) {
 fake_obj_t *o = &objects[id]; memset(o, 0, sizeof (*o));o->exists = 1;
 o->info.doi_type = DMU_OT_BPOBJ; o->info.doi_bonus_type = DMU_OT_BPOBJ_HDR;
 o->info.doi_bonus_size = sizeof (bpobj_phys_t);o->info.doi_data_block_size = 512;
 o->info.doi_max_offset = entries ? 512 : 0;o->hdr.bpo_num_blkptrs = entries;
 o->hdr.bpo_bytes = entries * 8192;
 for (unsigned i = 0; i < entries && i < 2; i++) {
  blkptr_t *b = &o->bps[i]; BP_SET_TYPE(b, DMU_OT_PLAIN_FILE_CONTENTS);
  BP_SET_LSIZE(b, 4096); BP_SET_PSIZE(b, 4096); BP_SET_CHECKSUM(b, ZIO_CHECKSUM_FLETCHER_4);
  BP_SET_COMPRESS(b, ZIO_COMPRESS_OFF);BP_SET_BIRTH(b, 20, 19);
  DVA_SET_ASIZE(&b->blk_dva[0],8192);DVA_SET_OFFSET(&b->blk_dva[0],4096+i*8192);
 }
}
static void link_obj(unsigned parent, unsigned list, unsigned child) {
 fake_obj_t *o = &objects[list];memset(o,0,sizeof (*o));o->exists=1;
 o->info.doi_type=DMU_OT_BPOBJ_SUBOBJ;o->info.doi_data_block_size=512;o->info.doi_max_offset=512;
 o->children[0]=child;objects[parent].hdr.bpo_subobjs=list;objects[parent].hdr.bpo_num_subobjs=1;
}
int main(int argc,char **argv) {
 if (argc != 3) return (99);
 const char *mode=argv[1];init_obj(1,1);init_obj(2,0);
 spa_t spa = { 0 };dsl_pool_t dp={ 0 };vdev_t root={ 0 }, vd={ 0 };
 vdev_ops_t ops={ 0 };(void)strlcpy(ops.vdev_op_type,"raidz",sizeof(ops.vdev_op_type));vdev_t *children[]={ &vd };
 vd.vdev_ops=&ops;vd.vdev_id=0;vd.vdev_guid=7;vd.vdev_ashift=12;
 vd.vdev_children=3;vd.vdev_nparity=1;vd.vdev_asize=1048576;
 root.vdev_children=1;root.vdev_child=children;spa.spa_root_vdev=&root;
 spa.spa_mode=FREAD;spa.spa_trust_config=B_TRUE;spa.spa_ubsync.ub_txg=30;
 spa.spa_meta_objset=(objset_t *)(uintptr_t)1;spa.spa_dsl_pool=&dp;
 spa.spa_deferred_bpobj.bpo_object=1;dp.dp_free_bpobj.bpo_object=2;
 if (!strcmp(mode,"empty")) init_obj(1,0);
 if (!strcmp(mode,"both")) init_obj(2,1);
 if (!strcmp(mode,"legacy")) { version=25;objects[1].info.doi_bonus_size=BPOBJ_SIZE_V1; }
 if (!strcmp(mode,"legacy-v0")) { version=15;objects[1].info.doi_bonus_size=BPOBJ_SIZE_V0; }
 if (!strcmp(mode,"nested")) { init_obj(4,1);link_obj(1,3,4); }
 if (!strcmp(mode,"duplicate-extents")) { init_obj(1,2);objects[1].bps[1]=objects[1].bps[0]; }
 if (!strcmp(mode,"embedded")) { blkptr_t *b=&objects[1].bps[0];memset(b,0,sizeof (*b));BP_SET_TYPE(b,DMU_OT_PLAIN_FILE_CONTENTS);BP_SET_EMBEDDED(b,1);b->blk_birth=20; }
 if (!strcmp(mode,"cycle")) link_obj(1,3,1);
 if (!strcmp(mode,"cross-root-alias")) link_obj(1,3,2);
 if (!strcmp(mode,"list-alias")) link_obj(1,2,4);
 if (!strcmp(mode,"duplicate-child")) { init_obj(4,0);link_obj(1,3,4);objects[1].hdr.bpo_num_subobjs=2;objects[3].children[1]=4; }
 if (!strcmp(mode,"zero-child")) link_obj(1,3,0);
 if (!strcmp(mode,"missing-child")) link_obj(1,3,4);
 if (!strcmp(mode,"bad-list-type")) {link_obj(1,3,4);objects[3].info.doi_type=DMU_OT_BPOBJ;}
 if (!strcmp(mode,"depth-limit")) {for(unsigned i=0;i<66;i++){init_obj(4+i,0);link_obj(i?3+i:1,130+i,4+i);}}
 if (!strcmp(mode,"missing-root")) spa.spa_deferred_bpobj.bpo_object=0;
 if (!strcmp(mode,"missing-dsl-root")) dp.dp_free_bpobj.bpo_object=0;
 if (!strcmp(mode,"bad-type")) objects[1].info.doi_type=DMU_OT_PLAIN_FILE_CONTENTS;
 if (!strcmp(mode,"bad-bonus-type")) objects[1].info.doi_bonus_type=DMU_OT_NONE;
 if (!strcmp(mode,"bad-bonus-size")) objects[1].info.doi_bonus_size=40;
 if (!strcmp(mode,"short-bonus")) tiny_bonus=1;
 if (!strcmp(mode,"bad-block-size")) objects[1].info.doi_data_block_size=129;
 if (!strcmp(mode,"huge-block")) objects[1].info.doi_data_block_size=ZP_BLOCK_LIMIT+512;
 if (!strcmp(mode,"truncated")) objects[1].info.doi_max_offset=64;
 if (!strcmp(mode,"entries-limit")) objects[1].hdr.bpo_num_blkptrs=ZP_ENTRY_LIMIT+1;
 if (!strcmp(mode,"subobjects-limit")) objects[1].hdr.bpo_num_subobjs=ZP_OBJECT_LIMIT+1;
 if (!strcmp(mode,"empty-accounting")) objects[1].hdr.bpo_num_blkptrs=0;
 if (!strcmp(mode,"read-error")) read_error=1;
 if (!strcmp(mode,"bonus-error")) bonus_error=1;
 if (!strcmp(mode,"info-error")) info_error=1;
 if (!strcmp(mode,"sparse")) sparse=1;
 if (!strcmp(mode,"writable")) spa.spa_mode|=FWRITE;
 if (!strcmp(mode,"invalid-type")) BP_SET_TYPE(&objects[1].bps[0],127);
 if (!strcmp(mode,"protected")) BP_SET_CRYPT(&objects[1].bps[0],1);
 if (!strcmp(mode,"dedup")) BP_SET_DEDUP(&objects[1].bps[0],1);
 if (!strcmp(mode,"ddt")) BP_SET_TYPE(&objects[1].bps[0],DMU_OT_DDT_ZAP);
 if (!strcmp(mode,"gang")) DVA_SET_GANG(&objects[1].bps[0].blk_dva[0],1);
 if (!strcmp(mode,"hole")) memset(objects[1].bps[0].blk_dva,0,sizeof(objects[1].bps[0].blk_dva));
 if (!strcmp(mode,"future-birth")) objects[1].bps[0].blk_birth=31;
 if (!strcmp(mode,"bad-geometry")) DVA_SET_ASIZE(&objects[1].bps[0].blk_dva[0],4096);
 if (!strcmp(mode,"outside-vdev")) DVA_SET_OFFSET(&objects[1].bps[0].blk_dva[0],1048576);
 /* Direct cap boundary checks exercise branches before expensive I/O. */
 zp_ctx_t caps={0};caps.bytes=ZP_BYTES_LIMIT;
 if (zp_budget(&caps,1)!=E2BIG) return (98);
 caps.id_count=ZP_OBJECT_LIMIT;if(zp_register(&caps,42)!=E2BIG)return (97);
 zdb_inventory_pair_mode();int e=zdb_inventory_begin(argv[2]);if(e)return (96);
 if (!strcmp(mode,"flush-error")) bad_flush=1;
 e=zp_run(&spa);
 if (!strcmp(mode,"close-error")) bad_close=1;
 e=zdb_inventory_end(e);
 if(holds!=releases || writes)return (95);
 (void)printf("reads=%u holds=%u releases=%u writes=%u payload_reads=0\n",reads,holds,releases,writes);
 return (e?1:0);
}
