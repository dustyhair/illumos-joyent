static void
space_map_write_seg(space_map_t *sm, uint64_t rstart, uint64_t rend,
    maptype_t maptype, uint64_t vdev_id, uint8_t words, dmu_buf_t **dbp,
    void *tag, dmu_tx_t *tx)
{
	ASSERT3U(words, !=, 0);
	ASSERT3U(words, <=, 2);

	/* ensure the vdev_id can be represented by the space map */
	ASSERT3U(vdev_id, <=, SM_NO_VDEVID);

	/*
	 * if this is a single word entry, ensure that no vdev was
	 * specified.
	 */
	IMPLY(words == 1, vdev_id == SM_NO_VDEVID);

	dmu_buf_t *db = *dbp;
	ASSERT3U(db->db_size, ==, sm->sm_blksz);

	uint64_t *block_base = db->db_data;
	uint64_t *block_end = block_base + (sm->sm_blksz / sizeof (uint64_t));
	uint64_t *block_cursor = block_base +
	    (sm->sm_phys->smp_length - db->db_offset) / sizeof (uint64_t);

	ASSERT3P(block_cursor, <=, block_end);

	uint64_t size = (rend - rstart) >> sm->sm_shift;
	uint64_t start = (rstart - sm->sm_start) >> sm->sm_shift;
	uint64_t run_max = (words == 2) ? SM2_RUN_MAX : SM_RUN_MAX;

	ASSERT3U(rstart, >=, sm->sm_start);
	ASSERT3U(rstart, <, sm->sm_start + sm->sm_size);
	ASSERT3U(rend - rstart, <=, sm->sm_size);
	ASSERT3U(rend, <=, sm->sm_start + sm->sm_size);

	while (size != 0) {
		ASSERT3P(block_cursor, <=, block_end);

		/*
		 * If we are at the end of this block, flush it and start
		 * writing again from the beginning.
		 */
		if (block_cursor == block_end) {
			dmu_buf_rele(db, tag);

			uint64_t next_word_offset = sm->sm_phys->smp_length;
			VERIFY0(dmu_buf_hold(sm->sm_os,
			    space_map_object(sm), next_word_offset,
			    tag, &db, DMU_READ_PREFETCH));
			dmu_buf_will_dirty(db, tx);

			/* update caller's dbuf */
			*dbp = db;

			ASSERT3U(db->db_size, ==, sm->sm_blksz);

			block_base = db->db_data;
			block_cursor = block_base;
			block_end = block_base +
			    (db->db_size / sizeof (uint64_t));
		}

		/*
		 * If we are writing a two-word entry and we only have one
		 * word left on this block, just pad it with an empty debug
		 * entry and write the two-word entry in the next block.
		 */
		uint64_t *next_entry = block_cursor + 1;
		if (next_entry == block_end && words > 1) {
			ASSERT3U(words, ==, 2);
			*block_cursor = SM_PREFIX_ENCODE(SM_DEBUG_PREFIX) |
			    SM_DEBUG_ACTION_ENCODE(0) |
			    SM_DEBUG_SYNCPASS_ENCODE(0) |
			    SM_DEBUG_TXG_ENCODE(0);
			block_cursor++;
			sm->sm_phys->smp_length += sizeof (uint64_t);
			ASSERT3P(block_cursor, ==, block_end);
			continue;
		}

		uint64_t run_len = MIN(size, run_max);
		switch (words) {
		case 1:
			*block_cursor = SM_OFFSET_ENCODE(start) |
			    SM_TYPE_ENCODE(maptype) |
			    SM_RUN_ENCODE(run_len);
			block_cursor++;
			break;
		case 2:
			/* write the first word of the entry */
			*block_cursor = SM_PREFIX_ENCODE(SM2_PREFIX) |
			    SM2_RUN_ENCODE(run_len) |
			    SM2_VDEV_ENCODE(vdev_id);
			block_cursor++;

			/* move on to the second word of the entry */
			ASSERT3P(block_cursor, <, block_end);
			*block_cursor = SM2_TYPE_ENCODE(maptype) |
			    SM2_OFFSET_ENCODE(start);
			block_cursor++;
			break;
		default:
			panic("%d-word space map entries are not supported",
			    words);
			break;
		}
		sm->sm_phys->smp_length += words * sizeof (uint64_t);

		start += run_len;
		size -= run_len;
	}
	ASSERT0(size);

}
