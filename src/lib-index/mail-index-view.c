/* Copyright (C) 2003-2004 Timo Sirainen */

#include "lib.h"
#include "buffer.h"
#include "file-lock.h"
#include "mail-index-view-private.h"
#include "mail-transaction-log.h"

struct mail_index_view *mail_index_view_open(struct mail_index *index)
{
	struct mail_index_view *view;

	view = i_new(struct mail_index_view, 1);
	view->index = index;
	view->log_view = mail_transaction_log_view_open(index->log);

	view->map = index->map;
	view->map->refcount++;

	view->log_file_seq = view->map->log_file_seq;
	view->log_file_offset = view->map->log_file_offset;
	return view;
}

void mail_index_view_close(struct mail_index_view *view)
{
	mail_index_view_unlock(view);
	mail_transaction_log_view_close(view->log_view);

	if (view->log_syncs != NULL)
		buffer_free(view->log_syncs);
	mail_index_unmap(view->index, view->map);
	i_free(view);
}

int mail_index_view_lock_head(struct mail_index_view *view, int update_index)
{
	if (!mail_index_is_locked(view->index, view->lock_id)) {
		if (mail_index_lock_shared(view->index, update_index,
					   &view->lock_id) < 0)
			return -1;

		if (mail_index_map(view->index, FALSE) <= 0) {
			view->inconsistent = TRUE;
			return -1;
		}

		if (view->index->indexid != view->map->hdr->indexid) {
			/* index was rebuilt */
			view->inconsistent = TRUE;
			return -1;
		}
	} else if (update_index) {
		// FIXME: check if we need to reopen it!
	}

	return 0;
}

int mail_index_view_lock(struct mail_index_view *view)
{
	if (view->inconsistent)
		return -1;

	if (view->map != view->index->map) {
		/* not head mapping, no need to lock */
		return 0;
	}

	return mail_index_view_lock_head(view, FALSE);
}

void mail_index_view_unlock(struct mail_index_view *view)
{
	if (view->lock_id != 0) {
		mail_index_unlock(view->index, view->lock_id);
		view->lock_id = 0;
	}
}

uint32_t mail_index_view_get_message_count(struct mail_index_view *view)
{
	return view->map->records_count;
}

int mail_index_view_is_inconsistent(struct mail_index_view *view)
{
	return view->inconsistent;
}

struct mail_index *mail_index_view_get_index(struct mail_index_view *view)
{
	return view->index;
}

void mail_index_view_transaction_ref(struct mail_index_view *view)
{
	view->transactions++;
}

void mail_index_view_transaction_unref(struct mail_index_view *view)
{
	i_assert(view->transactions > 0);

	view->transactions--;
}

int mail_index_get_header(struct mail_index_view *view,
			  const struct mail_index_header **hdr_r)
{
	if (mail_index_view_lock(view) < 0)
		return -1;

	*hdr_r = view->map->hdr;
	return 0;
}

int mail_index_lookup(struct mail_index_view *view, uint32_t seq,
		      const struct mail_index_record **rec_r)
{
	struct mail_index_map *map;
	const struct mail_index_record *rec;
	uint32_t uid;

	i_assert(seq > 0);
	i_assert(seq <= view->map->records_count);

	if (mail_index_view_lock(view) < 0)
		return -1;

	rec = &view->map->records[seq-1];
	if (view->map == view->index->map) {
		*rec_r = rec;
		return 0;
	}

	if (mail_index_view_lock_head(view, FALSE) < 0)
		return -1;

	/* look for it in the head mapping */
	uid = rec->uid;
	if (seq > view->index->hdr->messages_count)
		seq = view->index->hdr->messages_count;

	map = view->index->map;
	while (seq > 0) {
		// FIXME: we could be skipping more by uid diff
		if (map->records[--seq].uid <= uid)
			break;
	}

	*rec_r = map->records[seq].uid == uid ?
		&map->records[seq] : rec;
	return 0;
}

