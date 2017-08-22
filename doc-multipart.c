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
	int docnum; /* may be 'nparts', in which case 'm' == NULL */
};

#include "core.h"

struct mp_info {
	struct doc	doc;
	int		nparts;
	int		parts_size;
	struct part {
		struct pane	*pane safe;
		int		visible;
	} *parts safe;
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
	struct part *p;

	if (part < 0 || part > mpi->nparts)
		return;
	if (m->ref.m)
		mark_free(m->ref.m);
	if (part < mpi->nparts) {
		p = &mpi->parts[part];
		m1 = vmark_new(p->pane, MARK_UNGROUPED);
		call3("doc:set-ref", p->pane, !end, m1);
		m->ref.m = m1;
	} else
		m->ref.m = NULL;
	m->ref.docnum = part;
	m->refcnt = mp_mark_refcnt;
}

DEF_CMD(mp_close)
{
	struct mp_info *mpi = ci->home->data;
	int i;
	for (i = 0; i < mpi->nparts; i++)
		call3("doc:closed", mpi->parts[i].pane, 0, NULL);
	doc_free(&mpi->doc);
	free(mpi->parts);
	free(mpi);
	return 1;
}

DEF_CMD(mp_set_ref)
{
	struct mp_info *mpi = ci->home->data;
	int ret = 1;

	if (!ci->mark)
		return -1;

	if (!ci->mark->ref.m && !ci->mark->ref.docnum) {
		/* First time set-ref was called */
		change_part(mpi, ci->mark, 0, 0);
		mark_to_end(&mpi->doc, ci->mark, 0);
		reset_mark(ci->mark);
	}

	if (ci->numeric == 1)
		/* start */
		change_part(mpi, ci->mark, 0, 0);
	else
		change_part(mpi, ci->mark, mpi->nparts, 1);

	reset_mark(ci->mark);
	mp_check_consistent(mpi);
	return ret;
}

