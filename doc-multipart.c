/*
 * Copyright Neil Brown ©2016 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * doc-multipart: Present a sequence of documents as though it were
 *   just one.
 *   This is used for stitching together the parts of a MIME email message.
 *
 * The document is created empty, and then given subordinate documents
 * using a "multipart-add" command which causes the "focus" to be added
 * to a list.
 * If more sophisticated edits are needed, they can come later.
 */

#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>

#define PRIVATE_DOC_REF

struct doc_ref {
	struct mark *m;
	int docnum;
};

#include "core.h"

struct mp_info {
	struct doc	doc;
	int		nparts;
	int		parts_size;
	struct pane	* safe *parts safe;
};

static struct map *mp_map safe;

static void reset_mark(struct mark *m)
{
	/* m->ref.m might have moved.  If so, move m in the list of
	 * marks so marks in this document are still properly ordered
	 */
	struct mark *m2;

	if (!m || hlist_unhashed(&m->all))
		return;
	while ((m2 = doc_next_mark_all(m)) != NULL &&
	       (m2->ref.docnum < m->ref.docnum ||
		(m2->ref.docnum == m->ref.docnum &&
		 m2->ref.m && m->ref.m &&
		 m2->ref.m->seq < m->ref.m->seq))) {
		/* m should be after m2 */
		mark_forward_over(m, m2);
	}

	while ((m2 = doc_prev_mark_all(m)) != NULL &&
	       (m2->ref.docnum > m->ref.docnum||
		(m2->ref.docnum == m->ref.docnum &&
		 m2->ref.m && m->ref.m &&
		 m2->ref.m->seq > m->ref.m->seq))) {
		/* m should be before m2 */
		mark_backward_over(m, m2);
	}
}

static void mp_mark_refcnt(struct mark *m safe, int inc)
{
	if (inc > 0) {
		/* Duplicate being created of this mark */
		if (m->ref.m) {
			m->ref.m = mark_dup(m->ref.m, 1);
			reset_mark(m);
		}
	}
	if (inc < 0) {
		/* mark is being discarded, or ref over-written */
		if (m->ref.m)
			mark_free(m->ref.m);
		m->ref.m = NULL;
	}
}

static void mp_check_consistent(struct mp_info *mpi safe)
{
//	struct mark *m;
	struct doc *d = &mpi->doc;
//	int s = -1;

	doc_check_consistent(d);
#if 0
	for (m = doc_first_mark_all(d); m; m = doc_next_mark_all(m)) {
		if (!m->ref.m || m->ref.m->seq <= s) {
			for (m = doc_first_mark_all(d); m; m = doc_next_mark_all(m))
				if (m && m->ref.m)
					printf("%p %d %d\n", m, m->seq, m->ref.m->seq);

			abort();
		}
		s = m->ref.m->seq;
	}
	doc_check_consistent(d);
#endif
}

static void change_part(struct mp_info *mpi safe, struct mark *m safe, int part, int end)
{
	struct mark *m1;
	struct pane *p;

	if (part < 0 || part >= mpi->nparts)
		return;
	p = mpi->parts[part];
	if (m->ref.m)
		mark_free(m->ref.m);
	m1 = vmark_new(p, MARK_UNGROUPED);
	m->ref.m = m1;
	m->ref.docnum = part;
	m->refcnt = mp_mark_refcnt;
	call3("doc:set-ref", p, !end, m1);
}

DEF_CMD(mp_close)
{
	struct mp_info *mpi = ci->home->data;
	int i;
	for (i = 0; i < mpi->nparts; i++)
		call3("doc:closed", mpi->parts[i], 0, NULL);
	doc_free(&mpi->doc);
	free(mpi->parts);
	free(mpi);
	return 1;
}

