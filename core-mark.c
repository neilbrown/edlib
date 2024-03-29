/*
 * Copyright Neil Brown ©2015-2023 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * Marks and Points.
 *
 * A 'Mark' is a reference to a location in a text.  The location
 * is between two characters and remains there while characters
 * are added or removed before or after.
 * All marks are linked together in text order and are assigned
 * sparse ordered numbers so that it is easy to determine the
 * relative order of two points.
 *
 * Each mark is optionally in two lists.  There is one list that
 * contains all marks on a given text, and arbitrary other lists
 * that contain only select marks.  A given mark can only be in one
 * of these extra lists - except for points which are described below.
 *
 * A mark has a list of attributes and a pointer to a handler
 * which can be called in various circumstances to customise
 * handling of different sections of text.
 *
 * A 'point' is a special mark which identifies a place where
 * things can happen.  Text can be added or removed, marks can
 * be created or destroyed, only at a 'point'.
 * A 'point' is on all lists.  This allows nearby marks on any list to be
 * found quickly.
 *
 * As the 'group' lists can hold either marks or points, and with a
 * different anchor in each, we use 'tlist_head'.  Pointers in a
 * tlist use the bottom 2 bits to store a type.  We only have
 * two types: mark or point.  When we find a point we know which of the
 * various lists because we know the index of the list we are walking.
 *
 * The text buffer can only be modified at a point.
 * Prior to deleting text, all marks/points must be moved off the removed
 * section.  After adding text ... something.
 *
 * A mark or point can be deleted at any time with impunity.
 * A point can be duplicated, or can fork a mark.  When this happens
 * we might need to re-assign some seq numbers to keep them sparse.
 * A point can move forward or backward, or can jump to a mark.
 * When that happens, various lists might need to be adjusted and
 * seq number might need to be reassigned.
 *
 * There can only be one (non-borrowed) reference to a mark.  So
 * a second reference is wanted, the mark must be duplicated.
 * How these references are stored is not yet clear.  Maybe that is
 * up to whatever module uses it.
 *
 * Each mark has a 'type' identifying which group it is of.
 * Type -1 is points.  They aren't like other groups.
 * Other types(groups) can be defined as needed
 * The number is an index into text->groups and point->lists.
 *
 * This is distinct from the type used by the tlist to differentiate
 * points the location of the tlist_head.  There 'GRP_MARK' is the 'group'
 * in each mark of that group, and 'GRP_LIST' is the 'lists[n]' in each
 * point.  GRP_HEAD is the head of the list.
 */

#include <unistd.h>
#include <stdlib.h>
#include <memory.h>

#define DOC_DATA_TYPE struct doc
#include "core.h"
#include "core-pane.h"
#include "internal.h"
#include "misc.h"

static MEMPOOL(mark);

static struct mark *vmark_or_point_next(struct mark *m safe, int view);
static struct mark *vmark_or_point_prev(struct mark *m safe, int view);

/* seq numbers added to the end are given a gap of 128.
 * seq numbers at other locations are placed at mean of before and after.
 * If there is no room, seq add 256 to the 'next' seq, 255 to the one
 * after etc until we find a seq already above the target, or reach
 * gap size of 64. In later case we continue with fixed gap size.
 */
static void assign_seq(struct mark *m safe)
{
	struct mark *p, *n;
	int gap = 256;

	n = mark_next(m);
	p = mark_prev(m);
	if (!n && !p) {
		/* First mark: start in the middle */
		m->seq = INT_MAX/2;
		return;
	}
	while (--gap >= 2) {
		if (n && p && n->seq - p->seq >= 2) {
			/* There is room here, so insert */
			m->seq = p->seq + (n->seq - p->seq)/2;
			return;
		}
		if (!n && p->seq < INT_MAX - 1) {
			if (INT_MAX - p->seq < gap)
				gap = INT_MAX - p->seq;
			m->seq = p->seq + gap / 2;
			return;
		}
		if (!p && n->seq > 1) {
			if (n->seq < gap)
				gap = n->seq;
			m->seq = n->seq - gap / 2;
			return;
		}

		/* We are between two marks with no room for a seq number,
		 * need to reshuffle and try to find space somewhere.
		 * Try to spread out the current marks more so there is
		 * 'gap' space between them, but let 'gap' decrease as we
		 * go to avoid pushing too hard.
		 */
		if (p && n && p->seq > INT_MAX / 2) {
			while (p && n->seq - p->seq < gap) {
				/* step back to 'p' and leave 'm' with a larger
				 * gap.
				 */
				if (n->seq > gap)
					m->seq = n->seq - gap;
				else if (n->seq > 0)
					m->seq = n->seq - 1;
				else
					m->seq = 0; /* Ouch! */
				n = m;
				m = p;
				p = mark_prev(p);
				if (gap > 4)
					gap -= 1;
			}
		} else if (p) {
			while (n && n->seq - p->seq < gap) {
				if (INT_MAX - p->seq > gap)
					m->seq = p->seq + gap;
				else if (p->seq < INT_MAX)
					m->seq = p->seq + 1;
				else
					m->seq = INT_MAX; /* Ouch! */
				p = m;
				m = n;
				n = mark_next(n);
				if (gap > 4)
					gap -= 1;
			}
		}
		/* NOTE: p->seq may now exceed n->seq, and
		 * there might be adjacent seq numbers if we hit 0 or INT_MAX.
		 * These will be fixed up on next loop through.
		 */
	}
	/* We must have scanned all the way to one end, and all the way to the
	 * other, and found no space, so 4 billion marks.  Seems unlikely.
	 */
	abort();
}