DEF_CMD(mp_same)
{
	struct mp_info *mpi = ci->home->data;
	struct doc_ref d1, d2;

	if (!ci->mark || !ci->mark2)
		return -1;

	mp_check_consistent(mpi);

	if (ci->mark->seq < ci->mark2->seq) {
		d1 = ci->mark->ref;
		d2 = ci->mark2->ref;
	} else {
		d1 = ci->mark2->ref;
		d2 = ci->mark->ref;
	}

	if (d1.m &&
	    mpi->parts[d1.docnum].visible &&
	    doc_following_pane(mpi->parts[d1.docnum].pane, d1.m) == CHAR_RET(WEOF)) {
		/* End of part */
		d1.docnum++;
		d1.m = NULL;
	}
	if (d2.m && d2.docnum &&
	    mpi->parts[d2.docnum].visible &&
	    doc_prior_pane(mpi->parts[d2.docnum].pane, d2.m) == CHAR_RET(WEOF)) {
		/* Start of part */
		d2.docnum--;
		d2.m = NULL;
	}
	while (d1.docnum < mpi->nparts - 1 &&
	       !mpi->parts[d1.docnum].visible &&
	       !mpi->parts[d1.docnum+1].visible) {
		d1.m = NULL;
		d1.docnum++;
	}
	while (d2.docnum > 0 &&
	       !mpi->parts[d2.docnum].visible &&
	       !mpi->parts[d2.docnum-1].visible) {
		d2.m = NULL;
		d2.docnum--;
	}
	if (d2.docnum < d1.docnum)
		/* Nothing visible between the points */
		return 1;
	if (d1.docnum == d2.docnum) {
		if (ci->mark->ref.docnum == mpi->nparts)
			return 1;
		if (!mpi->parts[d1.docnum].visible)
			return 1;
		if (!d1.m || !d2.m)
			/* marks are at either end of a visible part.
			 * Assume part is not empty...
			 */
			return 2;
		return call_home7(mpi->parts[ci->mark->ref.docnum].pane,
				  "doc:mark-same", ci->focus, 0, d1.m, NULL,
				  0, NULL, d2.m, NULL);
	}

	/* Marks are in different visible documents.
	 * If they had been at the end, they would have
	 * been moved to the next.
	 * So the marks cannot be the same.
	 */
	return 2;
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

	m1 = ci->mark->ref.m;

	mp_check_consistent(mpi);

	if (ci->mark->ref.docnum == mpi->nparts ||
	    !mpi->parts[ci->mark->ref.docnum].visible)
		ret = -1;
	else
		ret = call_home7(mpi->parts[ci->mark->ref.docnum].pane,
				 ci->key, ci->focus, ci->numeric, m1, ci->str,
				 ci->extra,ci->str2, NULL, ci->comm2);
	while (ret == CHAR_RET(WEOF) || ret == -1) {
		if (ci->numeric) {
			if (ci->mark->ref.docnum >= mpi->nparts)
				break;
			do {
				change_part(mpi, ci->mark, ci->mark->ref.docnum + 1, 0);
			} while (ci->mark->ref.docnum < mpi->nparts &&
				 !mpi->parts[ci->mark->ref.docnum].visible);
		} else {
			if (ci->mark->ref.docnum == 0)
				break;
			do {
				change_part(mpi, ci->mark, ci->mark->ref.docnum - 1, 1);
			} while (ci->mark->ref.docnum > 0 &&
				 !mpi->parts[ci->mark->ref.docnum].visible);
		}
		m1 = ci->mark->ref.m;
		if (ci->mark->ref.docnum == mpi->nparts ||
		    !mpi->parts[ci->mark->ref.docnum].visible)
			ret = -1;
		else
			ret = call_home7(mpi->parts[ci->mark->ref.docnum].pane,
					 ci->key, ci->focus, ci->numeric, m1, ci->str,
					 ci->extra,ci->str2, NULL, ci->comm2);
	}
	reset_mark(ci->mark);

	mp_check_consistent(mpi);
	return ret == -1 ? (int)CHAR_RET(WEOF) : ret;
}

DEF_CMD(mp_attr)
{
	struct mp_info *mpi = ci->home->data;
	struct mark *m1 = NULL;
	int ret;
	int d;
	char *attr = ci->str;

	if (!ci->mark || !attr)
		return -1;

	m1 = ci->mark->ref.m;
	d = ci->mark->ref.docnum;

	if (strncmp(attr, "multipart-next:", 15) == 0) {
		d += 1;
		attr += 15;
		if (d >= mpi->nparts)
			return 1;
	} else if (strncmp(attr, "multipart-prev:", 15) == 0) {
		d -= 1;
		attr += 15;
		if (d < 0)
			return 1;
	} else if (strncmp(attr, "multipart-this:", 15) == 0)
		attr += 15;

	if (strcmp(attr, "multipart:visible") == 0) {
		if (d >= 0 && d < mpi->nparts &&
		    mpi->parts[d].visible)
			comm_call(ci->comm2, "callback:get_attr", ci->focus, 0, NULL, "1", 0);
		else
			comm_call(ci->comm2, "callback:get_attr", ci->focus, 0, NULL, "0", 0);
		return 1;
	}

	if (attr != ci->str) {
		/* Get a pane attribute, not char attribute */
		char *s = pane_attr_get(mpi->parts[d].pane, attr);
		if (s)
			return comm_call(ci->comm2, "callback", ci->focus, 0, NULL,
					 s, 0);
		return 1;
	}

	if (d == ci->mark->ref.docnum) {
		/* choose a visible document */
		if (ci->numeric != 0) {
			/* forward */
			while (d < mpi->nparts && !mpi->parts[d].visible)
				d++;
			if (d >= mpi->nparts)
				return 1;
		} else {
			/* backward */
			while (d >= 0 && !mpi->parts[d].visible)
				d++;
			if (d < 0)
				return 1;
		}
	}
	if (d != ci->mark->ref.docnum) {
		m1 = vmark_new(mpi->parts[d].pane, MARK_UNGROUPED);
		call3("doc:set-ref", mpi->parts[d].pane,
		      (d > ci->mark->ref.docnum), m1);
	}

	ret = call_home7(mpi->parts[d].pane,
			 ci->key, ci->focus, ci->numeric, m1, ci->str,
			 ci->extra,ci->str2, NULL, ci->comm2);
	if (d != ci->mark->ref.docnum)
		mark_free(m1);
	return ret;
}