DEF_CMD(mp_set_ref)
{
	struct mp_info *mpi = ci->home->data;
	struct mark *m1 = NULL;
	int ret;

	/* First we need to substitute the marks, then call the
	 * document.  Then make sure the marks are still in order.
	 */
	if (ci->mark) {
		if (!ci->mark->ref.m) {
			change_part(mpi, ci->mark, 0, 0);
			mark_to_end(&mpi->doc, ci->mark, 0);
			reset_mark(ci->mark);
		}
		m1 = ci->mark->ref.m;
	}
	if (!ci->mark)
		return -1;

	if (ci->numeric == 1) {
		/* start */
		if (ci->mark->ref.docnum != 0)
			change_part(mpi, ci->mark, 0, 0);
	} else {
		if (ci->mark->ref.docnum != mpi->nparts - 1)
			change_part(mpi, ci->mark, mpi->nparts - 1, 1);
	}

	ret = call_home7(mpi->parts[ci->mark->ref.docnum],
			 ci->key, ci->focus, ci->numeric, m1, ci->str,
			 ci->extra,ci->str2, NULL, ci->comm2);
	reset_mark(ci->mark);
	mp_check_consistent(mpi);
	return ret;
}

DEF_CMD(mp_same)
{
	struct mp_info *mpi = ci->home->data;
	struct mark *m1 = NULL, *m2 = NULL;
	struct pane *p1, *p2;
	int ret;

	/* Document access commands are handled by the 'cropper'.
	 * First we need to substitute the marks, then call the cropper
	 * which calls the document.  Then make sure the marks are still in order.
	 */
	mp_check_consistent(mpi);
	if (ci->mark) {
		if (!ci->mark->ref.m) {
			change_part(mpi, ci->mark, 0, 0);
			mark_to_end(&mpi->doc, ci->mark, 0);
			reset_mark(ci->mark);
		}
		m1 = ci->mark->ref.m;
	}
	if (ci->mark2) {
		if (!ci->mark2->ref.m) {
			change_part(mpi, ci->mark2, 0, 0);
			mark_to_end(&mpi->doc, ci->mark2, 0);
			reset_mark(ci->mark2);
		}
		m2 = ci->mark2->ref.m;
	}
	mp_check_consistent(mpi);
	if (ci->mark && ci->mark2 &&
	    ci->mark->ref.docnum != ci->mark2->ref.docnum) {
		p1 = mpi->parts[ci->mark->ref.docnum];
		p2 = mpi->parts[ci->mark2->ref.docnum];
		if (ci->mark->ref.docnum + 1 == ci->mark2->ref.docnum) {
			if (call5("doc:step", p1, 1, m1, NULL, 0) == CHAR_RET(WEOF) &&
			    call5("doc:step", p2, 0, m2, NULL, 0) == CHAR_RET(WEOF))
				return 1;
		} else if (ci->mark->ref.docnum - 1 == ci->mark2->ref.docnum) {
			if (call5("doc:step", p1, 0, m1, NULL, 0) == CHAR_RET(WEOF) &&
			    call5("doc:step", p2, 1, m2, NULL, 0) == CHAR_RET(WEOF))
				return 1;
		}
		return 2;
	}
	if (!ci->mark || !ci->mark2)
		return -1;
	ret = call_home7(mpi->parts[ci->mark->ref.docnum],
			 ci->key, ci->focus, ci->numeric, m1, ci->str,
			 ci->extra,ci->str2, m2, ci->comm2);
	reset_mark(ci->mark);
	reset_mark(ci->mark2);
	reset_mark(ci->mark);

	mp_check_consistent(mpi);
	return ret;
}

DEF_CMD(mp_step)
{
	struct mp_info *mpi = ci->home->data;
	struct mark *m1 = NULL;
	int ret;

	/* Document access commands are handled by the 'cropper'.
	 * First we need to substitute the marks, then call the cropper
	 * which calls the document.  Then make sure the marks are still in order.
	 */
	mp_check_consistent(mpi);
	if (!ci->mark)
		return -1;

	if (!ci->mark->ref.m) {
		change_part(mpi, ci->mark, 0, 0);
		mark_to_end(&mpi->doc, ci->mark, 0);
		reset_mark(ci->mark);
	}
	m1 = ci->mark->ref.m;

	mp_check_consistent(mpi);

	ret = call_home7(mpi->parts[ci->mark->ref.docnum],
			 ci->key, ci->focus, ci->numeric, m1, ci->str,
			 ci->extra,ci->str2, NULL, ci->comm2);
	while (ret == CHAR_RET(WEOF) || ret == -1) {
		if (ci->numeric) {
			if (ci->mark->ref.docnum >= mpi->nparts - 1)
				break;
			change_part(mpi, ci->mark, ci->mark->ref.docnum + 1, 0);
		} else {
			if (ci->mark->ref.docnum == 0)
				break;
			change_part(mpi, ci->mark, ci->mark->ref.docnum - 1, 1);
		}
		m1 = ci->mark->ref.m;
		ret = call_home7(mpi->parts[ci->mark->ref.docnum],
				 ci->key, ci->focus, ci->numeric, m1, ci->str,
				 ci->extra,ci->str2, NULL, ci->comm2);
	}
	reset_mark(ci->mark);

	mp_check_consistent(mpi);
	return ret;
}

