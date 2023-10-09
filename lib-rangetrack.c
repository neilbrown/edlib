/*
 * Copyright Neil Brown ©2023 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * rangetrack: track ranges of a document which have been processed
 * in some why, such a spell-check or syntax-highlight or other
 * parsing.
 *
 * rangetrack will attach a pane to the target document to store
 * marks and other state.  It can track an arbitrary set of different
 * range types.
 *
 * rangetrack:new : start tracking ranges on 'focus' document.
 *		str is the name of the range set.
 * rangetrack:add     : record that mark to mark2 are a valid range
 * rangetrack:remove  : record that from mark to mark2 are no longer valid
 * rangetrack:choose  : report a subrange for mark..mark2 which is not
 *			currently valid.
 *
 */

#define PANE_DATA_TYPE struct rangetrack_data

#include "core.h"
struct rangetrack_data {
	struct rci {
		const char *set safe;
		int view;
		struct rci *next;
	} *info;
};
#include "core-pane.h"

static struct rci *find_set(const struct cmd_info *ci safe)
{
	struct rangetrack_data *rtd = ci->home->data;
	struct rci *i;

	for (i = rtd->info; i; i = i->next) {
		if (ci->str && strcmp(ci->str, i->set) == 0)
			return i;
	}
	return NULL;
}

static void add_set(struct pane *home safe, const char *set safe)
{
	struct rangetrack_data *rtd = home->data;
	struct rci *i;

	alloc(i, pane);
	i->set = strdup(set);
	i->view = call("doc:add-view", home) - 1;
	i->next = rtd->info;
	rtd->info = i;
}

DEF_CMD_CLOSED(rangetrack_close)
{
	struct rangetrack_data *rtd = ci->home->data;
	struct rci *i;

	while ((i = rtd->info) != NULL) {
		rtd->info = i->next;
		free((void*)i->set);
		unalloc(i, pane);
	}
	return 1;
}

DEF_CMD(rangetrack_new)
{
	struct rci *i = find_set(ci);

	if (!ci->str)
		return Enoarg;
	if (i)
		return Efalse;
	add_set(ci->home, ci->str);
	return 1;
}

DEF_CMD(rangetrack_add)
{
	struct rci *i = find_set(ci);
	struct mark *start = ci->mark;
	struct mark *end = ci->mark2;
	struct mark *m, *m1, *m2;

	if (!i)
		return Efalse;
	if (!start || !end)
		/* Testing if configured already */
		return 1;
	m1 = vmark_at_or_before(ci->home, start, i->view, ci->home);
	if (m1 && attr_find(m1->attrs, "start"))
		m1 = vmark_next(m1);
	else if (m1 && mark_same(m1, start))
		/* m1 is an end-of-range. Can move m1 down to cover range */
		;
	else
		/* Must create a new mark, or move a later mark up */
		m1 = NULL;
	m2 = vmark_at_or_before(ci->home, end, i->view, ci->home);
	if (m2 && attr_find(m2->attrs, "start") == NULL) {
		if (mark_same(m2, end))
			/* Can move the start of this range earlier */
			m2 = vmark_prev(m2);
		else
			/* end not in rnage, must create mark or move
			 * earlier mark down
			 */
			m2 = NULL;
	}
	/* If m2, then move it backwards - no need to create */
	if (!m1 && !m2) {
		/* no overlaps, create a new region */
		m1 = vmark_new(ci->home, i->view, ci->home);
		if (!m1)
			return Efail;
		mark_to_mark(m1, start);
		m2 = vmark_new(ci->home, i->view, ci->home);
		if (!m2)
			return Efail;
		mark_to_mark(m2, end);
		attr_set_str(&m1->attrs, "start", "yes");
	} else if (m1 && !m2) {
		/* Can move m1 dow n to end, removing anything in the way */
		m = vmark_next(m1);
		while (m && mark_ordered_or_same(m, end)) {
			mark_free(m);
			m = vmark_next(m1);
		}
		mark_to_mark(m1, end);
	} else if (!m1 && m2) {
		/* Can move m2 up to start, removing things */
		m = vmark_prev(m2);
		while (m && mark_ordered_or_same(start, m)) {
			mark_free(m);
			m = vmark_prev(m2);
		}
		mark_to_mark(m2, start);
	} else if (m1 && m2) {
		/* Can remove all from m1 to m2 inclusive */
		while (m1 && mark_ordered_not_same(m1, m2)) {
			m = vmark_next(m1);
			mark_free(m1);
			m1 = m;
		}
		mark_free(m2);
	}
	return 1;
}