DEF_CMD(mp_set_attr)
{
	struct mp_info *mpi = ci->home->data;
	struct mark *m = ci->mark;
	int dn;
	char *attr = ci->str;

	if (!attr)
		return -1;
	if (!m)
		return 0;
	dn = m->ref.docnum;
	if (strncmp(attr, "multipart-prev:", 15) == 0) {
		dn -= 1;
		attr += 15;
	} else if (strncmp(attr, "multipart-next:", 15) == 0) {
		dn += 1;
		attr += 15;
	}
	if (strcmp(attr, "multipart:visible") != 0)
		return 0;

	if (dn < 0 || dn >= mpi->nparts)
		return -1;

	if ((ci->str2 && atoi(ci->str2) > 0) ||
	    (ci->str2 == NULL && ci->extra == 1 && ci->numeric > 0))
		mpi->parts[dn].visible = 1;
	else
		mpi->parts[dn].visible = 0;
	pane_notify(ci->home, "Notify:doc:Replace", NULL, NULL, NULL, NULL, 0, 0, NULL);
	return 1;
}

DEF_CMD(mp_notify_close)
{
	/* sub-document has been closed.
	 * Can we survive? or should we just shut down?
	 */
	pane_close(ci->home);
	return 1;
}

DEF_CMD(mp_notify_viewers)
{
	/* The autoclose document wants to know if it should close.
	 * tell it "no" */
	return 1;
}

static void mp_resize(struct mp_info *mpi safe, int size)
{
	if (mpi->parts_size >= size)
		return;
	size += 4;
	mpi->parts = realloc(mpi->parts, size * sizeof(struct part));
	mpi->parts_size = size;
}

DEF_CMD(mp_add)
{
	struct mp_info *mpi = ci->home->data;
	struct mark *m;
	int n;

	mp_resize(mpi, mpi->nparts+1);
	if (ci->mark == NULL)
		n = mpi->nparts;
	else
		n = ci->mark->ref.docnum;
	memmove(&mpi->parts[n+1], &mpi->parts[n],
		(mpi->nparts - n)*sizeof(mpi->parts[0]));
	mpi->nparts += 1;
	mpi->parts[n].pane = ci->focus;
	mpi->parts[n].visible = 1;
	hlist_for_each_entry(m, &mpi->doc.marks, all)
		if (m->ref.docnum >= n)
			m->ref.docnum ++;
	if (ci->mark)
		/* move mark to start of new part */
		change_part(mpi, ci->mark, n, 0);

	pane_add_notify(ci->home, ci->focus, "Notify:Close");
	call_home(ci->focus, "Request:Notify:doc:viewers", ci->home, 0, NULL, NULL);

	return 1;
}

static void mp_init_map(void)
{
	mp_map = key_alloc();
	key_add(mp_map, "doc:set-ref", &mp_set_ref);
	key_add(mp_map, "doc:mark-same", &mp_same);
	key_add(mp_map, "doc:step", &mp_step);
	key_add(mp_map, "doc:get-attr", &mp_attr);
	key_add(mp_map, "doc:set-attr", &mp_set_attr);
	key_add(mp_map, "Close", &mp_close);
	key_add(mp_map, "Notify:Close", &mp_notify_close);
	key_add(mp_map, "Notify:doc:viewers", &mp_notify_viewers);
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
