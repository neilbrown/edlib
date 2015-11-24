/*
 * Copyright Neil Brown <neil@brown.name> 2015
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

void mark_free(struct mark *m)
{
	if (m) {
		mark_delete(m);
		free(m);
	}
}

void point_free(struct point *p)
{
	int i;
	*p->owner = NULL;
	for (i = 0; i < p->size; i++)
		tlist_del_init(&p->lists[i]);
	mark_delete(&p->m);
	free(p);
}

static void dup_mark(struct mark *orig, struct mark *new)
{
	new->ref = orig->ref;
	new->rpos = orig->rpos;
	new->attrs= NULL;
	hlist_add_after(&orig->all, &new->all);
	assign_seq(new, orig->seq);
}

struct mark *mark_at_point(struct point *p, int view)
{
	struct mark *ret;
	int size = sizeof(*ret);

	if (view >= 0)
		size += p->doc->views[view].space;

	ret = calloc(size, 1);

	dup_mark(&p->m, ret);
	ret->viewnum = view;
	if (view >= 0)
		tlist_add(&ret->view, GRP_MARK, &p->lists[view]);
	else
		INIT_TLIST_HEAD(&ret->view, GRP_MARK);
	return ret;
}

struct point *point_dup(struct point *p, struct point **owner)
{
	int i;
	struct point *ret = malloc(sizeof(*ret) +
				   p->size * sizeof(ret->lists[0]));

	dup_mark(&p->m, &ret->m);
	ret->m.viewnum = MARK_POINT;
	ret->size = p->size;
	tlist_add(&ret->m.view, GRP_MARK, &p->m.view);
	for (i = 0; i < ret->size; i++)
		if (tlist_empty(&p->lists[i]))
			INIT_TLIST_HEAD(&ret->lists[i], GRP_LIST);
		else
			tlist_add(&ret->lists[i], GRP_LIST, &p->lists[i]);
	*owner = ret;
	ret->owner = owner;
	ret->doc = p->doc;
	return ret;
}

void points_resize(struct doc *d)
{
	struct point *p;
	tlist_for_each_entry(p, &d->points, m.view) {
		int i;
		struct point *new = malloc(sizeof(*new) +
					   d->nviews * sizeof(new->lists[0]));
		new->m.ref = p->m.ref;
		new->m.rpos = p->m.rpos;
		new->m.attrs = p->m.attrs;
		new->m.seq = p->m.seq;
		new->m.viewnum = p->m.viewnum;
		hlist_add_after(&p->m.all, &new->m.all);
		hlist_del(&p->m.all);
		tlist_add(&new->m.view, GRP_MARK, &p->m.view);
		tlist_del(&p->m.view);

		new->doc = p->doc;
		new->owner = p->owner;
		*new->owner = new;
		new->size = d->nviews;
		for (i = 0; i < p->size; i++) {
			tlist_add(&new->lists[i], GRP_LIST, &p->lists[i]);
			tlist_del(&p->lists[i]);
		}
		for (; i < new->size; i++)
			INIT_TLIST_HEAD(&new->lists[i], GRP_HEAD);
		p = new;
	}
}

void points_attach(struct doc *d, int view)
{
	struct point *p;
	tlist_for_each_entry(p, &d->points, m.view)
		tlist_add_tail(&p->lists[view], GRP_LIST, &d->views[view].head);
}

struct mark *mark_dup(struct mark *m, int notype)
{
	struct mark *ret;
	int size = sizeof(*ret);

	if (!notype) {
		struct tlist_head *tl;
		struct docview *dv;
		if (m->viewnum == MARK_POINT)
			return NULL;
		tl = &m->view;
		while (TLIST_TYPE(tl) != GRP_HEAD)
			tl = TLIST_PTR(tl->next);
		dv = container_of(tl, struct docview, head);
		size += dv->space;
	}

	ret = calloc(size, 1);
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

void __mark_reset(struct doc *d, struct mark *m, int new)
{
	struct point *p, pt;
	int i;
	struct cmd_info ci = {0};

	m->rpos = 0;
	if (!new)
		hlist_del(&m->all);
	hlist_add_head(&m->all, &d->marks);
	assign_seq(m, 0);

	ci.key = "doc:set-ref";
	ci.mark = m;
	ci.numeric = 1; /* start */
	pt.doc = d;
	p = &pt;
	ci.pointp = &p;
	key_lookup(d->map, &ci);

	if (m->viewnum == MARK_UNGROUPED)
		return;
	if (m->viewnum != MARK_POINT) {
		if (!new)
			tlist_del(&m->view);
		tlist_add(&m->view, GRP_MARK, &d->views[m->viewnum].head);
		return;
	}
	/* MARK_POINT */
	if (!new)
		tlist_del(&m->view);
	tlist_add(&m->view, GRP_MARK, &d->points);
	p = container_of(m, struct point, m);
	for (i = 0; i < p->size; i++)
		if (d->views[i].notify) {
			if (!new)
				tlist_del(&p->lists[i]);
			tlist_add(&p->lists[i], GRP_LIST, &d->views[i].head);
		} else if (new)
			INIT_TLIST_HEAD(&p->lists[i], GRP_LIST);
}