static void point_free(struct mark *p safe)
{
	unsigned int i;
	struct point_links *lnk = safe_cast p->mdata;
	for (i = 0; i < lnk->size; i++)
		tlist_del_init(&lnk->lists[i]);
	unalloc_buf(p->mdata, sizeof(*lnk) + lnk->size * sizeof(lnk->lists[0]),
		    mark);
}

void do_mark_free(struct mark *m)
{
	unalloc(m, mark);
}

static void mark_refcnt(struct mark *m safe, int inc)
{
	struct doc *d = &m->owner->doc;

	if (d->refcnt)
		d->refcnt(m, inc);
}

void mark_free(struct mark *m)
{
	struct pane *owner;

	/* mark might have already been freed by the
	 * pane getting closed.
	 */
	if (!mark_valid(m))
		return;
	if (m->viewnum == MARK_POINT)
		point_free(m);
	ASSERT(m->mdata == NULL);

	hlist_del_init(&m->all);
	if (m->viewnum != MARK_UNGROUPED)
		tlist_del_init(&m->view);
	attr_free(&m->attrs);

	mark_refcnt(m, -1);
	owner = m->owner;
	memset(m, 0xff, sizeof(*m));
	m->owner = owner;
	owner->marks -= 1;
	m->viewnum = MARK_UNGROUPED;
	editor_delayed_mark_free(m);
}

static void notify_mark_moving(struct mark *m safe, struct mark *m2)
{
	if (m->flags & MARK_FLAG_WATCHED) {
		m->flags &= ~MARK_FLAG_WATCHED;
		pane_notify("mark:moving", m->owner, 0, m);
	}

	if (m2 && (m2->flags & MARK_FLAG_WATCHED))
		pane_notify("mark:arrived", m->owner, 0, m, NULL, 0, m2);
}

void mark_watch(struct mark *m)
{
	if (m)
		m->flags |= MARK_FLAG_WATCHED;
}

static void mark_ref_copy(struct mark *to safe, struct mark *from safe)
{
	if (!mark_valid(to) || !mark_valid(from))
		return;

	if ((void*)to->owner && to->owner != from->owner) {
		LOG("mark_ref_copy given marks with different owners");
		return;
	}
	if (!(void*)to->owner) {
		from->owner->marks += 1;
		ASSERT(from->owner->marks < 20000000);
	}
	to->owner = from->owner;
	if (to->ref.p == from->ref.p &&
	    to->ref.i == from->ref.i)
		return;
	mark_refcnt(to, -1);
	to->ref = from->ref;
	mark_refcnt(to, 1);
}

static void dup_mark(struct mark *orig safe, struct mark *new safe)
{
	hlist_add_after(&orig->all, &new->all);
	INIT_TLIST_HEAD(&new->view, GRP_MARK);
	new->attrs = NULL;
	new->flags = 0;
	assign_seq(new);
	mark_ref_copy(new, orig);
}

struct mark *do_mark_at_point(struct mark *pt safe, int view)
{
	struct mark *ret;
	struct point_links *lnk;

	if (pt->viewnum != MARK_POINT)
		return NULL;
	lnk = safe_cast pt->mdata;

	alloc(ret, mark);

	dup_mark(pt, ret);
	ret->viewnum = view;
	if (view >= 0)
		tlist_add(&ret->view, GRP_MARK, &lnk->lists[view]);
	else
		INIT_TLIST_HEAD(&ret->view, GRP_MARK);
	notify_mark_moving(ret, pt);
	return ret;
}

struct mark *mark_at_point(struct pane *p safe, struct mark *pm, int view)
{
	return call_ret(mark, "doc:dup-point", p, 0, pm, NULL, view);
}

struct mark *safe point_dup(struct mark *p safe)
{
	unsigned int i;
	struct point_links *old = safe_cast p->mdata;
	struct mark *ret;
	struct point_links *lnk;

	alloc(ret, mark);
	lnk = alloc_buf(sizeof(*lnk) + old->size * sizeof(lnk->lists[0]),
			mark);

	dup_mark(p, ret);
	ret->viewnum = MARK_POINT;
	ret->mdata = lnk;
	lnk->size = old->size;
	lnk->pt = ret;
	tlist_add(&ret->view, GRP_MARK, &p->view);
	for (i = 0; lnk && i < lnk->size; i++)
		if (tlist_empty(&old->lists[i]))
			INIT_TLIST_HEAD(&lnk->lists[i], GRP_LIST);
		else
			tlist_add(&lnk->lists[i], GRP_LIST, &old->lists[i]);
	notify_mark_moving(ret, p);
	return ret;
}

