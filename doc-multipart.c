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

/* mark->mdata in marks we create on individual component documents
 * is used to track if the mark is shared among multiple marks in the
 * multipart document.
 * mdata must be a pointer, subtract ZERO to get a count, and add
 * ZERO to convert back to a pointer.
 */
#define MARK_DATA_PTR char
#define ZERO ((char *)0)
#define ONE (ZERO + 1)

#include "core.h"

struct mp_info {
	struct doc	doc;
	int		nparts;
	int		parts_size;
	struct part {
		struct pane	*pane safe;
	} *parts safe;
};

static struct map *mp_map safe;

/* Before moving a mark, we make sure m->ref.m is not shared.
 * After moving, we make sure the mark is correctly ordered among
 * siblings, and then share if m->ref.m should be shared.
 */
static void pre_move(struct mark *m safe)
{
	struct mark *m2;

	if (!m->ref.m || m->ref.m->mdata == ONE)
		return;
	/* Mark is shared, make it unshared */
	m2 = mark_dup(m->ref.m);
	m->ref.m->mdata = ZERO + (m->ref.m->mdata - ONE);
	m2->mdata = ONE;
	m->ref.m = m2;
}

static void post_move(struct mark *m)
{
	/* m->ref.m might have moved.  If so, move m in the list of
	 * marks so marks in this document are still properly ordered
	 * Then ensure that if neighbouring marks are at same location,
	 * they use same marks.
	 */
	struct mark *m2;

	if (!m || hlist_unhashed(&m->all))
		return;
	ASSERT(m->ref.m == NULL || m->ref.m->mdata == ONE);
	while ((m2 = doc_next_mark_all(m)) != NULL &&
	       (m2->ref.docnum < m->ref.docnum ||
		(m2->ref.docnum == m->ref.docnum &&
		 m2->ref.m && m->ref.m &&
		 m2->ref.m->seq < m->ref.m->seq))) {
		/* m should be after m2 */
		mark_to_mark_noref(m, m2);
	}

	while ((m2 = doc_prev_mark_all(m)) != NULL &&
	       (m2->ref.docnum > m->ref.docnum||
		(m2->ref.docnum == m->ref.docnum &&
		 m2->ref.m && m->ref.m &&
		 m2->ref.m->seq > m->ref.m->seq))) {
		/* m should be before m2 */
		mark_to_mark_noref(m, m2);
	}
	if (!m->ref.m)
		return;
	ASSERT(m->ref.m->mdata == ONE);
	/* Check if it should be shared */
	m2 = doc_next_mark_all(m);
	if (m2 && m2->ref.docnum == m->ref.docnum && m2->ref.m) {
		if (m->ref.m != m2->ref.m &&
		    mark_same(m->ref.m, m2->ref.m)) {
			m->ref.m->mdata = ZERO;
			mark_free(m->ref.m);
			m->ref.m = m2->ref.m;
			m->ref.m->mdata = ONE + (m->ref.m->mdata - ZERO);
			return;
		}
	}
	m2 = doc_prev_mark_all(m);
	if (m2 && m2->ref.docnum == m->ref.docnum && m2->ref.m) {
		if (m->ref.m != m2->ref.m &&
		    mark_same(m->ref.m, m2->ref.m)) {
			m->ref.m->mdata = ZERO;
			mark_free(m->ref.m);
			m->ref.m = m2->ref.m;
			m->ref.m->mdata = ONE + (m->ref.m->mdata - ZERO);
			return;
		}
	}
}