struct point *point_new(struct doc *d, struct point **owner)
{
	struct point *ret = malloc(sizeof(*ret) +
				   d->nviews * sizeof(ret->lists[0]));

	ret->m.attrs = NULL;
	ret->m.viewnum = MARK_POINT;
	ret->size = d->nviews;
	ret->owner = owner;
	ret->doc = d;
	__mark_reset(d, &ret->m, 1);
	if (owner)
		*owner = ret;
	return ret;
}

void mark_reset(struct doc *d, struct mark *m)
{
	__mark_reset(d, m, 0);
}

void point_reset(struct point *p)
{
	struct doc *d = p->doc;
	__mark_reset(d, &p->m, 0);
}

struct mark *doc_first_mark(struct doc *d, int view)
{
	struct tlist_head *tl;

	if (view < 0 || view >= d->nviews || d->views[view].notify == NULL)
		return NULL;
	if (tlist_empty(&d->views[view].head))
		return NULL;
	tlist_for_each(tl, &d->views[view].head)
		if (TLIST_TYPE(tl) == GRP_MARK)
			return tlist_entry(tl, struct mark, view);
	return NULL;
}

struct mark *doc_next_mark(struct doc *d, struct mark *m)
{
	int view = m->viewnum;
	struct tlist_head *tl = &m->view;

	tlist_for_each_continue(tl, &d->views[view].head)
		if (TLIST_TYPE(tl) == GRP_MARK)
			return tlist_entry(tl, struct mark, view);
	return NULL;
}

struct mark *doc_first_mark_all(struct doc *d)
{
	if (d->marks.first)
		return hlist_first_entry(&d->marks, struct mark, all);
	return NULL;
}

struct mark *doc_next_mark_all(struct doc *d, struct mark *m)
{
	if (m->all.next)
		return hlist_next_entry(m, all);
	return NULL;
}

struct mark *doc_prev_mark_all(struct doc *d, struct mark *m)
{
	if (d->marks.first != &m->all)
		return hlist_prev_entry(m, all);
	return NULL;
}


struct mark *doc_new_mark(struct doc *d, int view)
{
	struct mark *ret;