void points_resize(struct doc *d safe)
{
	struct mark *p;
	tlist_for_each_entry(p, &d->points, view) {
		unsigned int i;
		struct point_links *old = safe_cast p->mdata;
		struct point_links *new = alloc_buf(sizeof(*new) +
						    d->nviews *
						    sizeof(new->lists[0]),
						    mark);
		new->pt = p;
		new->size = d->nviews;
		p->mdata = new;
		for (i = 0; i < old->size; i++) {
			tlist_add(&new->lists[i], GRP_LIST, &old->lists[i]);
			tlist_del(&old->lists[i]);
		}
		for (; i < new->size; i++)
			INIT_TLIST_HEAD(&new->lists[i], GRP_HEAD);
		free(old);
	}
}

void points_attach(struct doc *d safe, int view)
{
	struct mark *p;
	tlist_for_each_entry(p, &d->points, view) {
		struct point_links *lnk = safe_cast p->mdata;
		tlist_add_tail(&lnk->lists[view], GRP_LIST,
			       &d->views[view].head);
	}
}

struct mark *safe mark_dup(struct mark *m safe)
{
	struct mark *ret;

	if (!mark_valid(m))
		return m;

	alloc(ret, mark);
	dup_mark(m, ret);
	ret->viewnum = MARK_UNGROUPED;
	INIT_TLIST_HEAD(&ret->view, GRP_MARK);
	notify_mark_moving(ret, m);
	return ret;
}

struct mark *safe mark_dup_view(struct mark *m safe)
{
	struct mark *ret;

	if (!mark_valid(m))
		return m;

	if (m->viewnum == MARK_POINT)
		return point_dup(m);

	alloc(ret, mark);
	dup_mark(m, ret);
	if (m->viewnum == MARK_POINT) abort();
	ret->viewnum = m->viewnum;
	if (ret->viewnum == MARK_UNGROUPED)
		INIT_TLIST_HEAD(&ret->view, GRP_MARK);
	else
		tlist_add(&ret->view, GRP_MARK, &m->view);
	notify_mark_moving(ret, m);
	return ret;
}

/* if 'end', move mark after all other marks, else move before all others */
void mark_to_end(struct pane *p safe, struct mark *m safe, int end)
{
	struct doc *d = &p->doc;
	unsigned int i;
	struct point_links *lnk;

	if (!mark_valid(m))
		return;

	ASSERT(m->owner == p);
	notify_mark_moving(m, NULL);

	hlist_del(&m->all);
	if (end) {
		if (hlist_empty(&d->marks))
			hlist_add_head(&m->all, &d->marks);
		else {
			struct mark *last = hlist_first_entry(&d->marks,
							      struct mark, all);
			while (last->all.next)
				last = hlist_next_entry(last, all);
			hlist_add_after(&last->all, &m->all);
		}
	} else
		hlist_add_head(&m->all, &d->marks);
	assign_seq(m);

	if (m->viewnum == MARK_UNGROUPED)
		return;
	if (m->viewnum != MARK_POINT) {
		tlist_del(&m->view);
		if (end)
			tlist_add_tail(&m->view, GRP_MARK,
				       &d->views[m->viewnum].head);
		else
			tlist_add(&m->view, GRP_MARK,
				  &d->views[m->viewnum].head);
		return;
	}
	/* MARK_POINT */
	tlist_del(&m->view);
	if (end)
		tlist_add_tail(&m->view, GRP_MARK, &d->points);
	else
		tlist_add(&m->view, GRP_MARK, &d->points);

	lnk = safe_cast m->mdata;
	for (i = 0; d->views && i < lnk->size; i++)
		if (d->views[i].owner) {
			tlist_del(&lnk->lists[i]);
			if (end)
				tlist_add_tail(&lnk->lists[i], GRP_LIST,
					       &d->views[i].head);
			else
				tlist_add(&lnk->lists[i],
					  GRP_LIST, &d->views[i].head);
		}
}

void mark_reset(struct pane *p safe, struct mark *m safe, int end)
{
	ASSERT((void*)m->owner == NULL || m->owner == p);

	if (!mark_valid(m))
		return;

	if (!(void*)m->owner) {
		p->marks += 1;
		ASSERT(p->marks < 20000000);
	}
	m->owner = p;
	pane_call(p, "doc:set-ref", p, !end, m);
}

struct mark *mark_first(struct doc *d safe)
{
	if (!hlist_empty(&d->marks))
		return hlist_first_entry(&d->marks, struct mark, all);
	return NULL;
}

struct mark *mark_next(struct mark *m safe)
{
	if (mark_valid(m) && m->all.next)
		return hlist_next_entry(m, all);
	return NULL;
}

struct mark *mark_prev(struct mark *m safe)
{
	if (mark_valid(m) && !HLIST_IS_HEAD(m->all.pprev))
		return hlist_prev_entry(m, all);
	return NULL;
}

