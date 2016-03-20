/*
 * Copyright Neil Brown ©2015 <neil@brown.name>
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
 * found quickly.  A cost is that a point will be reallocated when
 * a new list is created.  So stable refs to a point are not possible.
 * Each point has one reference from a window/pane (where it is the cursor)
 * or some other object (for background updates) and that can be found
 * through the ->owner link.
 * Each point knows which document it points to - a mark doesn't.
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

#include "core.h"

/* seq numbers added to the end are given a gap of 128.
 * seq numbers at other locations are placed at mean of before and after.
 * If there is no room, seq add 256 to the 'next' seq, 255 to the one
 * after etc until we find a seq already above the target, or reach
 * gap size of 64. In later case we continue with fixed gap size.
 */
static void assign_seq(struct mark *m, int prev)
{
	int gap = 256;

	while (m->all.next) {
		struct mark *mn = hlist_next_entry(m, all);
		if (prev+1 < mn->seq) {
			m->seq = (prev + mn->seq) / 2;
			return;
		}
		/* Doesn't fit, make a gap */
		m->seq = prev+gap;
		if (gap > 64)
			gap -= 1;
		prev = m->seq;
		m = mn;
	}
	/* We've come to the end */
	m->seq = prev + 128;
	ASSERT(m->seq >= 0);
}

static void mark_delete(struct mark *m)
{
	hlist_del_init(&m->all);
	if (m->viewnum != MARK_UNGROUPED)
		tlist_del_init(&m->view);
	attr_free(&m->attrs);
}

static void point_free(struct mark *p)
{
	int i;
	struct point_links *lnk = p->mdata;
	for (i = 0; i < lnk->size; i++)
		tlist_del_init(&lnk->lists[i]);
	free(lnk);
	p->mdata = NULL;
}

void mark_free(struct mark *m)
{
	if (!m)
		return;
	if (m->viewnum == MARK_POINT)
		point_free(m);
	ASSERT(m->mdata == NULL);
	mark_delete(m);
	free(m);
}

static void dup_mark(struct mark *orig, struct mark *new)
{
	new->ref = orig->ref;
	new->rpos = orig->rpos;
	new->attrs= NULL;
	hlist_add_after(&orig->all, &new->all);
	assign_seq(new, orig->seq);
}

struct mark *do_mark_at_point(struct mark *pt, int view)
{
	struct mark *ret;
	struct point_links *lnk;

	if (pt->viewnum != MARK_POINT)
		return NULL;
	lnk = pt->mdata;

	ret = calloc(sizeof(*ret), 1);

	dup_mark(pt, ret);
	ret->viewnum = view;
	if (view >= 0)
		tlist_add(&ret->view, GRP_MARK, &lnk->lists[view]);
	else
		INIT_TLIST_HEAD(&ret->view, GRP_MARK);
	return ret;
}

DEF_CMD(dup_point_callback)
{
	struct call_return *cr = container_of(ci->comm, struct call_return, c);
	cr->m = ci->mark;
	return 1;
}

struct mark *mark_at_point(struct pane *p, struct mark *pm, int view)
{
	struct call_return cr;
	int ret;

	cr.c = dup_point_callback;
	cr.m = NULL;
	ret = call_comm("doc:dup-point", p, 0, pm, NULL, view, &cr.c);
	if (ret <= 0)
		return NULL;
	return cr.m;
}

struct mark *point_dup(struct mark *p)
{
	int i;
	struct point_links *old = p->mdata;
	struct mark *ret = malloc(sizeof(*ret));
	struct point_links *lnk = malloc(sizeof(*lnk) +
					 old->size * sizeof(lnk->lists[0]));

