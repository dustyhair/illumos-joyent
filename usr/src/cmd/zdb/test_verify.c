/* Pool-free original-BP verifier tests; all ZIO/ABD activity is stubbed. */
#include <stdio.h>
static int bad_flush, bad_close;
static int verify_flush(FILE *), verify_close(FILE *);
#define fflush verify_flush
#define fclose verify_close
#include "zdb_inventory.c"
#undef fflush
#undef fclose
static int verify_flush(FILE *f) { return (bad_flush ? EOF : fflush(f)); }
static int verify_close(FILE *f) { int e = fclose(f); return (bad_close ? EOF : e); }
static unsigned reads, writes, frees;
static uint64_t allocated_size;
static const char *mode;
static zio_t fake;
static blkptr_t fake_bp, expected_bp;
static zbookmark_phys_t expected_zb;
static int policy_error;
abd_t *abd_alloc(size_t n, boolean_t meta) {
 (void) meta; allocated_size = n; return (calloc(1, sizeof (abd_t)));
}
void abd_free(abd_t *a) { frees++; free(a); }
zio_t *zio_read(zio_t *parent, spa_t *spa, const blkptr_t *bp, abd_t *abd,
    uint64_t n, zio_done_func_t *done, void *private, zio_priority_t priority,
    enum zio_flag flags, const zbookmark_phys_t *zb) {
 reads++;
 if (parent || spa_mode(spa) != FREAD || spa_writeable(spa) ||
     flags != (ZIO_FLAG_CANFAIL | ZIO_FLAG_SCRUB | ZIO_FLAG_RAW) ||
     priority != ZIO_PRIORITY_SYNC_READ || n != BP_GET_PSIZE(bp) ||
     allocated_size != n || memcmp(bp, &expected_bp, sizeof (*bp)) ||
     memcmp(zb, &expected_zb, sizeof (*zb))) policy_error = 1;
 memset(&fake, 0, sizeof (fake)); fake_bp = *bp;
 fake.io_spa = spa; fake.io_bp = &fake_bp; fake.io_abd = abd;
 fake.io_orig_abd = abd; fake.io_size = fake.io_orig_size = n;
 fake.io_type = ZIO_TYPE_READ; fake.io_flags = flags; fake.io_bookmark = *zb;
 fake.io_done = done; fake.io_private = private;
 return (&fake);
}
/* An attempted direct write from the diagnostic is an unconditional test failure. */
zio_t *zio_write(zio_t *pio, spa_t *spa, uint64_t txg, blkptr_t *bp,
    abd_t *data, uint64_t lsize, uint64_t psize, const zio_prop_t *zp,
    zio_done_func_t *ready, zio_done_func_t *children_ready,
    zio_done_func_t *physdone, zio_done_func_t *done, void *private,
    zio_priority_t priority, enum zio_flag flags, const zbookmark_phys_t *zb) {
 (void) pio; (void) spa; (void) txg; (void) bp; (void) data; (void) lsize;
 (void) psize; (void) zp; (void) ready; (void) children_ready; (void) physdone;
 (void) done; (void) private; (void) priority; (void) flags; (void) zb;
 writes++; abort(); return (NULL);
}
int zio_wait(zio_t *z) {
 if (strcmp(mode, "checksum-error") == 0) z->io_error = ECKSUM;
 if (strcmp(mode, "read-error") == 0) z->io_error = EIO;
 if (strcmp(mode, "short-completion") == 0) z->io_size--;
 if (strcmp(mode, "changed-bp") == 0) z->io_bp->blk_birth++;
 if (strcmp(mode, "changed-bookmark") == 0) z->io_bookmark.zb_object++;
 if (strcmp(mode, "writable-during-io") == 0) z->io_spa->spa_mode |= FWRITE;
 if (strcmp(mode, "wrong-type") == 0) z->io_type = ZIO_TYPE_WRITE;
 if (strcmp(mode, "missing-completion") != 0) z->io_done(z);
 if (strcmp(mode, "double-completion") == 0) z->io_done(z);
 if (strcmp(mode, "wait-disagrees") == 0) return (EIO);
 return (z->io_error);
}
int main(int argc, char **argv) {
 if (argc != 3) return (99);
 mode = argv[1];
 spa_t spa = { 0 }; spa.spa_mode = FREAD; spa.spa_trust_config = B_TRUE;
 blkptr_t bp = { 0 }; zbookmark_phys_t zb = { 5, 7, 0, 11 };
 BP_SET_TYPE(&bp, DMU_OT_PLAIN_FILE_CONTENTS); BP_SET_CHECKSUM(&bp, ZIO_CHECKSUM_FLETCHER_4);
 BP_SET_COMPRESS(&bp, ZIO_COMPRESS_OFF); BP_SET_LSIZE(&bp, 131072); BP_SET_PSIZE(&bp, 1024);
 BP_SET_BIRTH(&bp, 20, 19); DVA_SET_ASIZE(&bp.blk_dva[0], 8192);
 DVA_SET_OFFSET(&bp.blk_dva[0], 4096);
 if (strcmp(mode, "sha256") == 0) BP_SET_CHECKSUM(&bp, ZIO_CHECKSUM_SHA256);
 if (strcmp(mode, "compressed") == 0) BP_SET_COMPRESS(&bp, ZIO_COMPRESS_LZ4);
 if (strcmp(mode, "checksum-off") == 0) BP_SET_CHECKSUM(&bp, ZIO_CHECKSUM_OFF);
 if (strcmp(mode, "noparity") == 0) BP_SET_CHECKSUM(&bp, ZIO_CHECKSUM_NOPARITY);
 if (strcmp(mode, "invalid-type") == 0) BP_SET_TYPE(&bp, 127);
 if (strcmp(mode, "protected") == 0) BP_SET_CRYPT(&bp, 1);
 if (strcmp(mode, "gang") == 0) DVA_SET_GANG(&bp.blk_dva[0], 1);
 if (strcmp(mode, "dedup") == 0) BP_SET_DEDUP(&bp, 1);
 if (strcmp(mode, "ddt") == 0) BP_SET_TYPE(&bp, DMU_OT_DDT_ZAP);
 if (strcmp(mode, "hole") == 0) memset(bp.blk_dva, 0, sizeof (bp.blk_dva));
 if (strcmp(mode, "embedded") == 0) BP_SET_EMBEDDED(&bp, 1);
 if (strcmp(mode, "writable") == 0) spa.spa_mode = FREAD | FWRITE;
 if (strcmp(mode, "no-read-mode") == 0) spa.spa_mode = 0;
 if (strcmp(mode, "byte-limit") == 0) zv_requested = ZV_BYTE_LIMIT;
 zdb_inventory_verify_mode();
 int e = zdb_inventory_begin(argv[2]); if (e) return (e);
 expected_bp = bp; expected_zb = zb; zi_count = 1;
 e = zv_record(&spa, &bp, &zb, strcmp(mode, "bad-geometry") == 0);
 if (strcmp(mode, "duplicate") == 0 && !e) { zi_count++; e = zv_record(&spa, &bp, &zb, B_FALSE); }
 if (strcmp(mode, "close-error") == 0) bad_close = 1;
 if (strcmp(mode, "flush-error") == 0) bad_flush = 1;
 e = zdb_inventory_end(e);
 if (policy_error || writes || reads != frees || memcmp(&bp, &expected_bp, sizeof (bp)) ||
     memcmp(&zb, &expected_zb, sizeof (zb))) return (98);
 (void) printf("reads=%u writes=%u frees=%u P=%llu\n", reads, writes, frees,
     (u_longlong_t)allocated_size);
 return (e ? 1 : 0);
}