struct mark *doc_new_mark(struct pane *p safe, int view, struct pane *owner)
{
	/* FIXME view is >= -1 */
	struct mark *ret;
	struct doc *d = &p->doc;

	if (view >= d->nviews ||
	    view < MARK_UNGROUPED ||
	    (view >= 0 && (!d->views || d->views[view].owner != owner))) {
		/* Erroneous call, or race with document closing down */
		return NULL;
	}
	alloc(ret, mark);
	INIT_HLIST_NODE(&ret->all);
	INIT_TLIST_HEAD(&ret->view, GRP_MARK);
	ret->viewnum = view;
	hlist_add_head(&ret->all, &d->marks);

	if (view == MARK_POINT) {
		struct point_links *lnk = alloc_buf(sizeof(*lnk) +
						    d->nviews *
						    sizeof(lnk->lists[0]),
						    mark);
		int i;

		lnk->size = d->nviews;
		lnk->pt = ret;
		for (i = 0; i < d->nviews; i++)
			INIT_TLIST_HEAD(&lnk->lists[i], GRP_LIST);
		ret->mdata = lnk;
	}
	mark_reset(p, ret, 0);
	if (hlist_unhashed(&ret->all)) {
		/* Document misbehaved, fail gracefully */
		mark_free(ret);
		return NULL;
	}
	return ret;
}

/* Movement of points and marks.
 *
 * Both points and marks can move.
 * Marks can move forward or backward one character. They only
 * step over other marks if they have to.
 * To move a mark to another mark of the same group, or to a
 * point, the mark needs to be deleted and a new one created.
 * Points can step fore/back like marks.  They can jump to another
 * point easily but to move to mark they must walk one mark at a time.
 *
 * Note that 'm' isn't 'safe' - A NULL mark will be step to point
 * by the doc redirector.
 */

wint_t do_doc_step(struct pane *p safe, struct mark *m,
		   int forward, int move)
{
	int ret;
	static int dodebug = -1;
	static int count;

	if (dodebug < 0)
		dodebug = atoi(getenv("EDLIB_DEBUG_MARKS")?:"10000");

	if (m && dodebug && count++ >= dodebug) {
		count = 0;
		doc_check_consistent(&m->owner->doc);
	}

	if (move)
		ret = call("doc:char", p, forward ? 1 : -1, m);
	else
		ret = call("doc:char", p, 0, m, NULL, forward ? 1 : -1);
	if (ret <= 0)
		return WEOF;
	if ((unsigned)ret >= CHAR_RET(WEOF))
		return WEOF;
	else
		return ret & 0x1fffff;
}

/* Move the point so it is at the same location as the mark, both in the
 * text.
 * Firstly find the point closest to the mark, though that will often
 * be the point we already have.
 * Then for each mark group we find the last that is before the target,
 * and move the point to there.
 * Then update 'all' list, text ref and seq number.
 */

static void point_forward_to_mark(struct mark *p safe, struct mark *m safe)
{
	struct mark *ptmp, *pnear;
	unsigned int i;
	struct point_links *plnk = safe_cast p->mdata;

	pnear = p;
	ptmp = p;
	tlist_for_each_entry_continue(ptmp, GRP_HEAD, view) {
		if (ptmp->seq <= m->seq)
			pnear = ptmp;
		else
			break;
	}
	/* pnear is the nearest point to m that is before m. So
	 * move p after pnear in the point list. */
	if (p != pnear) {
		tlist_del(&p->view);
		tlist_add(&p->view, GRP_MARK, &pnear->view);
	}

	/* Now move 'p' in the various mark lists */
	for (i = 0; i < plnk->size; i++) {
		struct mark *mnear = NULL;
		struct tlist_head *tl;
		struct point_links *pnlnk = safe_cast pnear->mdata;

		tl = &pnlnk->lists[i];
		if (tlist_empty(tl))
			continue;
		tlist_for_each_continue(tl,  GRP_HEAD) {
			struct mark *mtmp;
			if (TLIST_TYPE(tl) != GRP_MARK)
				break;
			mtmp = container_of(tl, struct mark, view);
			if (mtmp->seq <= m->seq)
				mnear = mtmp;
			else
				break;
		}
		if (mnear) {
			tlist_del(&plnk->lists[i]);
			tlist_add(&plnk->lists[i], GRP_LIST, &mnear->view);
		} else if (p != pnear) {
			tlist_del(&plnk->lists[i]);
			tlist_add(&plnk->lists[i], GRP_LIST, &pnlnk->lists[i]);
		}
	}
	/* finally move in the overall list */
	hlist_del(&p->all);
	hlist_add_after(&m->all, &p->all);
	assign_seq(p);
}

