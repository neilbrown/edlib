/*
 * Copyright Neil Brown ©2016-2023 <neil@brown.name>
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
 */
#define GET_REFS(_mark) ((unsigned long)((_mark)->mdata))
#define SET_REFS(_mark, val) ((_mark)->mdata = (void*)(unsigned long)(val))
#define ADD_REFS(_mark, inc) SET_REFS(_mark, GET_REFS(_mark) + (inc))

#include "core.h"

struct mp_info {
	struct doc	doc;
	int		nparts;
	int		parts_size;
	struct part {
		struct pane	*pane;
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

	if (!m->ref.m || GET_REFS(m->ref.m) == 1)
		return;
	/* Mark is shared, make it unshared */
	m2 = mark_dup(m->ref.m);
	ADD_REFS(m->ref.m, -1);
	SET_REFS(m2, 1);
	m->ref.m = m2;
}

static void post_move(struct mark *m)
{
	/* m->ref.m might have moved.  If so, move m in the list of
	 * marks so marks in this document are still properly ordered
	 * Then ensure that if neighbouring marks are at same location,
	 * they use same marks.
	 */
	struct mark *m2, *mtarget;

	if (!m || hlist_unhashed(&m->all))
		return;
	ASSERT(m->ref.m == NULL || GET_REFS(m->ref.m) == 1);
	mtarget = m;
	while ((m2 = mark_next(mtarget)) != NULL &&
	       (m2->ref.docnum < m->ref.docnum ||
		(m2->ref.docnum == m->ref.docnum &&
		 m2->ref.m && m->ref.m &&
		 m2->ref.m->seq < m->ref.m->seq)))
		mtarget = m2;
	if (mtarget != m)
		/* m should be after mtarget */
		mark_to_mark_noref(m, mtarget);

	mtarget = m;
	while ((m2 = mark_prev(mtarget)) != NULL &&
	       (m2->ref.docnum > m->ref.docnum||
		(m2->ref.docnum == m->ref.docnum &&
		 m2->ref.m && m->ref.m &&
		 m2->ref.m->seq > m->ref.m->seq)))
		mtarget = m2;
	if (mtarget != m)
		/* m should be before mtarget */
		mark_to_mark_noref(m, mtarget);

	if (!m->ref.m)
		return;
	ASSERT(GET_REFS(m->ref.m) == 1);
	/* Check if it should be shared */
	m2 = mark_next(m);
	if (m2 && m2->ref.docnum == m->ref.docnum && m2->ref.m) {
		if (m->ref.m != m2->ref.m &&
		    mark_same(m->ref.m, m2->ref.m)) {
			SET_REFS(m->ref.m, 0);
			mark_free(m->ref.m);
			m->ref.m = m2->ref.m;
			ADD_REFS(m->ref.m, 1);
			return;
		}
	}
	m2 = mark_prev(m);
	if (m2 && m2->ref.docnum == m->ref.docnum && m2->ref.m) {
		if (m->ref.m != m2->ref.m &&
		    mark_same(m->ref.m, m2->ref.m)) {
			SET_REFS(m->ref.m, 0);
			mark_free(m->ref.m);
			m->ref.m = m2->ref.m;
			ADD_REFS(m->ref.m, 1);
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
		ADD_REFS(m->ref.m, 1);

	if (inc < 0) {
		/* mark is being discarded, or ref over-written */
		ADD_REFS(m->ref.m, -1);
		if (GET_REFS(m->ref.m) == 0)
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
	for (m = mark_first(d); m; m = mark_next(m)) {
		if (!m->ref.m || m->ref.m->seq <= s) {
			for (m = mark_first(d); m;
			     m = mark_next(m))
				if (m && m->ref.m)
					printf("%p %d %d\n", m, m->seq,
					       m->ref.m->seq);

			abort();
		}
		s = m->ref.m->seq;
	}
	doc_check_consistent(d);
#endif
}

static void change_part(struct mp_info *mpi safe, struct mark *m safe,
			int part, int end)
{
	struct mark *m1;
	struct part *p;

	if (part < 0 || part > mpi->nparts)
		return;
	if (m->ref.m) {
		ASSERT(GET_REFS(m->ref.m) == 1);
		SET_REFS(m->ref.m, 0);
		mark_free(m->ref.m);
		m->ref.m = NULL;
	}
	if (part < mpi->nparts && (p = &mpi->parts[part]) && p->pane) {
		m1 = vmark_new(p->pane, MARK_UNGROUPED, NULL);
		if (m1) {
			call("doc:set-ref", p->pane, !end, m1);
			m->ref.m = m1;
			SET_REFS(m1, 1);
		}
	} else
		m->ref.m = NULL;
	m->ref.docnum = part;
}

static void mp_normalize(struct mp_info *mpi safe, struct mark *m safe,
			 const char *vis)
{
	/* If points the end of a document, point to the start
	 * of the next instead.
	 */
	struct part *p;
	while (m->ref.m && (p = &mpi->parts[m->ref.docnum]) && p->pane &&
	       doc_following(p->pane, m->ref.m) == WEOF) {
		int n = m->ref.docnum + 1;
		while (n < mpi->nparts && vis && vis[n] == 'i')
			n += 1;
		change_part(mpi, m, n, 0);
	}
}

DEF_CMD(mp_close)
{
	struct mp_info *mpi = ci->home->data;
	int i;
	struct mark *m;

	for (m = mark_first(&mpi->doc); m ; m = mark_next(m))
		if (m->ref.m) {
			struct mark *m2 = m->ref.m;
			m->ref.m = NULL;
			ADD_REFS(m2, -1);
			if (GET_REFS(m2) == 0)
				mark_free(m2);
		}
	for (i = 0; i < mpi->nparts; i++) {
		struct pane *p = mpi->parts[i].pane;
		if (p)
			call("doc:closed", p);
	}
	return Efallthrough;
}

DEF_CMD(mp_free)
{
	struct mp_info *mpi = ci->home->data;

	free(mpi->parts);
	unalloc(mpi, pane);
	return 1;
}

DEF_CMD(mp_set_ref)
{
	struct mp_info *mpi = ci->home->data;
	const char *vis = ci->str && (int)strlen(ci->str) >= mpi->nparts ?
		ci->str : NULL;
	int ret = 1;

	if (!ci->mark)
		return Enoarg;

	/* Need to trigger a point:moved notification.  FIXME I wonder
	 * if this can be simpler
	 */
	mark_step(ci->mark, 0);

	if (!ci->mark->ref.m && !ci->mark->ref.docnum) {
		/* First time set-ref was called */
		pre_move(ci->mark);
		change_part(mpi, ci->mark, 0, 0);
		mark_to_end(ci->home, ci->mark, 0);
		post_move(ci->mark);
	}
	pre_move(ci->mark);

	if (ci->num == 1) {
		/* start */
		int n = 0;
		while (n < mpi->nparts && vis && vis[n] == 'i')
			n += 1;
		change_part(mpi, ci->mark, n, 0);
		mp_normalize(mpi, ci->mark, vis);
	} else
		change_part(mpi, ci->mark, mpi->nparts, 1);

	post_move(ci->mark);
	mp_check_consistent(mpi);
	return ret;
}

static int mp_step(struct pane *home safe, struct mark *mark safe,
		   int forward, int move, const char *str)
{
	struct mp_info *mpi = home->data;
	struct mark *m1 = NULL;
	struct mark *m = mark;
	const char *vis = str && (int)strlen(str) >= mpi->nparts ?
		str : NULL;
	int n;
	int ret;

	/* Document access commands are handled by the 'cropper'.  First
	 * we need to substitute the marks, then call the cropper which
	 * calls the document.  Then make sure the marks are still in
	 * order.
	 */

	mp_check_consistent(mpi);

	if (move) {
		mark_step(m, forward);
		pre_move(m);
	}

	m1 = m->ref.m;

	if (m->ref.docnum >= mpi->nparts)
		ret = -1;
	else
		ret = home_call(mpi->parts[m->ref.docnum].pane,
				"doc:char", home,
				move ? (forward ? 1 : -1) : 0,
				m1, str,
				move ? 0 : (forward ? 1 : -1),
				NULL, NULL);
	while (ret == CHAR_RET(WEOF) || ret == -1) {
		if (!move && m == mark) {
			/* don't change mark when not moving */
			m = mark_dup(m);
			pre_move(m);
		}
		if (forward) {
			if (m->ref.docnum >= mpi->nparts)
				break;
			n = m->ref.docnum + 1;
			while (n < mpi->nparts && vis && vis[n] == 'i')
				n += 1;
			change_part(mpi, m, n, 0);
		} else {
			n = m->ref.docnum - 1;
			while (n >= 0 && vis && vis[n] == 'i')
				n -= 1;
			if (n < 0)
				break;
			change_part(mpi, m, n, 1);
		}
		m1 = m->ref.m;
		if (m->ref.docnum >= mpi->nparts)
			ret = -1;
		else
			ret = home_call(mpi->parts[m->ref.docnum].pane,
					"doc:char", home,
					move ? (forward ? 1 : -1) : 0,
					m1, str,
					move ? 0 : (forward ? 1 : -1));
	}
	if (move) {
		mp_normalize(mpi, mark, vis);
		post_move(mark);
	}

	if (m != mark)
		mark_free(m);

	mp_check_consistent(mpi);
	return ret == -1 ? (int)CHAR_RET(WEOF) : ret;
}

DEF_CMD(mp_char)
{
	struct mark *m = ci->mark;
	struct mark *end = ci->mark2;
	int steps = ci->num;
	int forward = steps > 0;
	int ret = Einval;

	if (!m)
		return Enoarg;
	if (end && mark_same(m, end))
		return 1;
	if (end && (end->seq < m->seq) != (steps < 0))
		/* Can never cross 'end' */
		return Einval;
	while (steps && ret != CHAR_RET(WEOF) && (!end || !mark_same(m, end))) {
		ret = mp_step(ci->home, m, forward, 1, ci->str);
		steps -= forward*2 - 1;
	}
	if (end)
		return 1 + (forward ? ci->num - steps : steps - ci->num);
	if (ret == CHAR_RET(WEOF) || ci->num2 == 0)
		return ret;
	if (ci->num && (ci->num2 < 0) == forward)
		return ret;
	/* Want the 'next' char */
	return mp_step(ci->home, m, ci->num2 > 0, 0, ci->str);
}

DEF_CMD(mp_step_part)
{
	/* Step forward or backward to part boundary.
	 * Stepping forward takes us to start of next part.
	 * Stepping backward takes us to start of this
	 * part - we might not move.
	 * if ->num is -1, step to start of previous part
	 * Return part number plus 1.
	 * If ->str is given, only consider visible parts.
	 */
	struct mp_info *mpi = ci->home->data;
	struct mark *m = ci->mark;
	const char *vis = ci->str && (int)strlen(ci->str) >= mpi->nparts ?
		ci->str : NULL;
	int start;
	int first_vis;
	int n;

	if (!m)
		return Enoarg;
	pre_move(m);
	start = m->ref.docnum;
	n = start;
	if (ci->num > 0) {
		/* Forward - start of next part */
		n += 1;
		while (n < mpi->nparts && vis && vis[n] == 'i')
			n += 1;
	} else if (ci->num < 0) {
		/* Backward - start of prev part */
		n -= 1;
		while (n >= 0 && vis && vis[n] == 'i')
			n -= 1;
		if (n < 0)
			n = m->ref.docnum;
	}
	/* otherwise start of this part */
	change_part(mpi, m, n, 0);

	/* If this part is empty, need to move to next visible part */
	mp_normalize(mpi, m, vis);
	first_vis = 0;
	while (vis && vis[first_vis] == 'i')
		first_vis++;
	while (ci->num < 0 && m->ref.docnum == start && n > first_vis) {
		/* didn't move - must have an empty part, try further */
		n -= 1;
		change_part(mpi, m, n, 0);
		mp_normalize(mpi, m, vis);
	}
	post_move(m);
	if (ci->num && start == m->ref.docnum)
		return Efail;
	return m->ref.docnum + 1;
}

DEF_CMD(mp_get_boundary)
{
	/* return a mark past which rendering must not go. */
	struct mark *m = ci->mark;

	if (!m || !ci->comm2)
		return Enoarg;
	m = mark_dup(m);
	call("doc:step-part", ci->home, ci->num, m);
	comm_call(ci->comm2, "cb", ci->focus, 0, m);
	return 1;
}

struct mp_cb {
	struct command c;
	struct command *cb;
	struct pane *p safe;
	struct mark *m safe;
	int last_ret;
};

DEF_CB(mp_content_cb)
{
	struct mp_cb *c = container_of(ci->comm, struct mp_cb, c);
	struct mark *m1 = NULL;

	if (ci->mark) {
		m1 = c->m;
		pre_move(m1);
		if (m1->ref.m)
			mark_to_mark(m1->ref.m, ci->mark);
		post_move(m1);
	}

	c->last_ret = comm_call(c->cb, ci->key, c->p,
				ci->num, m1, ci->str,
				ci->num2, NULL, ci->str2,
				ci->x, ci->y);
	return c->last_ret;
}

DEF_CMD(mp_content)
{
	/* Call doc:content on any visible docs in the range.
	 * Callback must re-wrap any marks
	 */
	struct mp_info *mpi = ci->home->data;
	struct mp_cb cb;
	struct mark *m, *m2;
	const char *invis = ci->str;
	int ret = 1;

	if (!ci->mark || !ci->comm2)
		return Enoarg;
	m = mark_dup(ci->mark);
	m2 = ci->mark2;
	cb.last_ret = 1;
	while (cb.last_ret > 0 && m->ref.docnum < mpi->nparts &&
	       (!m2 || m->ref.docnum <= m2->ref.docnum)) {
		/* Need to call doc:content on this document */
		int n = m->ref.docnum;
		if ((!invis || invis[n] != 'i') && m->ref.m) {
			struct mark *m2a = NULL;
			struct mark *mtmp = NULL;
			cb.c = mp_content_cb;
			cb.cb = ci->comm2;
			cb.p = ci->focus;
			cb.m = m;

			if (m->ref.m)
				mtmp = mark_dup(m->ref.m);
			if (m2 && m2->ref.docnum == n && m2->ref.m)
				m2a = mark_dup(m2->ref.m);

			ret = home_call_comm(mpi->parts[n].pane,
					     ci->key, ci->home, &cb.c,
					     ci->num, mtmp, NULL,
					     ci->num2, m2a);
			mark_free(m2a);
			mark_free(mtmp);
			if (ret < 0)
				break;
		}
		if (cb.last_ret > 0) {
			pre_move(m);
			change_part(mpi, m, n+1, 0);
			post_move(m);
		}
	}
	mark_free(m);
	return ret;
}

DEF_CMD(mp_attr)
{
	struct mp_info *mpi = ci->home->data;
	struct mark *m1 = NULL;
	struct part *p;
	int ret = Efallthrough;
	int d;
	const char *attr = ci->str;

	if (!ci->mark || !attr)
		return Enoarg;

	m1 = ci->mark->ref.m;
	d = ci->mark->ref.docnum;

	if (d < mpi->nparts && m1 && (p = &mpi->parts[d]) &&
	    p->pane &&  doc_following(p->pane, m1) == WEOF)
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
		comm_call(ci->comm2, "callback:get_attr", ci->focus,
			  0, ci->mark, n, 0, NULL, attr);
		return 1;
	}

	if (d >= mpi->nparts || d < 0)
		return 1;

	if (attr != ci->str) {
		/* Get a pane attribute, not char attribute */
		char *s = pane_attr_get(mpi->parts[d].pane, attr);
		if (s)
			return comm_call(ci->comm2, "callback", ci->focus,
					 0, ci->mark, s, 0, NULL, ci->str);
		return 1;
	}

	p = &mpi->parts[d];
	if (d != ci->mark->ref.docnum && p->pane) {
		m1 = vmark_new(p->pane, MARK_UNGROUPED, NULL);
		call("doc:set-ref", p->pane,
		     (d > ci->mark->ref.docnum), m1);
	}

	if (p->pane)
		ret = home_call(p->pane,
				ci->key, ci->focus, ci->num, m1, ci->str,
				ci->num2, NULL, ci->str2, 0,0, ci->comm2);
	if (d != ci->mark->ref.docnum)
		mark_free(m1);
	return ret;
}

DEF_CMD(mp_set_attr)
{
	struct mp_info *mpi = ci->home->data;
	struct part *p;
	struct mark *m = ci->mark;
	struct mark *m1;
	int dn;
	const char *attr = ci->str;

	if (!attr)
		return Enoarg;
	if (!m)
		return Efallthrough;
	dn = m->ref.docnum;
	m1 = m->ref.m;

	if (strncmp(attr, "multipart-", 10) == 0) {
		/* Set an attribute on a part */
		if (strncmp(attr, "multipart-prev:", 15) == 0 &&
		    dn > 0 && (p = &mpi->parts[dn-1]) && p->pane)
			attr_set_str(&p->pane->attrs,
				     attr+15, ci->str2);
		else if (strncmp(attr, "multipart-next:", 15) == 0 &&
			 dn < mpi->nparts && (p = &mpi->parts[dn+1]) && p->pane)
			attr_set_str(&p->pane->attrs,
				     attr+15, ci->str2);
		else if (strncmp(attr, "multipart-this:", 15) == 0 &&
			 (p = &mpi->parts[dn]) && p->pane)
			attr_set_str(&p->pane->attrs,
				     attr+15, ci->str2);
		else
			return Efail;
		return 1;
	}
	/* Send the request to a sub-document */
	p = &mpi->parts[dn];
	if (p->pane)
		return call(ci->key, p->pane, ci->num, m1, ci->str,
			    0, NULL, ci->str2);
	return Efail;
}

DEF_CMD(mp_notify_close)
{
	/* sub-document has been closed.
	 * Can we survive? or should we just shut down?
	 */
	struct mp_info *mpi = ci->home->data;
	int i;

	for (i = 0; i < mpi->nparts; i++)
		if (mpi->parts[i].pane == ci->focus) {
			/* sub-document has been closed.
			 * Can we survive? or should we just shut down?
			 */
			mpi->parts[i].pane = NULL;
			pane_close(ci->home);
			return 1;
		}
	/* Not a sub-pane, maybe an owner for vmarks */
	return Efallthrough;
}

DEF_CMD(mp_notify_viewers)
{
	/* The autoclose document wants to know if it should close.
	 * tell it "no" */
	return 1;
}

DEF_CMD(mp_doc_replaced)
{
	/* Something changed in a component, report that the
	 * whole doc changed - simplest for now.
	 */
	pane_notify("doc:replaced", ci->home);
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
	home_call(ci->focus, "doc:request:doc:notify-viewers", ci->home);
	home_call(ci->focus, "doc:request:doc:replaced", ci->home);

	return 1;
}

DEF_CMD(mp_forward_by_num)
{
	struct mp_info *mpi = ci->home->data;
	struct mark *m1 = NULL, *m2 = NULL;
	struct part *p;
	const char *key;
	int d;
	int ret;

	key = ksuffix(ci, "doc:multipart-");
	d = atoi(key);
	key = strchr(key, '-');
	if (!key)
		return Einval;
	key += 1;

	if (d >= mpi->nparts || d < 0)
		return 1;

	if (ci->mark && ci->mark->ref.docnum == d)
		m1 = ci->mark->ref.m;
	if (ci->mark2 && ci->mark2->ref.docnum == d)
		m2 = ci->mark2->ref.m;

	p = &mpi->parts[d];
	if (p->pane)
		ret = call(key, p->pane, ci->num, m1, ci->str,
			   ci->num2, m2, ci->str2, ci->x, ci->y, ci->comm2);
	else
		ret = Efail;
	return ret;
}

DEF_CMD(mp_get_part)
{
	struct mp_info *mpi = ci->home->data;
	struct part *p;
	int d = ci->num;

	if (d < 0 || d >= mpi->nparts)
		return Einval;
	p = &mpi->parts[d];
	if (p->pane)
		comm_call(ci->comm2, "cb", p->pane);
	return 1;
}

DEF_CMD(mp_forward)
{
	/* forward this command to this/next/prev document based on
	 * ci->mark2.
	 * ci->mark is forwarded if it is in same document
	 */
	struct mp_info *mpi = ci->home->data;
	struct part *p;
	struct mark *m1, *m2;
	const char *key;
	int d;

	if (!ci->mark2)
		return Enoarg;

	m2 = ci->mark2->ref.m;
	d = ci->mark2->ref.docnum;

	if (d < mpi->nparts && m2 && (p = &mpi->parts[d]) && p->pane &&
	    doc_following(p->pane, m2) == WEOF)
		/* at the wrong end of a part */
		d += 1;

	if ((key = ksuffix(ci, "multipart-next:"))[0]) {
		d += 1;
		if (d >= mpi->nparts)
			return 1;
	} else if ((key = ksuffix(ci, "multipart-prev:"))[0]) {
		d -= 1;
		if (d < 0)
			return 1;
	} else if ((key = ksuffix(ci, "multipart-this:"))[0]) {
		;
	} else return Einval;

	if (d >= mpi->nparts || d < 0)
		return 1;

	m1 = NULL;
	if (ci->mark && ci->mark->ref.docnum == d)
		m1 = ci->mark->ref.m;
	p = &mpi->parts[d];
	if (p->pane)
		return call(key, p->pane, ci->num, m1, ci->str,
			    ci->num2, NULL, ci->str2, 0,0, ci->comm2);
	return Efail;
}

DEF_CMD(mp_val_marks)
{
	struct mark *m1, *m2;

	if (!ci->mark || !ci->mark2)
		return Enoarg;

	if (ci->mark->ref.docnum < ci->mark2->ref.docnum)
		return 1;
	if (ci->mark->ref.docnum > ci->mark2->ref.docnum) {
		LOG("mp_val_marks: docs not in order");
		return Efalse;
	}

	m1 = ci->mark->ref.m;
	m2 = ci->mark->ref.m;
	if (m1 == m2)
		return 1;
	if (m1 && m2 && m1->seq > m2->seq) {
		LOG("mp_val_marks: subordinate marks out of order!");
		return Efalse;
	}
	if (!m1)
		LOG("mp_val_marks: m1 is NULL");
	else if (!m2 || marks_validate(m1, m2))
		return 1;
	return Efalse;
}

static void mp_init_map(void)
{
	mp_map = key_alloc();
	key_add_chain(mp_map, doc_default_cmd);
	key_add(mp_map, "doc:set-ref", &mp_set_ref);
	key_add(mp_map, "doc:char", &mp_char);
	key_add(mp_map, "doc:content", &mp_content);
	key_add(mp_map, "doc:content-bytes", &mp_content);
	key_add(mp_map, "doc:get-attr", &mp_attr);
	key_add(mp_map, "doc:set-attr", &mp_set_attr);
	key_add(mp_map, "doc:step-part", &mp_step_part);
	key_add(mp_map, "doc:get-boundary", &mp_get_boundary);
	key_add(mp_map, "Close", &mp_close);
	key_add(mp_map, "Free", &mp_free);
	key_add(mp_map, "Notify:Close", &mp_notify_close);
	key_add(mp_map, "doc:notify-viewers", &mp_notify_viewers);
	key_add(mp_map, "doc:replaced", &mp_doc_replaced);
	key_add(mp_map, "multipart-add", &mp_add);
	key_add(mp_map, "debug:validate-marks", &mp_val_marks);
	key_add(mp_map, "doc:multipart:get-part", &mp_get_part);
	key_add_prefix(mp_map, "multipart-this:", &mp_forward);
	key_add_prefix(mp_map, "multipart-next:", &mp_forward);
	key_add_prefix(mp_map, "multipart-prev:", &mp_forward);
	key_add_prefix(mp_map, "doc:multipart-", &mp_forward_by_num);
}
DEF_LOOKUP_CMD(mp_handle, mp_map);

DEF_CMD(attach_mp)
{
	struct mp_info *mpi;
	struct pane *h;

	alloc(mpi, pane);

	h = doc_register(ci->home, &mp_handle.c, mpi);
	if (h) {
		mpi->doc.refcnt = mp_mark_refcnt;
		attr_set_str(&h->attrs, "render-default", "text");
		return comm_call(ci->comm2, "callback:doc", h);
	}

	free(mpi);
	return Efail;
}

void edlib_init(struct pane *ed safe)
{
	mp_init_map();
	call_comm("global-set-command", ed, &attach_mp, 0, NULL,
		  "attach-doc-multipart");
}