	if (view == MARK_POINT || view >= d->nviews || d->views[view].notify == NULL)
		return NULL;
	ret = malloc(sizeof(*ret));
	ret->rpos = 0;
	ret->attrs = NULL;
	ret->viewnum = view;
	__mark_reset(d, ret, 1);
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

static struct mark *next_mark(struct doc *d, struct mark *m)
{
	if (m->all.next == NULL)
		return NULL;
	return hlist_next_entry(m, all);
}
static struct mark *prev_mark(struct doc *d, struct mark *m)
{
	if (m->all.pprev == &d->marks.first)
		return NULL;
	return hlist_prev_entry(m, all);
}

static void swap_lists(struct point *p1, struct point *p2)
{
	int i;
	for (i = 0; i < p1->size; i++) {
		tlist_del(&p1->lists[i]);
		tlist_add(&p1->lists[i], GRP_LIST, &p2->lists[i]);
	}
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
		struct point *p = container_of(m, struct point, m);
		struct point *p2 = container_of(m2, struct point, m);
		swap_lists(p, p2);
	} else if (m->viewnum == MARK_POINT) {
		/* Moving a point over a mark */
		struct point *p = container_of(m, struct point, m);
		if (m2->viewnum >= 0) {
			tlist_del(&m2->view);
			tlist_add_tail(&m2->view, GRP_MARK, &p->lists[m2->viewnum]);
		}
	} else if (m2->viewnum == MARK_POINT) {
		/* stepping a mark over a point */
		struct point *p = container_of(m2, struct point, m);
		if (m->viewnum >= 0) {
			tlist_del(&m->view);
			tlist_add(&m->view, GRP_MARK, &p->lists[m->viewnum]);
		}
	}
	seq = m->seq;
	m->seq = m2->seq;
	m2->seq = seq;
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
		struct point *p = container_of(m, struct point, m);
		struct point *pp = container_of(mp, struct point, m);
		swap_lists(pp, p);
	} else if (m->viewnum == MARK_POINT) {
		/* Moving a point over a mark */
		struct point *p = container_of(m, struct point, m);
		if (mp->viewnum >= 0) {
			tlist_del(&mp->view);
			tlist_add(&mp->view, GRP_MARK, &p->lists[mp->viewnum]);
		}
	} else if (mp->viewnum == MARK_POINT) {
		/* Step back over a point */
		struct point *p = container_of(mp, struct point, m);
		if (m->viewnum >= 0) {
			tlist_del(&m->view);
			tlist_add_tail(&m->view, GRP_MARK, &p->lists[m->viewnum]);
		}
	}
	seq = m->seq;
	m->seq = mp->seq;
	mp->seq = seq;
}

wint_t mark_step(struct doc *d, struct mark *m, int forward, int move, struct cmd_info *ci)
{
	struct point p, *pt = &p;

	p.doc = d;
	ci->key = "doc:step";
	ci->pointp = &pt;
	ci->mark = m;
	ci->numeric = forward;
	ci->extra = move;
	key_lookup(d->map, ci);
	return ci->extra;
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

	while ((m2 = next_mark(d, m)) != NULL &&
	       mark_same(d, m, m2))
		mark_forward_over(m, m2);

	ret = mark_step2(d, m, 1, 1);
	if (ret == WEOF)
		return ret;

/* FIXME do I need to do this - is it precise enough? */
	while ((m2 = next_mark(d, m)) != NULL &&
	       mark_same(d, m, m2))
		mark_forward_over(m, m2);
	return ret;
}