static void mp_mark_refcnt(struct mark *m safe, int inc)
{
	if (!m->ref.m)
		return;

	if (inc > 0)
		/* Duplicate being created of this mark */
		m->ref.m->mdata = ONE + (m->ref.m->mdata - ZERO);

	if (inc < 0) {
		/* mark is being discarded, or ref over-written */
		m->ref.m->mdata = ZERO + (m->ref.m->mdata - ONE);
		if (m->ref.m->mdata == ZERO)
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
	if (m->ref.m) {
		ASSERT(m->ref.m->mdata == ONE);
		m->ref.m->mdata = ZERO;
		mark_free(m->ref.m);
	}
	if (part < mpi->nparts) {
		p = &mpi->parts[part];
		m1 = vmark_new(p->pane, MARK_UNGROUPED);
		if (m1) {
			call("doc:set-ref", p->pane, !end, m1);
			m->ref.m = m1;
			m1->mdata = ONE;
		}
	} else
		m->ref.m = NULL;
	m->ref.docnum = part;
	m->refcnt = mp_mark_refcnt;
}

static void mp_normalize(struct mp_info *mpi safe, struct mark *m safe)
{
	/* If points the end of a document, point to the start
	 * of the next instead.
	 */
	while (m->ref.m &&
	       doc_following_pane(mpi->parts[m->ref.docnum].pane,
				  m->ref.m) == WEOF) {
		change_part(mpi, m, m->ref.docnum + 1, 0);
	}
}

DEF_CMD(mp_close)
{
	struct mp_info *mpi = ci->home->data;
	int i;
	struct mark *m;

	for (m = doc_first_mark_all(&mpi->doc); m ; m = doc_next_mark_all(m))
		if (m->ref.m) {
			struct mark *m2 = m->ref.m;
			m->ref.m = NULL;
			m2->mdata = ZERO + (m2->mdata - ONE);
			if (m2->mdata == ZERO)
				mark_free(m2);
		}
	for (i = 0; i < mpi->nparts; i++)
		call("doc:closed", mpi->parts[i].pane);
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
		return Enoarg;

	if (!ci->mark->ref.m && !ci->mark->ref.docnum) {
		/* First time set-ref was called */
		pre_move(ci->mark);
		change_part(mpi, ci->mark, 0, 0);
		mark_to_end(&mpi->doc, ci->mark, 0);
		post_move(ci->mark);
	}
	pre_move(ci->mark);

	if (ci->num == 1) {
		/* start */
		change_part(mpi, ci->mark, 0, 0);
		mp_normalize(mpi, ci->mark);
	} else
		change_part(mpi, ci->mark, mpi->nparts, 1);

	post_move(ci->mark);
	mp_check_consistent(mpi);
	return ret;
}

DEF_CMD(mp_step)
{
	struct mp_info *mpi = ci->home->data;
	struct mark *m1 = NULL;
	struct mark *m = ci->mark;
	int ret;

	/* Document access commands are handled by the 'cropper'.
	 * First we need to substitute the marks, then call the cropper
	 * which calls the document.  Then make sure the marks are still in order.
	 */
	mp_check_consistent(mpi);
	if (!m)
		return Enoarg;

	mp_check_consistent(mpi);

	if (ci->num2)
		pre_move(m);

	m1 = m->ref.m;

	if (m->ref.docnum == mpi->nparts)
		ret = -1;
	else
		ret = home_call(mpi->parts[m->ref.docnum].pane,
				ci->key, ci->focus, ci->num, m1, ci->str,
				ci->num2, NULL, ci->str2, 0,0, ci->comm2);
	while (ret == CHAR_RET(WEOF) || ret == -1) {
		if (!ci->num2 && m == ci->mark) {
			/* don't change ci->mark when not moving */
			m = mark_dup(m);
			pre_move(m);
		}
		if (ci->num) {
			if (m->ref.docnum >= mpi->nparts)
				break;
			change_part(mpi, m, m->ref.docnum + 1, 0);
		} else {
			if (m->ref.docnum == 0)
				break;
			change_part(mpi, m, m->ref.docnum - 1, 1);
		}
		m1 = m->ref.m;
		if (m->ref.docnum == mpi->nparts)
			ret = -1;
		else
			ret = home_call(mpi->parts[m->ref.docnum].pane,
					ci->key, ci->focus, ci->num, m1, ci->str,
					ci->num2, NULL, ci->str2, 0,0, ci->comm2);
	}
	if (ci->num2) {
		mp_normalize(mpi, ci->mark);
		post_move(ci->mark);
	}

	if (m != ci->mark)
		mark_free(m);

	mp_check_consistent(mpi);
	return ret == -1 ? (int)CHAR_RET(WEOF) : ret;
}

DEF_CMD(mp_step_part)
{
	/* Step forward or backward to part boundary.
	 * Stepping forward takes us to start of next part.
	 * Stepping backward takes us to start of this
	 * part - we might not move.
	 * Return part number plus 1.
	 */
	struct mp_info *mpi = ci->home->data;
	struct mark *m = ci->mark;

	if (!m)
		return Enoarg;
	pre_move(m);
	if (ci->num > 0)
		/* Forward - next part */
		change_part(mpi, m, m->ref.docnum + 1, 0);
	else
		/* Backward - this part */
		change_part(mpi, m, m->ref.docnum, 0);

	mp_normalize(mpi, m);
	post_move(m);
	return m->ref.docnum + 1;
}

DEF_CMD(mp_attr)
{
	struct mp_info *mpi = ci->home->data;
	struct mark *m1 = NULL;
	int ret;
	int d;
	char *attr = ci->str;

	if (!ci->mark || !attr)
		return Enoarg;

	m1 = ci->mark->ref.m;
	d = ci->mark->ref.docnum;

	if (d < mpi->nparts && m1 &&
	    mark_step_pane(mpi->parts[d].pane, m1, 1, 0) == WEOF)
		/* at the wrong end of a part */
		d += 1;

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

	if (strcmp(attr, "multipart:part-num") == 0) {
		char n[11];
		snprintf(n, sizeof(n), "%d", d);
		comm_call(ci->comm2, "callback:get_attr", ci->focus, 0, NULL, n);
		return 1;
	}

	if (d >= mpi->nparts || d < 0)
		return 1;

	if (attr != ci->str) {
		/* Get a pane attribute, not char attribute */
		char *s = pane_attr_get(mpi->parts[d].pane, attr);
		if (s)
			return comm_call(ci->comm2, "callback", ci->focus, 0, NULL, s);
		return 1;
	}

	if (d != ci->mark->ref.docnum) {
		m1 = vmark_new(mpi->parts[d].pane, MARK_UNGROUPED);
		call("doc:set-ref", mpi->parts[d].pane,
		      (d > ci->mark->ref.docnum), m1);
	}

	ret = home_call(mpi->parts[d].pane,
			ci->key, ci->focus, ci->num, m1, ci->str,
			ci->num2, NULL, ci->str2, 0,0, ci->comm2);
	if (d != ci->mark->ref.docnum)
		mark_free(m1);
	return ret;
}

DEF_CMD(mp_set_attr)
{
	struct mp_info *mpi = ci->home->data;
	struct mark *m = ci->mark;
	struct mark *m1;
	int dn;
	char *attr = ci->str;

	if (!attr)
		return Enoarg;
	if (!m)
		return Efallthrough;
	dn = m->ref.docnum;
	m1 = m->ref.m;

	if (dn < mpi->nparts && m1 &&
	     mark_step_pane(mpi->parts[dn].pane, m1, ci->num, 0) == WEOF) {
		/* at the wrong end of a part */
		if (ci->num)
			dn += 1;
		else if (dn > 0)
			dn -= 1;
	}

	if (strncmp(attr, "multipart-prev:", 15) == 0) {
		dn -= 1;
		attr += 15;
	} else if (strncmp(attr, "multipart-next:", 15) == 0) {
		dn += 1;
		attr += 15;
	}
	return Efallthrough;
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
	hlist_for_each_entry(m, &mpi->doc.marks, all)
		if (m->ref.docnum >= n)
			m->ref.docnum ++;
	if (ci->mark)
		/* move mark to start of new part */
		change_part(mpi, ci->mark, n, 0);

	pane_add_notify(ci->home, ci->focus, "Notify:Close");
	home_call(ci->focus, "Request:Notify:doc:viewers", ci->home);

	return 1;
}

DEF_CMD(mp_forward)
{
	/* forward this command to this/next/prev document based on
	 * ci->mark2.
	 * ci->mark is forwarded if it is in same document
	 */
	struct mp_info *mpi = ci->home->data;
	struct mark *m1, *m2;
	char *key;
	int d;

	if (!ci->mark2)
		return Enoarg;

	m2 = ci->mark2->ref.m;
	d = ci->mark2->ref.docnum;

	if (d < mpi->nparts && m2 &&
	     mark_step_pane(mpi->parts[d].pane, m2, 1, 0) == WEOF)
		/* at the wrong end of a part */
		d += 1;

	key = ci->key;
	if (strncmp(key, "multipart-next:", 15) == 0) {
		d += 1;
		key += 15;
		if (d >= mpi->nparts)
			return 1;
	} else if (strncmp(key, "multipart-prev:", 15) == 0) {
		d -= 1;
		key += 15;
		if (d < 0)
			return 1;
	} else if (strncmp(key, "multipart-this:", 15) == 0)
		key += 15;
	else return Einval;

	if (d >= mpi->nparts || d < 0)
		return 1;

	m1 = NULL;
	if (ci->mark && ci->mark->ref.docnum == d)
		m1 = ci->mark->ref.m;
	return call(key, mpi->parts[d].pane, ci->num, m1, ci->str,
		    ci->num2, NULL, ci->str2, 0,0, ci->comm2);
}

static void mp_init_map(void)
{
	mp_map = key_alloc();
	key_add_chain(mp_map, doc_default_cmd);
	key_add(mp_map, "doc:set-ref", &mp_set_ref);
	key_add(mp_map, "doc:step", &mp_step);
	key_add(mp_map, "doc:get-attr", &mp_attr);
	key_add(mp_map, "doc:set-attr", &mp_set_attr);
	key_add(mp_map, "doc:step-part", &mp_step_part);
	key_add(mp_map, "Close", &mp_close);
	key_add(mp_map, "Notify:Close", &mp_notify_close);
	key_add(mp_map, "Notify:doc:viewers", &mp_notify_viewers);
	key_add(mp_map, "multipart-add", &mp_add);
	key_add_range(mp_map, "multipart-this:", "multipart-this;", &mp_forward);
	key_add_range(mp_map, "multipart-next:", "multipart-next;", &mp_forward);
	key_add_range(mp_map, "multipart-prev:", "multipart-prev;", &mp_forward);
}
DEF_LOOKUP_CMD(mp_handle, mp_map);

DEF_CMD(attach_mp)
{
	struct mp_info *mpi;
	struct pane *h;

	mpi = calloc(1, sizeof(*mpi));
	doc_init(&mpi->doc);

	h = pane_register(ci->home, 0, &mp_handle.c, &mpi->doc, NULL);
	mpi->doc.home = h;
	if (h)
		return comm_call(ci->comm2, "callback:doc", h);

	free(mpi);
	return Esys;
}

void edlib_init(struct pane *ed safe)
{
	mp_init_map();
	call_comm("global-set-command", ed, &attach_mp, 0, NULL, "attach-doc-multipart");
}