static void point_backward_to_mark(struct mark *p safe, struct mark *m safe)
{
	struct mark *ptmp, *pnear;
	unsigned int i;
	struct point_links *plnk = safe_cast p->mdata;

	pnear = p;
	ptmp = p;
	tlist_for_each_entry_continue_reverse(ptmp, GRP_HEAD, view) {
		if (ptmp->seq >= m->seq)
			pnear = ptmp;
		else
			break;
	}
	/* pnear is the nearest point to m that is after m. So
	 * move p before pnear in the point list */
	if (p != pnear) {
		tlist_del(&p->view);
		tlist_add_tail(&p->view, GRP_MARK, &pnear->view);
	}

	/* Now move 'p' in the various mark lists */
	for (i = 0; i < plnk->size; i++) {
		struct mark *mnear = NULL;
		struct tlist_head *tl;
		struct point_links *pnlnk = safe_cast pnear->mdata;

		tl = &pnlnk->lists[i];
		if (tlist_empty(tl))
			continue;
		tlist_for_each_continue_reverse(tl, GRP_HEAD) {
			struct mark *mtmp;
			if (TLIST_TYPE(tl) != GRP_MARK)
				break;
			mtmp = container_of(tl, struct mark, view);
			if (mtmp->seq >= m->seq)
				mnear = mtmp;
			else
				break;
		}
		if (mnear) {
			tlist_del(&plnk->lists[i]);
			tlist_add_tail(&plnk->lists[i], GRP_LIST, &mnear->view);
		} else if (p != pnear) {
			tlist_del(&plnk->lists[i]);
			tlist_add_tail(&plnk->lists[i], GRP_LIST,
				       &pnlnk->lists[i]);
		}
	}
	/* finally move in the overall list */
	hlist_del(&p->all);
	hlist_add_before(&p->all, &m->all);
	assign_seq(p);
}

static void _mark_to_mark_noref(struct mark *m safe, struct mark *target safe)
{
	/* DEBUG: Make sure they are on same list */
	struct mark *a = m, *b = target;

	if (!mark_valid(m) || !mark_valid(target))
		return;
	if (a->owner != b->owner)
		LOG("mark_to_mark: marks have different owners");
	else {
		if (m->seq > target->seq) {
			a = target; b = m;
		}
		while (a && a != b)
			a = mark_next(a);
		if (a != b)
			LOG("mark_to_mark with marks on different list - same owner");
	}
	if (a != b) {
		call("editor:notify:Message:broadcast",
		     m->owner, 0, NULL,
		     "WARNING marks inconsistent - see log");
		return;
	}
	/* END DEBUG */

	if (m == target)
		return;
	if (!mark_same(m, target))
		notify_mark_moving(m, target);
	if (m->viewnum == MARK_POINT) {
		/* Lots of linkage to fix up */
		if (m->seq < target->seq)
			point_forward_to_mark(m, target);
		else if (m->seq > target->seq)
			point_backward_to_mark(m, target);
		return;
	}
	if (m->viewnum == MARK_UNGROUPED) {
		/* Only one linked list to worry about */
		if (m->seq < target->seq) {
			hlist_del(&m->all);
			hlist_add_after(&target->all, &m->all);
			assign_seq(m);
		} else {
			hlist_del(&m->all);
			hlist_add_before(&m->all, &target->all);
			m->seq = target->seq;
			assign_seq(target);
		}
		return;
	}
	if (m->viewnum == target->viewnum) {
		/* Same view, both on the same 2 lists */
		if (m->seq < target->seq) {
			hlist_del(&m->all);
			hlist_add_after(&target->all, &m->all);
			tlist_del(&m->view);
			tlist_add(&m->view, GRP_MARK, &target->view);
			assign_seq(m);
		} else {
			hlist_del(&m->all);
			hlist_add_before(&m->all, &target->all);
			tlist_del(&m->view);
			tlist_add_tail(&m->view, GRP_MARK, &target->view);
			m->seq = target->seq;
			assign_seq(target);
		}
		return;
	}
	if (target->viewnum == MARK_POINT) {
		/* A vmark and a point, both on the only 2 lists
		 * that need changing */
		struct point_links *lnks = safe_cast target->mdata;
		if (m->seq < target->seq) {
			hlist_del(&m->all);
			hlist_add_after(&target->all, &m->all);
			tlist_del(&m->view);
			tlist_add(&m->view, GRP_MARK, &lnks->lists[m->viewnum]);
			assign_seq(m);
		} else {
			hlist_del(&m->all);
			hlist_add_before(&m->all, &target->all);
			tlist_del(&m->view);
			tlist_add_tail(&m->view, GRP_MARK,
				       &lnks->lists[m->viewnum]);
			m->seq = target->seq;
			assign_seq(target);
		}
		return;
	}
	/* Hard case: We have a vmark and a mark which isn't on the same list.
	 * We need to find a matching vmark 'close' to the destination and link
	 * after that.
	 */
	if (m->seq < target->seq) {
		struct mark *m1 = m, *n;
		while ((n = vmark_or_point_next(m1, m->viewnum)) != NULL &&
		       n->seq <= target->seq)
			m1 = n;
		if (m1 != m) {
			tlist_del(&m->view);
			if (m1->viewnum == MARK_POINT) {
				struct point_links *lnks = safe_cast m1->mdata;
				tlist_add(&m->view, GRP_MARK,
					  &lnks->lists[m->viewnum]);
			} else
				tlist_add(&m->view, GRP_MARK, &m1->view);
		}
		hlist_del(&m->all);
		hlist_add_after(&target->all, &m->all);
		assign_seq(m);
	} else {
		struct mark *m1 = m, *n;
		while ((n = vmark_or_point_prev(m1, m->viewnum)) != NULL &&
		       n->seq >= target->seq)
			m1 = n;
		if (m1 != m) {
			tlist_del(&m->view);
			if (m1->viewnum == MARK_POINT) {
				struct point_links *lnks = safe_cast m1->mdata;
				tlist_add_tail(&m->view, GRP_MARK,
					       &lnks->lists[m->viewnum]);
			} else
				tlist_add_tail(&m->view, GRP_MARK, &m1->view);
		}
		hlist_del(&m->all);
		hlist_add_before(&m->all, &target->all);
		m->seq = target->seq;
		assign_seq(target);
	}
}

