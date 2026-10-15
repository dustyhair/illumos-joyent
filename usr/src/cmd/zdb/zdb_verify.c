/* CDDL: see usr/src/OPENSOLARIS.LICENSE. Included by zdb_inventory.c.
 * Original-BP synchronous raw checksum reads. No writes or BP rewriting.
 */
#include <sys/zio.h>
#include <sys/abd.h>
#define ZV_BYTE_LIMIT (1ULL << 30)
#define ZV_BLOCK_LIMIT (16ULL << 20)
static uint64_t zv_seen, zv_submitted, zv_completed, zv_passed, zv_failed;
static uint64_t zv_unsupported, zv_na, zv_requested, zv_verified, zv_outstanding;

typedef struct zv_completion {
 blkptr_t bp;
 zbookmark_phys_t bookmark;
 abd_t *abd;
 uint64_t p, size;
 unsigned calls;
 int error, invalid;
} zv_completion_t;

static boolean_t
zv_readonly(spa_t *spa)
{
 return (spa_mode(spa) == FREAD && !spa_writeable(spa));
}

static void
zv_done(zio_t *zio)
{
 zv_completion_t *c = zio->io_private;
 c->calls++;
 c->error = zio->io_error;
 c->size = zio->io_size;
 c->invalid = c->calls != 1 || zio->io_type != ZIO_TYPE_READ ||
     !zv_readonly(zio->io_spa) || zio->io_orig_size != c->p ||
     zio->io_size != c->p || zio->io_abd != c->abd ||
     zio->io_bp == NULL || memcmp(zio->io_bp, &c->bp, sizeof (c->bp)) ||
     memcmp(&zio->io_bookmark, &c->bookmark, sizeof (c->bookmark)) ||
     (zio->io_flags & (ZIO_FLAG_CANFAIL | ZIO_FLAG_SCRUB | ZIO_FLAG_RAW)) !=
     (ZIO_FLAG_CANFAIL | ZIO_FLAG_SCRUB | ZIO_FLAG_RAW) ||
     (zio->io_flags & (ZIO_FLAG_SPECULATIVE | ZIO_FLAG_DONT_PROPAGATE));
}

static int
zv_record(spa_t *spa, const blkptr_t *bp, const zbookmark_phys_t *zb,
    boolean_t geometry_rejected)
{
 const char *status = "failed";
 int error = 0;
 uint64_t p = 0, completed_p = 0;
 boolean_t measured = B_FALSE;
 zv_seen++;
 /* Never invoke a type-indexing macro before validating the raw type. */
 if (!zv_readonly(spa)) error = EROFS;
 else if (!DMU_OT_IS_VALID(BP_GET_TYPE(bp)) || BP_USES_CRYPT(bp) ||
     BP_GET_DEDUP(bp) || BP_GET_TYPE(bp) == DMU_OT_DEDUP ||
     BP_GET_TYPE(bp) == DMU_OT_DDT_ZAP || BP_GET_TYPE(bp) == DMU_OT_DDT_STATS ||
     geometry_rejected) error = ENOTSUP;
 else if (BP_IS_EMBEDDED(bp) || BP_IS_HOLE(bp)) {
  status = "not_applicable"; zv_na++;
 } else if (BP_COUNT_GANG(bp) ||
     (BP_GET_CHECKSUM(bp) != ZIO_CHECKSUM_FLETCHER_4 &&
     BP_GET_CHECKSUM(bp) != ZIO_CHECKSUM_SHA256)) error = ENOTSUP;
 else {
  p = BP_GET_PSIZE(bp);
  if (!p || p > ZV_BLOCK_LIMIT || p > ZV_BYTE_LIMIT - zv_requested ||
      zv_outstanding != 0) error = E2BIG;
  else {
   zv_completion_t completion = { 0 };
   completion.bp = *bp; completion.bookmark = *zb; completion.p = p;
   completion.abd = abd_alloc(p, B_FALSE);
   if (!completion.abd) error = ENOMEM;
   else {
    zv_submitted++; zv_outstanding++; zv_requested += p;
    zio_t *read = zio_read(NULL, spa, &completion.bp, completion.abd, p,
        zv_done, &completion, ZIO_PRIORITY_SYNC_READ,
        ZIO_FLAG_CANFAIL | ZIO_FLAG_SCRUB | ZIO_FLAG_RAW, &completion.bookmark);
    int result = zio_wait(read);
    zv_outstanding--;
    if (completion.calls == 1) zv_completed++;
    measured = completion.calls == 1 && !completion.invalid;
    error = result;
    if (!measured || completion.error != result || !zv_readonly(spa) ||
        memcmp(&completion.bp, bp, sizeof (*bp)) ||
        memcmp(&completion.bookmark, zb, sizeof (*zb)))
     error = EIO;
    if (!error) {
     status = "verified"; zv_passed++; zv_verified += p; completed_p = p;
    }
    abd_free(completion.abd);
   }
  }
 }
 if (error == ENOTSUP) { status = "unsupported"; zv_unsupported++; }
 else if (error) zv_failed++;
 (void) fprintf(zi_out, "{\"record\":\"verification\",\"sequence\":\"%llu\","
     "\"status\":\"%s\",\"measured\":%s,\"error\":%d,"
     "\"requested_p\":\"%llu\",\"completed_p\":\"%llu\","
     "\"objset\":\"%llu\",\"object\":\"%llu\",\"level\":\"%lld\","
     "\"blkid\":\"%llu\",\"bp_words\":",
     (u_longlong_t)zi_count, status, measured ? "true" : "false", error,
     (u_longlong_t)p, (u_longlong_t)completed_p,
     (u_longlong_t)zb->zb_objset, (u_longlong_t)zb->zb_object,
     (longlong_t)zb->zb_level, (u_longlong_t)zb->zb_blkid);
 zi_words(zi_out, bp);
 (void) fprintf(zi_out, ",%s}\n", zi_flags);
 if (fflush(zi_out) != 0 || ferror(zi_out)) return (EIO);
 return (error);
}

static int
zv_end(int error)
{
 if (zv_failed || zv_unsupported || zv_outstanding ||
     zv_submitted != zv_completed || zv_submitted != zv_passed ||
     zv_seen != zv_passed + zv_na || zv_requested != zv_verified)
  if (!error) error = EIO;
 (void) fprintf(zi_out, "{\"record\":\"verification_end\","
     "\"verification_complete\":%s,\"seen\":\"%llu\","
     "\"submitted\":\"%llu\",\"completed\":\"%llu\","
     "\"passed\":\"%llu\",\"failed\":\"%llu\","
     "\"unsupported\":\"%llu\",\"not_applicable\":\"%llu\","
     "\"outstanding\":\"%llu\",\"requested_p\":\"%llu\","
     "\"verified_p\":\"%llu\",%s}\n", error ? "false" : "true",
     (u_longlong_t)zv_seen, (u_longlong_t)zv_submitted,
     (u_longlong_t)zv_completed, (u_longlong_t)zv_passed,
     (u_longlong_t)zv_failed, (u_longlong_t)zv_unsupported,
     (u_longlong_t)zv_na, (u_longlong_t)zv_outstanding,
     (u_longlong_t)zv_requested, (u_longlong_t)zv_verified, zi_flags);
 return (error);
}
