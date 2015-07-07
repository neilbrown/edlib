/* Marks and Points.
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
 * Each point has one reference from a window (where it is the cursor)
 * and that can be found through the ->owner link.
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
#include <wchar.h>
#include "list.h"
#include "text.h"
#include "attr.h"
#include "mark.h"
#include "pane.h"
#include "keymap.h"

struct mark {
	struct text_ref		ref;
	struct hlist_node	all;
	struct tlist_head	group;
	struct attrset		*attrs;
	int			seq;
	int			type;
};

struct point {
	struct mark		m;
	struct point		**owner;
	int			size;
	struct tlist_head	lists[];
};

static void mark_check_consistent(struct text *t);

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
}

static void mark_delete(struct mark *m)
{
	hlist_del_init(&m->all);
	if (m->type != MARK_UNGROUPED)
		tlist_del_init(&m->group);
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
	for (i = 0; i < p->size; i++)
		tlist_del_init(&p->lists[i]);
	mark_delete(&p->m);
	free(p);
}

static void dup_mark(struct mark *orig, struct mark *new)
{
	new->ref = orig->ref;
	new->attrs= NULL;
	hlist_add_after(&orig->all, &new->all);
	assign_seq(new, orig->seq);
}

struct mark *mark_at_point(struct point *p, int type)
{
	struct mark *ret = malloc(sizeof(*ret));

	dup_mark(&p->m, ret);
	ret->type = type;
	if (type >= 0)
		tlist_add(&ret->group, GRP_MARK, &p->lists[type]);
	else
		INIT_TLIST_HEAD(&ret->group, GRP_MARK);
	return ret;
}

struct point *point_dup(struct point *p, struct point **owner)
{
	int i;
	struct point *ret = malloc(sizeof(*ret) +
				   p->size * sizeof(ret->lists[0]));

	dup_mark(&p->m, &ret->m);
	ret->m.type = MARK_POINT;
	ret->size = p->size;
	tlist_add(&ret->m.group, GRP_MARK, &p->m.group);
	for (i = 0; i < ret->size; i++)
		if (tlist_empty(&p->lists[i]))
			INIT_TLIST_HEAD(&ret->lists[i], GRP_LIST);
		else
			tlist_add(&ret->lists[i], GRP_LIST, &p->lists[i]);
	ret->owner = owner;
	*owner = ret;
	return ret;
}

void points_resize(struct text *t)
{
	struct point *p;
	tlist_for_each_entry(p, &t->points, m.group) {
		int i;
		struct point *new = malloc(sizeof(*new) +
					   t->ngroups * sizeof(new->lists[0]));
		new->m.ref = p->m.ref;
		new->m.attrs = p->m.attrs;
		new->m.seq = p->m.seq;
		new->m.type = p->m.type;
		hlist_add_after(&p->m.all, &new->m.all);
		hlist_del(&p->m.all);
		tlist_add(&new->m.group, GRP_MARK, &p->m.group);
		tlist_del(&p->m.group);

		new->owner = p->owner;
		*(new->owner) = new;
		new->size = t->ngroups;
		for (i = 0; i < p->size; i++) {
			tlist_add(&new->lists[i], GRP_LIST, &p->lists[i]);
			tlist_del(&p->lists[i]);
		}
		for (; i < new->size; i++)
			INIT_TLIST_HEAD(&new->lists[i], GRP_HEAD);
		p = new;
	}
}

void points_attach(struct text *t, int type)
{
	struct point *p;
	tlist_for_each_entry(p, &t->points, m.group)
		tlist_add_tail(&p->lists[type], GRP_LIST, &t->groups[type].head);
}

struct mark *mark_dup(struct mark *m, int notype)
{
	struct mark *ret = malloc(sizeof(*ret));
	dup_mark(m, ret);
	if (notype) {
		ret->type = MARK_UNGROUPED;
		INIT_TLIST_HEAD(&ret->group, GRP_MARK);
	} else {
		ret->type = m->type;
		if (ret->type == MARK_UNGROUPED)
			INIT_TLIST_HEAD(&ret->group, GRP_MARK);
		else
			tlist_add(&ret->group, GRP_MARK, &m->group);
	}
	return ret;
}

struct point *point_new(struct text *t, struct point **owner)
{
	int i;
	struct point *ret = malloc(sizeof(*ret) +
				   t->ngroups * sizeof(ret->lists[0]));

	ret->m.ref = text_find_ref(t, 0);
	ret->m.attrs = NULL;
	hlist_add_head(&ret->m.all, &t->marks);
	assign_seq(&ret->m, 0);
	ret->m.type = MARK_POINT;
	ret->size = t->ngroups;
	tlist_add(&ret->m.group, GRP_MARK, &t->points);
	for (i = 0; i < ret->size; i++)
		if (t->groups[i].notify)
			tlist_add(&ret->lists[i], GRP_LIST, &t->groups[i].head);
		else
			INIT_TLIST_HEAD(&ret->lists[i], GRP_LIST);
	ret->owner = owner;
	*owner = ret;
	return ret;
}

static void point_reset(struct text *t, struct point *p)
{
	int i;
	/* move point to start of text */
	p->m.ref = text_find_ref(t, 0);
	hlist_del(&p->m.all);
	hlist_add_head(&p->m.all, &t->marks);
	tlist_del(&p->m.group);
	tlist_add(&p->m.group, GRP_MARK, &t->points);
	for (i = 0; i < p->size; i++)
		if (t->groups[i].notify) {
			tlist_del(&p->lists[i]);
			tlist_add(&p->lists[i], GRP_LIST, &t->groups[i].head);
		}
	assign_seq(&p->m, 0);
}