DEF_CMD(rangetrack_clear)
{
	struct rci *i = find_set(ci);
	struct mark *start = ci->mark;
	struct mark *end = ci->mark2;
	struct mark *m1, *m2;

	if (!i)
		return Efalse;
	if (!start || !end) {
		start = vmark_first(ci->home, i->view, ci->home);
		end = vmark_last(ci->home, i->view, ci->home);
	}
	if (!start || !end)
		return 1;

	m1 = vmark_at_or_before(ci->home, start, i->view, ci->home);

	if (!m1 || attr_find(m1->attrs, "start") == NULL) {
		/* Immediately after start is not active, so the
		 * earlierst we might need to remove is the next
		 * mark, or possibly the very first mark.
		 */
		if (m1)
			m1 = vmark_next(m1);
		else
			m1 = vmark_first(ci->home, i->view, ci->home);
		if (!m1 || mark_ordered_or_same(end, m1))
			/* Nothing to remove */
			return 1;
	} else {
		/* From m1 to start are in a range and should stay
		 * there.  Split the range from 'm1' at 'start'
		 */
		m1 = vmark_new(ci->home, i->view, ci->home);
		if (!m1)
			return Efail;
		mark_to_mark(m1, start);
		m1 = mark_dup_view(m1);
		/* Ensure this m1 is after the previous one */
		mark_step(m1, 1);
		attr_set_str(&m1->attrs, "start", "yes");
	}
	/* m is now the start of an active section that is within
	 * start-end and should be removed */
	m2 = vmark_at_or_before(ci->home, end, i->view, ci->home);
	if (m2 && mark_same(m2, end) && attr_find(m2->attrs, "start"))
		/* This section is entirely after end, so not interesting */
		m2 = vmark_prev(m2);
	if (m2 && attr_find(m2->attrs, "start")) {
		/* end is within an active secion that needs to be split */
		m2 = vmark_new(ci->home, i->view, ci->home);
		if (!m2)
			return Efail;
		mark_to_mark(m2, end);
		attr_set_str(&m2->attrs, "start", "yes");
		m2 = mark_dup_view(m2);
		mark_step(m2, 0);
	}
	if (!m2)
		return 1;
	/* m2 is now the end of an active section that needs to bie discarded */
	while (m1 && mark_ordered_not_same(m1, m2)) {
		struct mark *m = m1;
		m1 = vmark_next(m1);
		mark_free(m);
	}
	mark_free(m2);
	call(strconcat(ci->home, "doc:notify:rangetrack:recheck-", i->set),
	     ci->home);
	return 1;
}

DEF_CMD(rangetrack_choose)
{
	struct rci *i = find_set(ci);
	struct mark *start = ci->mark;
	struct mark *end = ci->mark2;
	struct mark *m1, *m2;

	if (!i)
		return Efail;
	if (!start || !end)
		return Enoarg;
	/* Contract start-end so that none of it is in-range */
	m1 = vmark_at_or_before(ci->home, start, i->view, ci->home);
	if (m1 && attr_find(m1->attrs, "start") == NULL)
		/* Start is not in-range, end must not exceed m1 */
		m2 = vmark_next(m1);
	else if (m1) {
		/* m1 is in-range, move it forward */
		m1 = vmark_next(m1);
		if (m1) {
			mark_to_mark(start, m1);
			m2 = vmark_next(m1);
		} else {
			/* Should be impossible */
			m2 = start;
		}
	} else {
		/* Start is before all ranges */
		m2 = vmark_first(ci->home, i->view, ci->home);
	}
	if (m2 && mark_ordered_not_same(m2, end))
		mark_to_mark(end, m2);
	return 1;
}

static struct map *rangetrack_map safe;
DEF_LOOKUP_CMD(rangetrack_handle, rangetrack_map);

DEF_CMD(rangetrack_attach)
{
	struct pane *doc = call_ret(pane, "doc:get-doc", ci->focus);
	const char *set = ci->str;
	struct pane *p;

	if (!set)
		return Enoarg;
	if (!doc)
		return Efail;
	if (call("doc:notify:rangetrack:new", ci->focus, 0, NULL, set) > 0)
		return 1;
	p = pane_register(doc, 0, &rangetrack_handle.c);
	if (!p)
		return Efail;
	pane_add_notify(p, doc, "rangetrack:new");
	pane_add_notify(p, doc, "rangetrack:add");
	pane_add_notify(p, doc, "rangetrack:clear");
	pane_add_notify(p, doc, "rangetrack:choose");
	add_set(p, set);
	return 1;
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, &rangetrack_attach,
		  0, NULL, "rangetrack:new");
	rangetrack_map = key_alloc();
	key_add(rangetrack_map, "Close", &rangetrack_close);
	key_add(rangetrack_map, "rangetrack:new", &rangetrack_new);
	key_add(rangetrack_map, "rangetrack:add", &rangetrack_add);
	key_add(rangetrack_map, "rangetrack:clear", &rangetrack_clear);
	key_add(rangetrack_map, "rangetrack:choose", &rangetrack_choose);
}