void mark_to_mark_noref(struct mark *m safe, struct mark *target safe)
{
	struct mark *m2;
	int err = 0;
	static bool warned = False;

	_mark_to_mark_noref(m, target);
	m2 = mark_prev(m);
	if (m2 && !mark_same(m2, m) &&
	    pane_call(m->owner, "debug:validate-marks", m->owner,
		      0, m2, NULL, 0, m) < 0)
		err = 1;
	m2 = mark_next(m);
	if (m2 && !mark_same(m, m2) &&
	    pane_call(m->owner, "debug:validate-marks", m->owner,
		      0, m, NULL, 0, m2) < 0)
		err = 1;

	if (err && !warned) {
		LOG_BT();
		call("editor:notify:Message:broadcast",
		     m->owner, 0, NULL,
		     "WARNING: marks inconsistent in mark_to_mark, check log");
		warned = True;
	}
}

void mark_to_mark(struct mark *m safe, struct mark *target safe)
{
	_mark_to_mark_noref(m, target);
	mark_ref_copy(m, target);
}

void mark_step(struct mark *m safe, int forward)
{
	/* step mark forward, or backward, over all marks with same
	 * ref.
	 */
	struct mark *m2, *target = m;

	/* This is called after .ref has been updated, so we can
	 * assume the point really is moving.
	 */
	notify_mark_moving(m, NULL);

	if (forward) {
		for (m2 = mark_next(m);
		     m2 && mark_same(m, m2);
		     m2 = mark_next(m2))
			target = m2;
	} else {
		for (m2 = mark_prev(m);
		     m2 && mark_same(m, m2);
		     m2 = mark_prev(m2))
			target = m2;
	}
	mark_to_mark_noref(m, target);
}

void mark_step_sharesref(struct mark *m safe, int forward)
{
	/* step mark forward, or backward, over all marks with same
	 * ref, ignoring the .i, and sets '.i' to a safe value wrt the
	 * last mark stepped over.
	 */
	struct mark *m2, *target = m;

	/* This is called after .ref has been updated, so we can
	 * assume the point really is moving.
	 */
	notify_mark_moving(m, NULL);

	if (forward) {
		for (m2 = mark_next(m);
		     m2 && m->ref.p == m2->ref.p;
		     m2 = mark_next(m2))
			target = m2;
	} else {
		for (m2 = mark_prev(m);
		     m2 && m->ref.p == m2->ref.p;
		     m2 = mark_prev(m2))
			target = m2;
	}
	m->ref.i = target->ref.i;
	mark_to_mark_noref(m, target);
}

/* A 'vmark' is a mark in a particular view.  We can walk around those
 * silently skipping over the points.
 */

static struct mark *_vmark_next(struct tlist_head *tl safe)
{
	while (TLIST_TYPE(tl) != GRP_HEAD) {
		if (TLIST_TYPE(tl) == GRP_LIST) {
			tl = TLIST_PTR(tl->next);
			continue;
		}
		return container_of(tl, struct mark, view);
	}
	return NULL;
}

struct mark *vmark_next(struct mark *m safe)
{
	struct tlist_head *tl;

	if (!mark_valid(m))
		return NULL;

	tl = TLIST_PTR(m->view.next);
	return _vmark_next(tl);
}

static struct mark *vmark_or_point_next(struct mark *m safe, int view)
{
	struct tlist_head *tl;
	struct point_links *lnk;

	if (m->viewnum == view)
		tl = TLIST_PTR(m->view.next);
	else if (m->viewnum == MARK_POINT) {
		lnk = safe_cast m->mdata;
		tl = TLIST_PTR(lnk->lists[view].next);
	} else
		return NULL;
	switch(TLIST_TYPE(tl)) {
	default:
	case GRP_HEAD:
		return NULL;
	case GRP_MARK:
		return container_of(tl, struct mark, view);
	case GRP_LIST:
		lnk = container_of_array(tl, struct point_links, lists, view);
		return lnk->pt;
	}
}