struct text_ref point_ref(struct point *p)
{
	return p->m.ref;
}

int mark_ordered(struct mark *m1, struct mark *m2)
{
	return m1->seq < m2->seq;
}

int mark_same(struct text *t, struct mark *m1, struct mark *m2)
{
	return text_ref_same(t, &m1->ref, &m2->ref);
}

struct mark *mark_of_point(struct point *p)
{
	return &p->m;
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
wchar_t mark_following(struct text *t, struct mark *m)
{
	struct text_ref r = m->ref;
	return text_next(t, &r);
}

wchar_t mark_prior(struct text *t, struct mark *m)
{
	struct text_ref r = m->ref;
	return text_prev(t, &r);
}

static struct mark *next_mark(struct text *t, struct mark *m)
{
	if (m->all.next == NULL)
		return NULL;
	return hlist_next_entry(m, all);
}
static struct mark *prev_mark(struct text *t, struct mark *m)
{
	if (m->all.pprev == &t->marks.first)
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

static void fore_mark(struct mark *m, struct mark *m2)
{
	int seq;

	hlist_del(&m->all);
	hlist_add_after(&m2->all, &m->all);
	if (m->type == m2->type && m->type != MARK_UNGROUPED) {
		tlist_del(&m->group);
		tlist_add(&m->group, GRP_MARK, &m2->group);
	}
	if (m->type == MARK_POINT && m2->type == MARK_POINT) {
		/* moving a point over a point */
		struct point *p = container_of(m, struct point, m);
		struct point *p2 = container_of(m2, struct point, m);
		swap_lists(p, p2);
	} else if (m->type == MARK_POINT) {
		/* Moving a point over a mark */
		struct point *p = container_of(m, struct point, m);
		if (m2->type >= 0) {
			tlist_del(&m2->group);
			tlist_add_tail(&m2->group, GRP_MARK, &p->lists[m2->type]);
		}
	} else if (m2->type == MARK_POINT) {
		/* stepping a mark over a point */
		struct point *p = container_of(m2, struct point, m);
		if (m->type >= 0) {
			tlist_del(&m->group);
			tlist_add(&m->group, GRP_MARK, &p->lists[m->type]);
		}
	}
	seq = m->seq;
	m->seq = m2->seq;
	m2->seq = seq;
}

static void back_mark(struct mark *m, struct mark *mp)
{
	int seq;

	hlist_del(&m->all);
	hlist_add_before(&m->all, &mp->all);
	if (m->type == mp->type && m->type != MARK_UNGROUPED) {
		tlist_del(&m->group);
		tlist_add_tail(&m->group, GRP_MARK, &mp->group);
	}
	if (m->type == MARK_POINT && mp->type == MARK_POINT) {
		/* moving a point over a point */
		struct point *p = container_of(m, struct point, m);
		struct point *pp = container_of(mp, struct point, m);
		swap_lists(pp, p);
	} else if (m->type == MARK_POINT) {
		/* Moving a point over a mark */
		struct point *p = container_of(m, struct point, m);
		if (mp->type >= 0) {
			tlist_del(&mp->group);
			tlist_add(&mp->group, GRP_MARK, &p->lists[mp->type]);
		}
	} else if (mp->type == MARK_POINT) {
		/* Step back over a point */
		struct point *p = container_of(mp, struct point, m);
		if (m->type >= 0) {
			tlist_del(&m->group);
			tlist_add_tail(&m->group, GRP_MARK, &p->lists[m->type]);
		}
	}
	seq = m->seq;
	m->seq = mp->seq;
	mp->seq = seq;
}

wint_t mark_next(struct text *t, struct mark *m)
{
	wint_t ret;
	struct mark *m2 = NULL;
	while ((m2 = next_mark(t, m)) != NULL &&
	       m2->ref.c == m->ref.c &&
	       m2->ref.o <= m->ref.o)
		fore_mark(m, m2);

	ret = text_next(t, &m->ref);
	if (ret == WEOF)
		return ret;

	while ((m2 = next_mark(t, m)) != NULL &&
	       m2->ref.c == m->ref.c &&
	       m2->ref.o < m->ref.o)
			fore_mark(m, m2);
	return ret;
}

wint_t mark_prev(struct text *t, struct mark *m)
{
	wint_t ret;
	struct mark *mp = NULL;
	while ((mp = prev_mark(t, m)) != NULL &&
	       mp->ref.c == m->ref.c &&
	       mp->ref.o >= m->ref.o)
		back_mark(m, mp);
	ret = text_prev(t, &m->ref);
	if (ret == WEOF)
		return ret;
	while ((mp = prev_mark(t, m)) != NULL &&
	       mp->ref.c == m->ref.c &&
	       mp->ref.o > m->ref.o)
		back_mark(m, mp);
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

static void point_forward_to_mark(struct text *t, struct point *p, struct mark *m)
{
	struct point *ptmp, *pnear;
	int i;

	pnear = p;
	ptmp = p;
	tlist_for_each_entry_continue(ptmp, &t->points, m.group) {
		if (ptmp->m.seq < m->seq)
			pnear = ptmp;
		else
			break;
	}
	/* pnear is the nearest point to m that is before m. So
	 * move p after pnear in the point list. */
	if (p != pnear) {
		tlist_del(&p->m.group);
		tlist_add(&p->m.group, GRP_MARK, &pnear->m.group);
	}

	/* Now move 'p' in the various mark lists */
	for (i = 0; i < p->size; i++) {
		struct mark *mnear = NULL;
		struct tlist_head *tl;

		if (!t->groups[i].notify)
			continue;
		tl = &pnear->lists[i];
		tlist_for_each_continue(tl,  &t->groups[i].head) {
			struct mark *mtmp;
			if (TLIST_TYPE(tl) != GRP_MARK)
				break;
			mtmp = container_of(tl, struct mark, group);
			if (mtmp->seq < m->seq)
				mnear = mtmp;
			else
				break;
		}
		if (mnear) {
			tlist_del(&p->lists[i]);
			tlist_add(&p->lists[i], GRP_LIST, &mnear->group);
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

static void point_backward_to_mark(struct text *t, struct point *p, struct mark *m)
{
	struct point *ptmp, *pnear;
	int i;

	pnear = p;
	ptmp = p;
	tlist_for_each_entry_continue_reverse(ptmp, &t->points, m.group) {
		if (ptmp->m.seq > m->seq)
			pnear = ptmp;
		else
			break;
	}
	/* pnear is the nearest point to m that is after m. So
	 * move p before pnear in the point list */
	if (p != pnear) {
		tlist_del(&p->m.group);
		tlist_add_tail(&p->m.group, GRP_MARK, &pnear->m.group);
	}

	/* Now move 'p' in the various mark lists */
	for (i = 0; i < p->size; i++) {
		struct mark *mnear = NULL;
		struct tlist_head *tl;

		if (!t->groups[i].notify)
			continue;
		tl = &pnear->lists[i];
		tlist_for_each_continue_reverse(tl, &t->groups[i].head) {
			struct mark *mtmp;
			if (TLIST_TYPE(tl) != GRP_MARK)
				break;
			mtmp = container_of(tl, struct mark, group);
			if (mtmp->seq > m->seq)
				mnear = mtmp;
			else
				break;
		}
		if (mnear) {
			tlist_del(&p->lists[i]);
			tlist_add_tail(&p->lists[i], GRP_LIST, &mnear->group);
		} else if (p != pnear) {
			tlist_del(&p->lists[i]);
			tlist_add_tail(&p->lists[i], GRP_LIST, &pnear->lists[i]);
		}
	}
	/* finally move in the overall list */
	hlist_del(&p->m.all);
	hlist_add_after(&m->all, &p->m.all);
	p->m.ref = m->ref;
	assign_seq(&p->m, m->seq);
}

void point_to_mark(struct text *t, struct point *p, struct mark *m)
{
	if (p->m.seq < m->seq)
		point_forward_to_mark(t, p, m);
	else if (p->m.seq > m->seq)
		point_backward_to_mark(t, p, m);
}

static void point_notify_change(struct point *p, struct text *t)
{
	struct cmd_info ci;
	int i;

	ci.key = EV_REPLACE;
	ci.focus = NULL;
	ci.repeat = 1;
	ci.x = ci.y = -1;
	ci.str = NULL;
	ci.text = t;
	for (i = 0; i < p->size; i++) {
		struct tlist_head *tl = &p->lists[i];
		struct command *c = t->groups[i].notify;
		if (!c)
			continue;
		while (TLIST_TYPE(tl) == GRP_LIST)
			tl = TLIST_PTR(tl->prev);
		if (TLIST_TYPE(tl) == GRP_MARK)
			ci.mark = tlist_entry(tl, struct mark, group);
		else
			ci.mark = NULL;
		c->func(c, &ci);
	}
}

void point_insert_text(struct text *t, struct point *p, char *s, int *first)
{
	struct mark *m;
	struct text_ref start;

	m = &p->m;
	text_add_str(t, &p->m.ref, s, &start, first);
	hlist_for_each_entry_continue_reverse(m, &t->marks, all)
		if (text_update_prior_after_change(t, &m->ref,
						   &start, &p->m.ref) == 0)
			break;
	m = &p->m;
	hlist_for_each_entry_continue(m, all)
		if (text_update_following_after_change(t, &m->ref,
						       &start, &p->m.ref) == 0)
			break;

	mark_check_consistent(t);

	point_notify_change(p, t);
}

void point_delete_text(struct text *t, struct point *p, int len, int *first)
{
	struct mark *m;

	text_del(t, &p->m.ref, len, first);
	m = &p->m;
	hlist_for_each_entry_continue_reverse(m, &t->marks, all)
		if (text_update_prior_after_change(t, &m->ref,
						   &p->m.ref, &p->m.ref) == 0)
			break;
	m = &p->m;
	hlist_for_each_entry_continue(m, all)
		if (text_update_following_after_change(t, &m->ref,
						       &p->m.ref, &p->m.ref) == 0)
			break;

	mark_check_consistent(t);

	point_notify_change(p, t);
}

void point_undo(struct text *t, struct point *p, int redo)
{
	struct text_ref start, end;
	int did_do = 2;
	int first = 1;

	while (did_do != 1) {
		struct mark *m;
		int where;
		int i;

		if (redo)
			did_do = text_redo(t, &start, &end);
		else
			did_do = text_undo(t, &start, &end);
		if (did_do == 0)
			break;

		if (first) {
			/* Not nearby, look from the start */
			point_reset(t, p);
			where = 1;
			first = 0;
		} else
			where = text_locate(t, &p->m.ref, &end);
		if (!where)
			break;

		if (where == 1) {
			do {
				i = text_advance_towards(t, &p->m.ref, &end);
				if (i == 0)
					break;
				while ((m = next_mark(t, &p->m)) != NULL &&
				       m->ref.c == p->m.ref.c &&
				       m->ref.o < p->m.ref.o)
					fore_mark(&p->m, m);
			} while (i == 2);
		} else {
			do {
				i = text_retreat_towards(t, &p->m.ref, &end);
				if (i == 0)
					break;
				while ((m = prev_mark(t, &p->m)) != NULL &&
				       m->ref.c == p->m.ref.c &&
				       m->ref.o > p->m.ref.o)
					back_mark(&p->m, m);
			} while (i == 2);
		}

		if (!text_ref_same(t, &p->m.ref, &end))
			/* eek! */
			break;
		/* point is now at location of undo */

		m = &p->m;
		hlist_for_each_entry_continue_reverse(m, &t->marks, all)
			if (text_update_prior_after_change(t, &m->ref,
							   &start, &end) == 0)
				break;
		m = &p->m;
		hlist_for_each_entry_continue(m, all)
			if (text_update_following_after_change(t, &m->ref,
							       &start, &end) == 0)
				break;
		mark_check_consistent(t);
	}
	// notify marks of change

}

static void mark_check_consistent(struct text *t)
{
	/* Check consistency of marks, and abort if not.
	 * Check:
	 * - text itself is consistent
	 * - every mark points to a valid chunk with valid offset
	 * - all marks are in seq order and text order
	 * - Maybe should check various mark lists too.
	 */
	struct mark *m, *prev;
	int seq = 0;
	int i;
	text_check_consistent(t);
	hlist_for_each_entry(m, &t->marks, all)
		text_ref_consistent(t, &m->ref);
	hlist_for_each_entry(m, &t->marks, all) {
		if (m->seq < seq)
			abort();
		seq = m->seq + 1;
	}
	prev = NULL;
	hlist_for_each_entry(m, &t->marks, all) {
		if (prev) {
			struct text_ref r = prev->ref;
			int i;
			while ((i = text_advance_towards(t, &r, &m->ref)) != 1) {
				if (i == 0)
					abort();
			}
		}
		prev = m;
	}
	for (i = 0; i < t->ngroups; i++)
		if (t->groups[i].notify == NULL) {
			if (!tlist_empty(&t->groups[i].head)) abort();
		} else {
			struct tlist_head *tl;
			struct point *p;
			seq = 0;
			tlist_for_each(tl, &t->groups[i].head) {
				switch(TLIST_TYPE(tl)) {
				case GRP_HEAD: abort();
				case GRP_MARK:
					m = container_of(tl, struct mark, group);
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
