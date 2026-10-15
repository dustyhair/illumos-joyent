/* Pool-free checksum-domain tests using real pinned ABD/checksum code. */
#include <stdio.h>
#include <stdlib.h>
#include <sys/zfs_context.h>
#include <sys/spa_impl.h>
#include <sys/zio.h>
#include <sys/zio_checksum.h>
#include <sys/abd.h>
#include <zfs_fletcher.h>
#define CHECK(x) do { if (!(x)) { (void) fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); return (1); } checks++; } while (0)
int main(void) {
 unsigned checks = 0; spa_t spa = { 0 }; abd_init(); fletcher_4_init();
 enum zio_checksum algorithms[] = { ZIO_CHECKSUM_FLETCHER_4, ZIO_CHECKSUM_SHA256 };
 uint64_t sizes[] = { 512, 1024, 131072 };
 for (unsigned a = 0; a < 2; a++) for (unsigned j = 0; j < 3; j++) {
  uint64_t p = sizes[j], q = P2ROUNDUP(p, 4096);
  unsigned char *bytes = malloc(q);
  for (uint64_t i = 0; i < q; i++) bytes[i] = (unsigned char)(i * 37 + 11);
  abd_t *abd = abd_get_from_buf(bytes, q);
  blkptr_t bp = { 0 }; BP_SET_TYPE(&bp, DMU_OT_PLAIN_FILE_CONTENTS);
  BP_SET_LSIZE(&bp, 131072); BP_SET_PSIZE(&bp, p);
  BP_SET_CHECKSUM(&bp, algorithms[a]); BP_SET_BYTEORDER(&bp, ZFS_HOST_BYTEORDER);
  /* Stored-byte checksum only; no decompression claim for these fixture bytes. */
  BP_SET_COMPRESS(&bp, ZIO_COMPRESS_LZ4);
  zio_t z = { 0 }; z.io_spa = &spa; z.io_bp = &bp; z.io_abd = abd; z.io_size = p;
  zio_checksum_compute(&z, algorithms[a], abd, p);
  zio_bad_cksum_t info = { 0 };
  CHECK(zio_checksum_error_impl(&spa, &bp, algorithms[a], abd, p, 4096, &info) == 0);
  bytes[p-1] ^= 1;
  CHECK(zio_checksum_error_impl(&spa, &bp, algorithms[a], abd, p, 4096, &info) == ECKSUM);
  bytes[p-1] ^= 1;
  if (q > p) {
   bytes[p] ^= 1;
   CHECK(zio_checksum_error_impl(&spa, &bp, algorithms[a], abd, p, 4096, &info) == 0);
  }
  abd_put(abd); free(bytes);
 }
 fletcher_4_fini(); abd_fini(); (void) printf("PASS %u real checksum-domain checks; no pools opened\n", checks);
 return (0);
}