static struct mark *_vmark_prev(struct tlist_head *tl safe)
{
	while (TLIST_TYPE(tl) != GRP_HEAD) {
		if (TLIST_TYPE(tl) == GRP_LIST) {
			tl = TLIST_PTR(tl->prev);
			continue;
		}
		return container_of(tl, struct mark, view);
	}
	return NULL;
}

struct mark *vmark_prev(struct mark *m safe)
{
	struct tlist_head *tl;

	if (!mark_valid(m))
		return NULL;

	tl = TLIST_PTR(m->view.prev);
	return _vmark_prev(tl);
}

static struct mark *vmark_or_point_prev(struct mark *m safe, int view)
{
	struct tlist_head *tl;
	struct point_links *lnk;

	if (m->viewnum == view)
		tl = TLIST_PTR(m->view.prev);
	else if (m->viewnum == MARK_POINT) {
		lnk = safe_cast m->mdata;
		tl = TLIST_PTR(lnk->lists[view].prev);
	} else
		return NULL;
	switch(TLIST_TYPE(tl)) {
	default:
	case GRP_HEAD:
		return NULL;
	case GRP_MARK:
		return container_of(tl, struct mark, view);
	case GRP_LIST:
		lnk = container_of_array(tl, struct point_links, lists, view);
		return lnk->pt;
	}
}

struct mark *do_vmark_first(struct doc *d safe, int view,
			    struct pane *owner safe)
{
	struct tlist_head *tl;

	if (view < 0 || view >= d->nviews || d->views == NULL)
		return NULL;
	if (d->views[view].owner != owner)
		return NULL;

	tl = TLIST_PTR(d->views[view].head.next);
	while (TLIST_TYPE(tl) != GRP_HEAD) {
		if (TLIST_TYPE(tl) == GRP_LIST) {
			tl = TLIST_PTR(tl->next);
			continue;
		}
		return container_of(tl, struct mark, view);
	}
	return NULL;
}

struct mark *do_vmark_last(struct doc *d safe, int view,
			   struct pane *owner safe)
{
	struct tlist_head *tl;

	if (view < 0 || view >= d->nviews || d->views == NULL)
		return NULL;
	if (d->views[view].owner != owner)
		return NULL;

	tl = TLIST_PTR(d->views[view].head.prev);
	while (TLIST_TYPE(tl) != GRP_HEAD) {
		if (TLIST_TYPE(tl) == GRP_LIST) {
			tl = TLIST_PTR(tl->prev);
			continue;
		}
		return container_of(tl, struct mark, view);
	}
	return NULL;
}

struct mark *vmark_first(struct pane *p safe, int view, struct pane *owner safe)
{
	return home_call_ret(mark, p, "doc:vmark-get", owner, view);
}

struct mark *vmark_last(struct pane *p safe, int view, struct pane *owner safe)
{
	return home_call_ret(mark2, p, "doc:vmark-get", owner, view);
}

struct mark *vmark_at_or_before(struct pane *p safe, struct mark *m safe,
				int view, struct pane *owner)
{
	return home_call_ret(mark, p, "doc:vmark-prev", owner?:p,
			     view, m);
}

struct mark *vmark_new(struct pane *p safe, int view, struct pane *owner)
{
	return home_call_ret(mark, p, "doc:vmark-new", owner?:p, view);
}

struct mark *vmark_matching(struct mark *m safe)
{
	/* Find a nearby mark in the same view with the same ref */
	struct mark *m2;

	if (!mark_valid(m))
		return NULL;

	m2 = vmark_prev(m);
	if (m2 && mark_same(m, m2))
		return m2;
	m2 = vmark_next(m);
	if (m2 && mark_same(m, m2))
		return m2;
	return NULL;
}

struct mark *do_vmark_at_or_before(struct doc *d safe,
				   struct mark *m safe,
				   int view, struct pane *owner)
{
	/* Find the last 'view' mark that is not later in the document than 'm'.
	 * It might be later in the mark list, but not in the document.
	 * Return NULL if all 'view' marks are after 'm' in the document.
	 */
	struct mark *vm = m;

	if (!mark_valid(m))
		return NULL;

	if (&m->owner->doc != d) {
		LOG("vmark_at_or_before called with incorrect mark");
		return NULL;
	}

	if ((view < 0 && view != MARK_POINT) || view >= d->nviews ||
	    d->views == NULL || d->views[view].owner != owner)
		return NULL;

	/* might need to hunt along 'all' list for something suitable */
	while (vm && vm->viewnum != MARK_POINT && vm->viewnum != view)
		vm = mark_next(vm);
	if (!vm) {
		vm = m;
		while (vm && vm->viewnum != MARK_POINT && vm->viewnum != view)
			vm = mark_prev(vm);
	}
	if (!vm)
		/* No 'view' marks at all! */
		return vm;
	/* 'vm' is either a point or a 'view' mark.  It is probably after 'm',
	 * but if it is before, then no 'view' mark is after.
	 */
	if (vm->viewnum == MARK_POINT) {
		struct point_links *lnk = safe_cast vm->mdata;
		struct tlist_head *tl = &lnk->lists[view];
		vm = _vmark_next(tl);
		if (!vm)
			vm = _vmark_prev(tl);
		else if (mark_same(vm, m)) {
			/* maybe there are even more */
			struct mark *vm2;
			while ((vm2 = vmark_next(vm)) != NULL &&
			       mark_same(vm, vm2))
				vm = vm2;
		}
	} else if (vm->viewnum == view) {
		/* Just use this, or nearby */
		struct mark *vm2;
		while ((vm2 = vmark_next(vm)) != NULL &&
		       mark_same(vm, m))
			vm = vm2;
	}
	while (vm && !mark_ordered_or_same(vm, m))
		vm = vmark_prev(vm);

	return vm;
}