wint_t mark_prev(struct doc *d, struct mark *m)
{
	wint_t ret;
	struct mark *mp = NULL;

	while ((mp = prev_mark(d, m)) != NULL &&
	       mark_same(d, m, mp))
		mark_backward_over(m, mp);

	ret = mark_step2(d, m, 0, 1);
	if (ret == WEOF)
		return ret;
	while ((mp = prev_mark(d, m)) != NULL &&
	       mark_same(d, m, mp))
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

static void point_forward_to_mark(struct point *p, struct mark *m)
{
	struct point *ptmp, *pnear;
	int i;
	struct doc *d = p->doc;

	pnear = p;
	ptmp = p;
	tlist_for_each_entry_continue(ptmp, &d->points, m.view) {
		if (ptmp->m.seq < m->seq)
			pnear = ptmp;
		else
			break;
	}
	/* pnear is the nearest point to m that is before m. So
	 * move p after pnear in the point list. */
	if (p != pnear) {
		tlist_del(&p->m.view);
		tlist_add(&p->m.view, GRP_MARK, &pnear->m.view);
	}

	/* Now move 'p' in the various mark lists */
	for (i = 0; i < p->size; i++) {
		struct mark *mnear = NULL;
		struct tlist_head *tl;

		if (!d->views[i].notify)
			continue;
		tl = &pnear->lists[i];
		tlist_for_each_continue(tl,  &d->views[i].head) {
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
			tlist_del(&p->lists[i]);
			tlist_add(&p->lists[i], GRP_LIST, &mnear->view);
		} else if (p != pnear) {
			tlist_del(&p->lists[i]);
			tlist_add(&p->lists[i], GRP_LIST, &pnear->lists[i]);
		}
	}
	/* finally move in the overall list */
	hlist_del(&p->m.all);
	hlist_add_before(&p->m.all, &m->all);
	p->m.ref = m->ref;
	assign_seq(&p->m, hlist_prev_entry(&p->m, all)->seq);
}

static void point_backward_to_mark(struct point *p, struct mark *m)
{
	struct point *ptmp, *pnear;
	struct doc *d = p->doc;
	int i;

	pnear = p;
	ptmp = p;
	tlist_for_each_entry_continue_reverse(ptmp, &d->points, m.view) {
		if (ptmp->m.seq > m->seq)
			pnear = ptmp;
		else
			break;
	}
	/* pnear is the nearest point to m that is after m. So
	 * move p before pnear in the point list */
	if (p != pnear) {
		tlist_del(&p->m.view);
		tlist_add_tail(&p->m.view, GRP_MARK, &pnear->m.view);
	}

	/* Now move 'p' in the various mark lists */
	for (i = 0; i < p->size; i++) {
		struct mark *mnear = NULL;
		struct tlist_head *tl;

		if (!d->views[i].notify)
			continue;
		tl = &pnear->lists[i];
		tlist_for_each_continue_reverse(tl, &d->views[i].head) {
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
			tlist_del(&p->lists[i]);
			tlist_add_tail(&p->lists[i], GRP_LIST, &mnear->view);
		} else if (p != pnear) {
			tlist_del(&p->lists[i]);
			tlist_add_tail(&p->lists[i], GRP_LIST, &pnear->lists[i]);
		}
	}
	/* finally move in the overall list */
	hlist_del(&p->m.all);
	hlist_add_after(&m->all, &p->m.all);
	p->m.ref = m->ref;
	p->m.rpos = m->rpos;
	assign_seq(&p->m, m->seq);
}

void point_to_mark(struct point *p, struct mark *m)
{
	if (p->m.seq < m->seq)
		point_forward_to_mark(p, m);
	else if (p->m.seq > m->seq)
		point_backward_to_mark(p, m);
	p->m.rpos = m->rpos;
}

void mark_to_mark(struct doc *d, struct mark *m, struct mark *target)
{

	if (m->viewnum == MARK_POINT) {
		point_to_mark(container_of(m, struct point, m),
			      target);
		return;
	}
	while (mark_ordered(m, target)) {
		struct mark *n = doc_next_mark_all(d, m);
		mark_forward_over(m, n);
	}
	while (mark_ordered(target, m)) {
		struct mark *n = doc_prev_mark_all(d, m);
		mark_backward_over(m, n);
	}
	m->ref = target->ref;
	m->rpos = target->rpos;
}

int mark_same2(struct doc *d, struct mark *m1, struct mark *m2, struct cmd_info *ci)
{
	struct point p, *pt = &p;
	struct cmd_info ci2 = {0};
	p.doc = d;
	if (!ci)
		ci = &ci2;
	ci->key = "doc:mark-same";
	ci->mark = m1;
	ci->mark2 = m2;
	ci->pointp = &pt;
	ci->extra = 0;
	key_lookup(d->map, ci);
	return ci->extra;
}

int mark_same(struct doc *d, struct mark *m1, struct mark *m2)
{
	return mark_same2(d, m1, m2, NULL);
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

struct mark *vmark_first(struct doc *d, int view)
{
	struct tlist_head *tl;

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

struct mark *vmark_last(struct doc *d, int view)
{
	struct tlist_head *tl;

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

struct mark *vmark_matching(struct doc *d, struct mark *m)
{
	/* Find a nearby mark in the same view with the same ref */
	struct mark *m2;

	m2 = vmark_prev(m);
	if (m2 && mark_same(d, m, m2))
		return m2;
	m2 = vmark_next(m);
	if (m2 && mark_same(d, m, m2))
		return m2;
	return NULL;
}

struct mark *vmark_at_point(struct point *pt, int view)
{
	struct tlist_head *tl;
	struct mark *m;

	tl = &pt->lists[view];
	m = __vmark_prev(tl);
	if (m && mark_same(pt->doc, m, &pt->m))
		return m;
	tl = &pt->lists[view];
	m = __vmark_next(tl);
	if (m && mark_same(pt->doc, m, &pt->m))
		return m;
	return NULL;
}

void point_notify_change(struct point *p, struct mark *m)
{
	/* Notify of changes from m (might be NULL) to p.
	 * Notify the last mark which is before p or m,
	 * and all marks with the same ref as p or m.
	 * There will be none in between.
	 */
	struct cmd_info ci = {0};
	struct doc *d = p->doc;
	int i;

	ci.key = "Replace";
	ci.numeric = 1;
	ci.x = ci.y = -1;
	ci.pointp = p->owner;
	if (!m)
		m = &p->m;
	for (i = 0; i < p->size; i++) {
		struct tlist_head *tl = &p->lists[i];
		struct command *c = d->views[i].notify;

		if (!c)
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
		tl = &p->lists[i];
		while (TLIST_TYPE(tl) != GRP_HEAD) {
			if (TLIST_TYPE(tl) == GRP_MARK) {
				ci.mark = tlist_entry(tl, struct mark, view);
				if (mark_same(d, ci.mark, &p->m))
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
 */
void doc_notify_change(struct doc *d, struct mark *m)
{
	struct cmd_info ci = {0};
	char *done = alloca(d->nviews);
	int i;
	int remaining = d->nviews;

	for (i = 0; i < d->nviews; i++)
		done[i] = 0;
	ci.key = "Replace";
	ci.numeric = 1;
	ci.x = ci.y = -1;
	while (remaining) {
		if (m->viewnum == MARK_POINT) {
			/* This is a point so we can notify all remaining easily. */
			struct point *p = container_of(m, struct point, m);
			for (i = 0; i < p->size; i++) {
				struct tlist_head *tl = &p->lists[i];
				struct command *c = d->views[i].notify;
				if (done[i])
					continue;
				done[i] = 1;
				remaining -= 1;
				if (!c)
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
			if (c) {
				ci.mark = m;
				ci.comm = c;
				c->func(&ci);
			}
		}
		if (m->all.pprev == &d->marks.first) {
			/* Notify everything else with a NULL mark */
			for (i = 0; i < d->nviews; i++) {
				struct command *c = d->views[i].notify;
				if (done[i])
					continue;
				if (!c)
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
		if (d->views[i].notify == NULL) {
			if (!tlist_empty(&d->views[i].head)) abort();
		} else {
			struct tlist_head *tl;
			struct point *p;
			seq = 0;
			tlist_for_each(tl, &d->views[i].head) {
				switch(TLIST_TYPE(tl)) {
				case GRP_HEAD: abort();
				case GRP_MARK:
					m = container_of(tl, struct mark, view);
					break;
				case GRP_LIST:
					p = container_of(tl, struct point, lists[i]);
					m = &p->m;
					break;
				default: abort();
				}
				if (m->seq < seq)
					abort();
				seq = m->seq + 1;
			}
		}
}