DEF_CMD(mp_attr)
{
	struct mp_info *mpi = ci->home->data;
	struct mark *m1 = NULL;
	int ret;

	/*
	 * First we need to substitute the marks, then call the
	 * document.  Then make sure the marks are still in order.
	 */
	mp_check_consistent(mpi);
	if (ci->mark) {
		if (!ci->mark->ref.m) {
			change_part(mpi, ci->mark, 0, 0);
			mark_to_end(&mpi->doc, ci->mark, 0);
			reset_mark(ci->mark);
		}
		m1 = ci->mark->ref.m;
	}
	mp_check_consistent(mpi);
	if (!ci->mark)
		return -1;
	ret = call_home7(mpi->parts[ci->mark->ref.docnum],
			 ci->key, ci->focus, ci->numeric, m1, ci->str,
			 ci->extra,ci->str2, NULL, ci->comm2);
	reset_mark(ci->mark);
	mp_check_consistent(mpi);
	return ret;
}

DEF_CMD(mp_notify_close)
{
	if (strcmp(ci->key, "Notify:Close:request") == 0) {
		/* The autoclose document wants to know if it should close.
		 * tell it "no" */
		return 1;
	}

	if (strcmp(ci->key, "Notify:Close") == 0) {
		/* sub-document has been closed.
		 * Can we survive? or should we just shut down?
		 */
		pane_close(ci->home);
		return 1;
	}
	return 0;
}

static void mp_resize(struct mp_info *mpi safe, int size)
{
	if (mpi->parts_size >= size)
		return;
	size += 4;
	mpi->parts = realloc(mpi->parts, size * sizeof(struct pane*));
	mpi->parts_size = size;
}

DEF_CMD(mp_add)
{
	struct mp_info *mpi = ci->home->data;
	int n;

	mp_resize(mpi, mpi->nparts+1);
	n = mpi->nparts;
	mpi->nparts += 1;
	mpi->parts[n] = ci->focus;
	pane_add_notify(ci->home, ci->focus, "Notify:Close");
	return 1;
}

static void mp_init_map(void)
{
	mp_map = key_alloc();
	key_add(mp_map, "doc:set-ref", &mp_set_ref);
	key_add(mp_map, "doc:mark-same", &mp_same);
	key_add(mp_map, "doc:step", &mp_step);
	key_add(mp_map, "doc:get-attr", &mp_attr);
	key_add(mp_map, "Close", &mp_close);
	key_add_range(mp_map, "Notify:Close", "Notify:Close\377", &mp_notify_close);
	key_add(mp_map, "multipart-add", &mp_add);
}
DEF_LOOKUP_CMD_DFLT(mp_handle, mp_map, doc_default_cmd);

DEF_CMD(attach_mp)
{
	struct mp_info *mpi;
	struct pane *h;

	mpi = calloc(1, sizeof(*mpi));
	doc_init(&mpi->doc);

	h = pane_register(ci->home, 0, &mp_handle.c, &mpi->doc, NULL);
	mpi->doc.home = h;
	if (h)
		return comm_call(ci->comm2, "callback:doc", h, 0, NULL, NULL, 0);


	free(mpi);
	return -1;
}

void edlib_init(struct pane *ed safe)
{
	mp_init_map();
	call_comm("global-set-command", ed, 0, NULL, "attach-doc-multipart", 0, &attach_mp);
}