void mark_clip(struct mark *m safe, struct mark *start, struct mark *end,
	       bool tostart)
{
	/* Note: if this is called while looping over a list of marks,
	 * the loop should move forward when tostart,
	 * and backward when !tostart.
	 */
	if (!mark_valid(m) || !mark_valid(start) || !mark_valid(end))
		return;
	if (!mark_ordered_or_same(start, m))
		return;
	if (!mark_ordered_not_same(m, end))
		return;

	if (tostart)
		mark_to_mark(m, start);
	else
		mark_to_mark(m, end);
}

void marks_clip(struct pane *p safe, struct mark *start, struct mark *end,
		int view, struct pane *owner, bool tostart)
{
	struct mark *m, *m2;

	if (!mark_valid(start) || !mark_valid(end))
		return;

	if (tostart) {
		m = vmark_at_or_before(p, start, view, owner);
		while (m && (m2=vmark_prev(m)) && mark_same(start, m2))
			m = m2;
		while (m && !mark_ordered_or_same(start, m))
			m = vmark_next(m);

		while (m && !mark_ordered_or_same(end, m)) {
			m2 = vmark_next(m);
			mark_clip(m, start, end, tostart);
			m = m2;
		}
	} else {
		m = vmark_at_or_before(p, end, view, owner);
		while (m && (m2=vmark_next(m)) && mark_same(end,m2))
			m = m2;
		while (m && !mark_ordered_not_same(m, end))
			m = vmark_prev(m);

		while (m && !mark_ordered_not_same(m, start)) {
			m2 = vmark_prev(m);
			mark_clip(m, start, end, tostart);
			m = m2;
		}
	}
}

void doc_check_consistent(struct doc *d safe)
{
	/* Check consistency of marks, and abort if not.
	 * Check:
	 * - all marks are in seq order
	 * - all view lists are in seq order
	 */
	struct mark *m, *m2 = NULL;
	int seq = 0;
	int i;
	int max = 1000;
	static bool warned = False;

	if (pane_no_consistency(safe_cast container_of(d, struct pane, doc)))
		return;

	hlist_for_each_entry(m, &d->marks, all) {
		ASSERT(m->seq >= seq);
		ASSERT(&m->owner->doc == d);
		seq = m->seq + 1;
		if (m2 && !marks_validate(m2, m) && !warned) {
			LOG_BT();
			call("editor:notify:Message:broadcast",
			     m->owner, 0, NULL,
			     "WARNING: mark inconsistency, check log");
			warned = True;
		}
		m2 = m;
		if (max-- < 0)
			break;
	}
	for (i = 0; d->views && i < d->nviews; i++)
		if (d->views[i].owner == NULL) {
			if (!tlist_empty(&d->views[i].head)) abort();
		} else {
			struct tlist_head *tl;
			struct point_links *pl;
			seq = 0;
			tlist_for_each(tl, &d->views[i].head) {
				switch(TLIST_TYPE(tl)) {
				case GRP_HEAD: abort();
				case GRP_MARK:
					m = container_of(tl, struct mark, view);
					break;
				case GRP_LIST:
					pl = container_of_array(
						tl, struct point_links,
						lists, i);
					m = pl->pt;
					break;
				default: abort();
				}
				if (m->seq < seq)
					abort();
				seq = m->seq + 1;
				if (max-- < 0)
					break;
			}
		}
}

bool marks_validate(struct mark *m1 safe, struct mark *m2 safe)
{
	struct mark *m;
	struct doc *d = &m1->owner->doc;
	int found = 0;
	int ret;
	int max = 1000;

	if (m1 == m2) {
		for (m = mark_first(d); m; m = mark_next(m)) {
			if (m1 == m)
				return True;
			if (max-- < 0)
				break;
		}
		LOG("marks_validate: marks not found");
		return False;
	}
	if (m1->seq >= m2->seq) {
		m = m1; m1 = m2; m2 = m;
	}

	for (m = mark_first(d); m; m = mark_next(m)) {
		if (m == m1 || m == m2)
			found += 1;
		if (max-- < 0)
			break;
	}
	if (found != 2) {
		LOG("log_val_marks not both found, only %d", found);
		return False;
	}
	if (mark_same(m1, m2))
		return True;
	ret = pane_call(m1->owner, "debug:validate-marks", m1->owner,
			0, m1, NULL, 0, m2);
	return (ret > 0 || ret == Efallthrough);
}