	dup_mark(p, ret);
	ret->viewnum = MARK_POINT;
	ret->mdata = lnk;
	ret->mtype = NULL;
	lnk->size = old->size;
	lnk->pt = ret;
	tlist_add(&ret->view, GRP_MARK, &p->view);
	for (i = 0; i < lnk->size; i++)
		if (tlist_empty(&old->lists[i]))
			INIT_TLIST_HEAD(&lnk->lists[i], GRP_LIST);
		else
			tlist_add(&lnk->lists[i], GRP_LIST, &old->lists[i]);
	return ret;
}

void points_resize(struct doc *d)
{
	struct mark *p;
	tlist_for_each_entry(p, &d->points, view) {
		int i;
		struct point_links *old = p->mdata;
		struct point_links *new = malloc(sizeof(*new) +
						 d->nviews * sizeof(new->lists[0]));
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

void points_attach(struct doc *d, int view)
{
	struct mark *p;
	tlist_for_each_entry(p, &d->points, view) {
		struct point_links *lnk = p->mdata;
		tlist_add_tail(&lnk->lists[view], GRP_LIST, &d->views[view].head);
	}
}

struct mark *mark_dup(struct mark *m, int notype)
{
	struct mark *ret;

	if (!notype && m->viewnum == MARK_POINT)
			return NULL;

	ret = calloc(sizeof(*ret), 1);
	dup_mark(m, ret);
	if (notype) {
		ret->viewnum = MARK_UNGROUPED;
		INIT_TLIST_HEAD(&ret->view, GRP_MARK);
	} else {
		if (m->viewnum == MARK_POINT) abort();
		ret->viewnum = m->viewnum;
		if (ret->viewnum == MARK_UNGROUPED)
			INIT_TLIST_HEAD(&ret->view, GRP_MARK);
		else
			tlist_add(&ret->view, GRP_MARK, &m->view);
	}
	return ret;
}

void __mark_reset(struct doc *d, struct mark *m, int new, int end)
{
	int i;
	int seq = 0;
	struct point_links *lnk;

	m->rpos = 0;
	if (!new)
		hlist_del(&m->all);
	if (end) {
		if (hlist_empty(&d->marks))
			hlist_add_head(&m->all, &d->marks);
		else {
			struct mark *last = hlist_first_entry(&d->marks,
							      struct mark, all);
			while (last->all.next)
				last = hlist_next_entry(last, all);
			seq = last->seq;
			hlist_add_after(&last->all, &m->all);
		}
	} else
		hlist_add_head(&m->all, &d->marks);
	assign_seq(m, seq);

	call3("doc:set-ref", d->home, !end, m);

	if (m->viewnum == MARK_UNGROUPED)
		return;
	if (m->viewnum != MARK_POINT) {
		if (!new)
			tlist_del(&m->view);
		if (end)
			tlist_add_tail(&m->view, GRP_MARK, &d->views[m->viewnum].head);
		else
			tlist_add(&m->view, GRP_MARK, &d->views[m->viewnum].head);
		return;
	}
	/* MARK_POINT */
	if (!new)
		tlist_del(&m->view);
	if (end)
		tlist_add_tail(&m->view, GRP_MARK, &d->points);
	else
		tlist_add(&m->view, GRP_MARK, &d->points);

	lnk = m->mdata;
	for (i = 0; i < lnk->size; i++)
		if (d->views[i].state) {
			if (!new)
				tlist_del(&lnk->lists[i]);
			if (end)
				tlist_add_tail(&lnk->lists[i], GRP_LIST,
					       &d->views[i].head);
			else
				tlist_add(&lnk->lists[i], GRP_LIST, &d->views[i].head);
		} else if (new)
			INIT_TLIST_HEAD(&lnk->lists[i], GRP_LIST);
}

struct mark *point_new(struct doc *d)
{
	struct mark *ret = malloc(sizeof(*ret));
	struct point_links *lnk = malloc(sizeof(*lnk) +
					 d->nviews * sizeof(lnk->lists[0]));

	ret->attrs = NULL;
	ret->viewnum = MARK_POINT;
	ret->mdata = lnk;
	ret->mtype = NULL;
	lnk->size = d->nviews;
	lnk->pt = ret;
	__mark_reset(d, ret, 1, 0);
	return ret;
}

void mark_reset(struct doc *d, struct mark *m)
{
	__mark_reset(d, m, 0, 0);
}

struct mark *doc_next_mark_view(struct mark *m)
{
	struct tlist_head *tl = &m->view;

	tlist_for_each_continue(tl, GRP_HEAD)
		if (TLIST_TYPE(tl) == GRP_MARK)
			return tlist_entry(tl, struct mark, view);
	return NULL;
}

struct mark *doc_prev_mark_view(struct mark *m)
{
	struct tlist_head *tl = &m->view;

	tlist_for_each_continue_reverse(tl, GRP_HEAD)
		if (TLIST_TYPE(tl) == GRP_MARK)
			return tlist_entry(tl, struct mark, view);
	return NULL;
}

struct mark *doc_first_mark_all(struct doc *d)
{
	if (!hlist_empty(&d->marks))
		return hlist_first_entry(&d->marks, struct mark, all);
	return NULL;
}

struct mark *doc_next_mark_all(struct mark *m)
{
	if (m->all.next)
		return hlist_next_entry(m, all);
	return NULL;
}

struct mark *doc_prev_mark_all(struct mark *m)
{
	if (!HLIST_IS_HEAD(m->all.pprev))
		return hlist_prev_entry(m, all);
	return NULL;
}

struct mark *doc_new_mark(struct doc *d, int view)
{
	struct mark *ret;

	if (view == MARK_POINT || view >= d->nviews || d->views[view].state != 1)
		return NULL;
	ret = calloc(sizeof(*ret), 1);
	ret->viewnum = view;
	__mark_reset(d, ret, 1, 0);
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
 */

static void swap_lists(struct mark *p1, struct mark *p2)
{
	struct point_links *tmp;
	tmp = p1->mdata;
	p1->mdata = p2->mdata;
	p2->mdata = tmp;
	tmp->pt = p2;
	tmp = p1->mdata;
	tmp->pt = p1;
}

void mark_forward_over(struct mark *m, struct mark *m2)
{
	int seq;

	hlist_del(&m->all);
	hlist_add_after(&m2->all, &m->all);
	if (m->viewnum == m2->viewnum && m->viewnum != MARK_UNGROUPED) {
		tlist_del(&m->view);
		tlist_add(&m->view, GRP_MARK, &m2->view);
	}
	if (m->viewnum == MARK_POINT && m2->viewnum == MARK_POINT) {
		/* moving a point over a point */
		swap_lists(m, m2);
	} else if (m->viewnum == MARK_POINT) {
		/* Moving a point over a mark */
		struct point_links *lnk = m->mdata;
		if (m2->viewnum >= 0) {
			tlist_del(&m2->view);
			tlist_add_tail(&m2->view, GRP_MARK, &lnk->lists[m2->viewnum]);
		}
	} else if (m2->viewnum == MARK_POINT) {
		/* stepping a mark over a point */
		struct point_links *lnk = m2->mdata;
		if (m->viewnum >= 0) {
			tlist_del(&m->view);
			tlist_add(&m->view, GRP_MARK, &lnk->lists[m->viewnum]);
		}
	}
	seq = m->seq;
	m->seq = m2->seq;
	m2->seq = seq;

	m->ref = m2->ref;
}

void mark_backward_over(struct mark *m, struct mark *mp)
{
	int seq;

	hlist_del(&m->all);
	hlist_add_before(&m->all, &mp->all);
	if (m->viewnum == mp->viewnum && m->viewnum != MARK_UNGROUPED) {
		tlist_del(&m->view);
		tlist_add_tail(&m->view, GRP_MARK, &mp->view);
	}
	if (m->viewnum == MARK_POINT && mp->viewnum == MARK_POINT) {
		/* moving a point over a point */
		swap_lists(m, mp);
	} else if (m->viewnum == MARK_POINT) {
		/* Moving a point over a mark */
		struct point_links *lnks = m->mdata;
		if (mp->viewnum >= 0) {
			tlist_del(&mp->view);
			tlist_add(&mp->view, GRP_MARK, &lnks->lists[mp->viewnum]);
		}
	} else if (mp->viewnum == MARK_POINT) {
		/* Step back over a point */
		struct point_links *lnks = mp->mdata;
		if (m->viewnum >= 0) {
			tlist_del(&m->view);
			tlist_add_tail(&m->view, GRP_MARK, &lnks->lists[m->viewnum]);
		}
	}
	seq = m->seq;
	m->seq = mp->seq;
	mp->seq = seq;

	m->ref = mp->ref;
}

wint_t mark_step(struct doc *d, struct mark *m, int forward, int move, struct cmd_info *ci)
{
	int ret = call5("doc:step", d->home, forward, m, NULL, move);

	if (ret <= 0)
		return ret;
	if (ret >= 0x1fffff)
		return WEOF;
	else
		return ret & 0xfffff;
}

wint_t mark_step_pane(struct pane *p, struct mark *m, int forward, int move, struct cmd_info *ci)
{
	int ret = call5("doc:step", p, forward, m, NULL, move);

	if (ret <= 0)
		return ret;
	if (ret >= 0x1fffff)
		return WEOF;
	else
		return ret & 0xfffff;
}

wint_t mark_step2(struct doc *d, struct mark *m, int forward, int move)
{
	struct cmd_info ci = {0};

	return mark_step(d, m, forward, move, &ci);
}

wint_t mark_next(struct doc *d, struct mark *m)
{
	wint_t ret;
	struct mark *m2 = NULL;

	while ((m2 = doc_next_mark_all(m)) != NULL &&
	       mark_same(d, m, m2))
		mark_forward_over(m, m2);

	ret = mark_step2(d, m, 1, 1);
	if (ret == WEOF)
		return ret;

/* FIXME do I need to do this - is it precise enough? */
	while ((m2 = doc_next_mark_all(m)) != NULL &&
	       mark_same(d, m, m2))
		mark_forward_over(m, m2);
	return ret;
}

wint_t mark_next_pane(struct pane *p, struct mark *m)
{
	wint_t ret;
	struct mark *m2 = NULL;
	struct cmd_info same_ci = {0};

	while ((m2 = doc_next_mark_all(m)) != NULL &&
	       mark_same_pane(p, m, m2, &same_ci))
		mark_forward_over(m, m2);

	ret = mark_step_pane(p, m, 1, 1, NULL);
	if (ret == WEOF)
		return ret;

/* FIXME do I need to do this - is it precise enough? */
	while ((m2 = doc_next_mark_all(m)) != NULL &&
	       mark_same_pane(p, m, m2, &same_ci))
		mark_forward_over(m, m2);
	return ret;
}

wint_t mark_prev(struct doc *d, struct mark *m)
{
	wint_t ret;
	struct mark *mp = NULL;

	while ((mp = doc_prev_mark_all(m)) != NULL &&
	       mark_same(d, m, mp))
		mark_backward_over(m, mp);

	ret = mark_step2(d, m, 0, 1);
	if (ret == WEOF)
		return ret;
	while ((mp = doc_prev_mark_all(m)) != NULL &&
	       mark_same(d, m, mp))
		mark_backward_over(m, mp);
	return ret;
}

wint_t mark_prev_pane(struct pane *p, struct mark *m)
{
	wint_t ret;
	struct mark *mp = NULL;
	struct cmd_info same_ci = {0};

	while ((mp = doc_prev_mark_all(m)) != NULL &&
	       mark_same_pane(p, m, mp, &same_ci))
		mark_backward_over(m, mp);

	ret = mark_step_pane(p, m, 0, 1, NULL);
	if (ret == WEOF)
		return ret;
	while ((mp = doc_prev_mark_all(m)) != NULL &&
	       mark_same_pane(p, m, mp, &same_ci))
		mark_backward_over(m, mp);
	return ret;
}

/* Move the point so it is at the same location as the mark, both in the
 * text.
 * Firstly find the point closest to the mark, though that will often
 * be the point we already have.
 * Then for each mark group we find the last that is before the target,
 * and move the point to there.
 * Then update 'all' list, text ref and seq number.
 */

static void point_forward_to_mark(struct mark *p, struct mark *m)
{
	struct mark *ptmp, *pnear;
	int i;
	struct point_links *plnk = p->mdata;

	pnear = p;
	ptmp = p;
	tlist_for_each_entry_continue(ptmp, GRP_HEAD, view) {
		if (ptmp->seq < m->seq)
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
		struct point_links *pnlnk = pnear->mdata;

		tl = &pnlnk->lists[i];
		if (tlist_empty(tl))
			continue;
		tlist_for_each_continue(tl,  GRP_HEAD) {
			struct mark *mtmp;
			if (TLIST_TYPE(tl) != GRP_MARK)
				break;
			mtmp = container_of(tl, struct mark, view);
			if (mtmp->seq < m->seq)
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
	hlist_add_before(&p->all, &m->all);
	p->ref = m->ref;
	assign_seq(p, hlist_prev_entry(p, all)->seq);
}

static void point_backward_to_mark(struct mark *p, struct mark *m)
{
	struct mark *ptmp, *pnear;
	int i;
	struct point_links *plnk = p->mdata;

	pnear = p;
	ptmp = p;
	tlist_for_each_entry_continue_reverse(ptmp, GRP_HEAD, view) {
		if (ptmp->seq > m->seq)
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
		struct point_links *pnlnk = pnear->mdata;

		tl = &pnlnk->lists[i];
		if (tlist_empty(tl))
			continue;
		tlist_for_each_continue_reverse(tl, GRP_HEAD) {
			struct mark *mtmp;
			if (TLIST_TYPE(tl) != GRP_MARK)
				break;
			mtmp = container_of(tl, struct mark, view);
			if (mtmp->seq > m->seq)
				mnear = mtmp;
			else
				break;
		}
		if (mnear) {
			tlist_del(&plnk->lists[i]);
			tlist_add_tail(&plnk->lists[i], GRP_LIST, &mnear->view);
		} else if (p != pnear) {
			tlist_del(&plnk->lists[i]);
			tlist_add_tail(&plnk->lists[i], GRP_LIST, &pnlnk->lists[i]);
		}
	}
	/* finally move in the overall list */
	hlist_del(&p->all);
	hlist_add_after(&m->all, &p->all);
	p->ref = m->ref;
	p->rpos = m->rpos;
	assign_seq(p, m->seq);
}

void point_to_mark(struct mark *p, struct mark *m)
{
	if (p->seq < m->seq)
		point_forward_to_mark(p, m);
	else if (p->seq > m->seq)
		point_backward_to_mark(p, m);
	p->rpos = m->rpos;
}

void mark_to_mark(struct mark *m, struct mark *target)
{

	if (m->viewnum == MARK_POINT) {
		point_to_mark(m, target);
		return;
	}
	while (mark_ordered(m, target)) {
		struct mark *n = doc_next_mark_all(m);
		mark_forward_over(m, n);
	}
	while (mark_ordered(target, m)) {
		struct mark *n = doc_prev_mark_all(m);
		mark_backward_over(m, n);
	}
	m->ref = target->ref;
	m->rpos = target->rpos;
}

int mark_same2(struct doc *d, struct mark *m1, struct mark *m2, struct cmd_info *ci)
{
	struct cmd_info ci2 = {0};

	if (!ci)
		ci = &ci2;
	ci->key = "doc:mark-same";
	ci->mark = m1;
	ci->mark2 = m2;
	ci->home = d->home;
	return key_handle(ci) == 1;
}

int mark_same(struct doc *d, struct mark *m1, struct mark *m2)
{
	return mark_same2(d, m1, m2, NULL);
}

int mark_same_pane(struct pane *p, struct mark *m1, struct mark *m2,
		   struct cmd_info *ci)
{
	struct cmd_info ci2 = {0};

	if (!ci)
		ci = &ci2;
	ci->key = "doc:mark-same";
	ci->mark = m1;
	ci->mark2 = m2;
	ci->focus = p;
	return key_handle(ci) == 1;
}

/* A 'vmark' is a mark in a particular view.  We can walk around those
 * silently skipping over the points.
 */

static struct mark *__vmark_next(struct tlist_head *tl)
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

struct mark *vmark_next(struct mark *m)
{
	struct tlist_head *tl;

	tl = TLIST_PTR(m->view.next);
	return __vmark_next(tl);
}

static struct mark *__vmark_prev(struct tlist_head *tl)
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

struct mark *vmark_prev(struct mark *m)
{
	struct tlist_head *tl;

	tl = TLIST_PTR(m->view.prev);
	return __vmark_prev(tl);
}

struct mark *do_vmark_first(struct doc *d, int view)
{
	struct tlist_head *tl;

	if (view < 0 || view >= d->nviews)
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

struct mark *do_vmark_last(struct doc *d, int view)
{
	struct tlist_head *tl;

	if (view < 0 || view >= d->nviews)
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

DEF_CMD(take_marks)
{
	struct call_return *cr = container_of(ci->comm, struct call_return, c);
	cr->m = ci->mark;
	cr->m2 = ci->mark2;
	return 1;
}

static int vmark_get(struct pane *p, int view,
		     struct mark **first, struct mark **last, struct mark **point, struct mark **new)
{
	struct cmd_info ci = {0};
	struct call_return cr;

	ci.key = "doc:vmark-get";
	ci.focus = p;
	ci.numeric = view;
	cr.c = take_marks;
	cr.m = cr.m2 = NULL;
	ci.comm2 = &cr.c;
	if (point)
		ci.extra = 1;
	else if (new)
		ci.extra = 2;
	if (key_handle(&ci) == 0)
		return 0;
	if (first)
		*first = cr.m;
	if (point)
		*point = cr.m2;
	else if (new)
		*new = cr.m2;
	else if (last)
		*last = cr.m2;
	return 1;
}

struct mark *vmark_first(struct pane *p, int view)
{
	struct mark *first = NULL;
	if (vmark_get(p, view, &first, NULL, NULL, NULL) == 0)
		return 0;
	return first;
}

struct mark *vmark_last(struct pane *p, int view)
{
	struct mark *last = NULL;
	if (vmark_get(p, view, NULL, &last, NULL, NULL) == 0)
		return 0;
	return last;
}

struct mark *vmark_at_point(struct pane *p, int view)
{
	struct mark *point = NULL;
	if (vmark_get(p, view, NULL, NULL, &point, NULL) == 0)
		return 0;
	return point;
}

struct mark *vmark_at_or_before(struct pane *p, struct mark *m, int view)
{
	struct cmd_info ci = {0};
	struct call_return cr;

	ci.key = "doc:vmark-get";
	ci.focus = p;
	ci.numeric = view;
	ci.extra = 3;
	ci.mark = m;
	cr.c = take_marks;
	cr.m = cr.m2 = NULL;
	ci.comm2 = &cr.c;
	if (key_handle(&ci) == 0)
		return NULL;
	return cr.m2;
}

struct mark *vmark_new(struct pane *p, int view)
{
	struct mark *new = NULL;
	if (vmark_get(p, view, NULL, NULL, NULL, &new) == 0)
		return 0;
	return new;
}

struct mark *vmark_matching(struct pane *p, struct mark *m)
{
	/* Find a nearby mark in the same view with the same ref */
	struct mark *m2;

	m2 = vmark_prev(m);
	if (m2 && mark_same_pane(p, m, m2, NULL))
		return m2;
	m2 = vmark_next(m);
	if (m2 && mark_same_pane(p, m, m2, NULL))
		return m2;
	return NULL;
}

struct mark *do_vmark_at_point(struct doc *d, struct mark *pt, int view)
{
	struct tlist_head *tl;
	struct mark *m;
	struct point_links *lnk = pt->mdata;

	if (view < 0 || view >= d->nviews)
		return NULL;

	tl = &lnk->lists[view];
	m = __vmark_prev(tl);
	if (m && mark_same(d, m, pt))
		return m;
	tl = &lnk->lists[view];
	m = __vmark_next(tl);
	if (m && mark_same(d, m, pt))
		return m;
	return NULL;
}

struct mark *do_vmark_at_or_before(struct doc *d, struct mark *m, int view)
{
	/* First the last 'view' mark that is not later in the document than 'm'.
	 * It might be later in the mark list, but not in the document.
	 * Return NULL if all 'view' marks are after 'm' in the document.
	 */
	struct mark *vm = m;

	/* might need to hunt along 'all' list for something suitable */
	while (vm && vm->viewnum != MARK_POINT && vm->viewnum != view)
		vm = doc_next_mark_all(vm);
	if (!vm) {
		vm = m;
		while (vm && vm->viewnum != MARK_POINT && vm->viewnum != view)
			vm = doc_prev_mark_all(vm);
	}
	if (!vm)
		/* No 'view' marks at all! */
		return vm;
	/* 'vm' is either a point or a 'view' mark.  It is probably after 'm',
	 * but if it is before, then no 'view' mark is after.
	 */
	if (vm->viewnum == MARK_POINT) {
		struct point_links *lnk = vm->mdata;
		struct tlist_head *tl = &lnk->lists[view];
		vm = __vmark_next(tl);
		if (vm && mark_same(d, vm, m)) {
			/* maybe there are even more */
			struct mark *vm2;
			while ((vm2 = vmark_next(vm)) != NULL &&
			       mark_same(d, vm, vm2))
				vm = vm2;
		} else
			vm = __vmark_prev(tl);
	} else if (vm->viewnum == view) {
		/* Just use this, or nearby */
		struct mark *vm2;
		while ((vm2 = vmark_next(vm)) != NULL &&
		       mark_same(d, vm, m))
			vm = vm2;
		while (vm && vm->seq > m->seq && !mark_same(d, vm, m))
			vm = vmark_prev(vm);

	}
	return vm;
}

static void point_notify_change(struct doc *d, struct mark *p, struct mark *m)
{
	/* Notify of changes from m (might be NULL) to p.
	 * Notify the last mark which is before p or m,
	 * and all marks with the same ref as p or m.
	 * There will be none in between.
	 */
	struct cmd_info ci = {0};
	int i;
	struct point_links *lnk = p->mdata;

	ci.key = "Notify:Replace";
	ci.numeric = 1;
	ci.x = ci.y = -1;
	if (!m)
		m = p;
	for (i = 0; i < lnk->size; i++) {
		struct tlist_head *tl = &lnk->lists[i];
		struct command *c = d->views[i].notify;

		if (!c || d->views[i].state == 0)
			continue;
		ci.comm = c;
		while (TLIST_TYPE(tl) == GRP_LIST)
			tl = TLIST_PTR(tl->prev);
		if (TLIST_TYPE(tl) == GRP_MARK)
			ci.mark = tlist_entry(tl, struct mark, view);
		else
			ci.mark = NULL;
		c->func(&ci);
		while (TLIST_TYPE(tl) == GRP_MARK &&
		       (mark_ordered_or_same(d, m, ci.mark))) {
			do
				tl = TLIST_PTR(tl->prev);
			while (TLIST_TYPE(tl) == GRP_LIST);

			if (TLIST_TYPE(tl) == GRP_MARK) {
				ci.mark = tlist_entry(tl, struct mark, view);
				c->func(&ci);
			}
		}

		/* Now any relevant marks after point but at the same ref */
		tl = &lnk->lists[i];
		while (TLIST_TYPE(tl) != GRP_HEAD) {
			if (TLIST_TYPE(tl) == GRP_MARK) {
				ci.mark = tlist_entry(tl, struct mark, view);
				if (mark_same(d, ci.mark, p))
					c->func(&ci);
				else
					break;
			}
			tl = TLIST_PTR(tl->next);
		}
	}
}

/* doc_notify_change is slower than point_notify_change, but only
 * requires a mark, not a point.
 * A second mark should only be given if the first mark is a point
 */
void doc_notify_change(struct doc *d, struct mark *m, struct mark *m2)
{
	struct cmd_info ci = {0};
	char *done;
	int i;
	int remaining = d->nviews;

	pane_notify(d->home, "Notify:Replace", m, m2, NULL);

	if (m->viewnum == MARK_POINT) {
		point_notify_change(d, m, m2);
		return;
	}
	ASSERT(m2 == NULL);

	done = alloca(d->nviews);
	for (i = 0; i < d->nviews; i++)
		done[i] = 0;
	ci.key = "Notify:Replace";
	ci.numeric = 1;
	ci.x = ci.y = -1;
	while (remaining) {
		if (m->viewnum == MARK_POINT) {
			/* This is a point so we can notify all remaining easily. */
			struct point_links *lnk = m->mdata;
			for (i = 0; i < lnk->size; i++) {
				struct tlist_head *tl = &lnk->lists[i];
				struct command *c = d->views[i].notify;
				if (done[i])
					continue;
				done[i] = 1;
				remaining -= 1;
				if (!c || d->views[i].state == 0)
					continue;
				while (TLIST_TYPE(tl) == GRP_LIST)
					tl = TLIST_PTR(tl->prev);
				if (TLIST_TYPE(tl) == GRP_MARK)
					ci.mark = tlist_entry(tl, struct mark, view);
				else
					ci.mark = NULL;
				ci.comm = c;
				c->func(&ci);
			}
			break;
		}
		if (m->viewnum != MARK_UNGROUPED &&
		    !done[m->viewnum]) {
			/* Just notify this one */
			struct command *c = d->views[m->viewnum].notify;
			done[m->viewnum] = 1;
			remaining -= 1;
			if (c && d->views[m->viewnum].state) {
				ci.mark = m;
				ci.comm = c;
				c->func(&ci);
			}
		}
		if (HLIST_IS_HEAD(m->all.pprev)) {
			/* Notify everything else with a NULL mark */
			for (i = 0; i < d->nviews; i++) {
				struct command *c = d->views[i].notify;
				if (done[i])
					continue;
				if (!c || d->views[i].state == 0)
					continue;
				ci.mark = NULL;
				ci.comm = c;
				c->func(&ci);
			}
			break;
		}
		m = hlist_prev_entry(m, all);
	}
}


void doc_check_consistent(struct doc *d)
{
	/* Check consistency of marks, and abort if not.
	 * Check:
	 * - all marks are in seq order
	 * - all view lists are in seq order
	 */
	struct mark *m;
	int seq = 0;
	int i;

	hlist_for_each_entry(m, &d->marks, all) {
		if (m->seq < seq)
			abort();
		seq = m->seq + 1;
	}
	for (i = 0; i < d->nviews; i++)
		if (d->views[i].state == 0) {
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
					pl = container_of(tl, struct point_links, lists[i]);
					m = pl->pt;
					break;
				default: abort();
				}
				if (m->seq < seq)
					abort();
				seq = m->seq + 1;
			}
		}
}