int mail_index_lookup_uid(struct mail_index_view *view, uint32_t seq,
			  uint32_t *uid_r)
{
	i_assert(seq > 0);
	i_assert(seq <= view->map->records_count);

	if (mail_index_view_lock(view) < 0)
		return -1;

	*uid_r = view->map->records[seq-1].uid;
	return 0;
}

static uint32_t mail_index_bsearch_uid(struct mail_index_view *view,
				       uint32_t uid, uint32_t *left_idx_p,
				       int nearest_side)
{
	const struct mail_index_record *rec;
	uint32_t idx, left_idx, right_idx;

	rec = view->map->records;

	idx = 0;
	left_idx = *left_idx_p;
	right_idx = view->map->records_count;

	while (left_idx < right_idx) {
		idx = (left_idx + right_idx) / 2;

		if (rec[idx].uid < uid)
			left_idx = idx+1;
		else if (rec[idx].uid > uid)
			right_idx = idx;
		else
			break;
	}

        *left_idx_p = left_idx;
	if (rec[idx].uid != uid) {
		if (nearest_side > 0) {
			/* we want uid or larger */
			return rec[idx].uid > uid ? idx+1 :
				idx == view->map->records_count-1 ? 0 : idx+2;
		} else {
			/* we want uid or smaller */
			return rec[idx].uid < uid ? idx + 1 : idx;
		}
	}

	return idx+1;
}

int mail_index_lookup_uid_range(struct mail_index_view *view,
				uint32_t first_uid, uint32_t last_uid,
				uint32_t *first_seq_r, uint32_t *last_seq_r)
{
	uint32_t left_idx;

	i_assert(first_uid > 0);
	i_assert(first_uid <= last_uid);

	if (mail_index_view_lock(view) < 0)
		return -1;

	left_idx = 0;
	*first_seq_r = mail_index_bsearch_uid(view, first_uid, &left_idx, 1);
	if (*first_seq_r == 0 ||
	    view->map->records[*first_seq_r-1].uid > last_uid) {
		*first_seq_r = 0;
		*last_seq_r = 0;
		return 0;
	}
	if (first_uid == last_uid) {
		*last_seq_r = *first_seq_r;
		return 0;
	}

	if (last_uid >= view->map->hdr->next_uid)
		last_uid = view->map->hdr->next_uid-1;

	/* optimization - binary lookup only from right side: */
	*last_seq_r = mail_index_bsearch_uid(view, last_uid, &left_idx, -1);
	i_assert(*last_seq_r >= *first_seq_r);
	return 0;
}

int mail_index_lookup_first(struct mail_index_view *view, enum mail_flags flags,
			    uint8_t flags_mask, uint32_t *seq_r)
{
#define LOW_UPDATE(x) \
	STMT_START { if ((x) > low_uid) low_uid = x; } STMT_END
	const struct mail_index_record *rec;
	uint32_t seq, low_uid = 1;

	*seq_r = 0;

	if (mail_index_view_lock(view) < 0)
		return -1;

	if ((flags_mask & MAIL_RECENT) != 0 && (flags & MAIL_RECENT) != 0)
		LOW_UPDATE(view->map->hdr->first_recent_uid_lowwater);
	if ((flags_mask & MAIL_SEEN) != 0 && (flags & MAIL_SEEN) == 0)
		LOW_UPDATE(view->map->hdr->first_unseen_uid_lowwater);
	if ((flags_mask & MAIL_DELETED) != 0 && (flags & MAIL_DELETED) != 0)
		LOW_UPDATE(view->map->hdr->first_deleted_uid_lowwater);

	if (low_uid == 1)
		seq = 1;
	else {
		if (mail_index_lookup_uid_range(view, low_uid, low_uid,
						&seq, &seq) < 0)
			return -1;

		if (seq == 0)
			return 0;
	}

	rec = &view->map->records[seq-1];
	for (; seq <= view->map->records_count; seq++, rec++) {
		if ((rec->flags & flags_mask) == (uint8_t)flags) {
			*seq_r = seq;
			break;
		}
	}

	return 0;
}
