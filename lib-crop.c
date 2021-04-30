/*
 * Copyright Neil Brown ©2016-2021 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * lib-crop: limit access to a range within a document.
 * Given two marks which refer to the parent document, we
 * pass on any commands without marks, or with marks in the
 * given range.  If either mark is moved beyond the range, we move
 * it back to the boundary and fail the request.
 */

#include <stdlib.h>
#include <string.h>
#include "core.h"

struct crop_data {
	struct mark *start safe;
	struct mark *end safe;
};

static bool in_range(struct mark *m, struct crop_data *cd safe)
{
	if (!m)
		/* NULL is always in range */
		return True;
	if (m->seq >= cd->start->seq && m->seq <= cd->end->seq)
		return True;
	/* I think I want strict ordering at the point, so don't test mark_same */
	return False;
}

static bool crop(struct mark *m, struct crop_data *cd safe)
{
	/* If mark is outside of range, move it back, and report if more was
	 * required than just updating the ->seq
	 */
	if (!m || in_range(m, cd))
		return False;

	if (m->seq < cd->start->seq) {
		if (mark_same(m, cd->start)) {
			mark_to_mark(m, cd->start);
			return False;
		}
		mark_to_mark(m, cd->start);
	}
	if (m->seq > cd->end->seq) {
		if (mark_same(m, cd->end)) {
			mark_to_mark(m, cd->end);
			return False;
		}
		mark_to_mark(m, cd->end);
	}
	return True;
}

DEF_CMD(crop_close)
{
	struct crop_data *cd = ci->home->data;

	mark_free(cd->start);
	mark_free(cd->end);
	return 1;
}

DEF_CMD(crop_write)
{
	struct pane *p = ci->home->parent;
	struct crop_data *cd = ci->home->data;

	return home_call(p, ci->key, ci->focus, ci->num,
			 ci->mark ?: cd->start,
			 ci->str, ci->num2,
			 ci->mark2 ?: cd->end, ci->str2,
			 0,0, ci->comm2);
}

static int crop_step(struct pane *home safe, struct mark *mark safe,
		     int num, int num2)
{
	struct pane *p = home->parent;
	struct crop_data *cd = home->data;
	int ret;

	/* Always force marks to be in range */
	crop(mark, cd);

	ret = home_call(p, "doc:char", home,
			num2 ? (num ? 1 : -1) : 0,
			mark, NULL,
			num2 ? 0 : (num ? 1 : -1));
	if (crop(mark, cd))
		ret = CHAR_RET(WEOF);

	if (num2 == 0) {
		if (num) {
			if (mark_same(mark, cd->end))
				ret = CHAR_RET(WEOF);
		} else {
			if (mark_same(mark, cd->start))
				ret = CHAR_RET(WEOF);
		}
	}
	return ret;
}

DEF_CMD(crop_char)
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
	while (steps && ret != CHAR_RET(WEOF) && (!end || mark_same(m, end))) {
		ret = crop_step(ci->home, m, forward, 1);
		steps -= forward*2 - 1;
	}
	if (end)
		return 1 + (forward ? ci->num - steps : steps - ci->num);
	if (ret == CHAR_RET(WEOF) || ci->num2 == 0)
		return ret;
	if (ci->num && (ci->num2 < 0) == forward)
		return ret;
	/* Want the 'next' char */
	return crop_step(ci->home, m, ci->num2 > 0, 0);
}

DEF_CMD(crop_clip)
{
	struct crop_data *cd = ci->home->data;

	mark_clip(cd->start, ci->mark, ci->mark2, !!ci->num);
	mark_clip(cd->end, ci->mark, ci->mark2, !!ci->num);
	return Efallthrough;
}

DEF_CMD(crop_content)
{
	struct crop_data *cd = ci->home->data;
	struct mark *m2;
	int ret;

	crop(ci->mark, cd);
	crop(ci->mark2, cd);
	if (ci->mark2)
		m2 = ci->mark2;
	else
		m2 = mark_dup(cd->end);
	ret = home_call_comm(ci->home->parent, ci->key, ci->home,
			     ci->comm2, 0, ci->mark, NULL, 0, m2);
	if (m2 != ci->mark2)
		mark_free(m2);
	return ret;
}

DEF_CMD(crop_generic)
{
	struct pane *p = ci->home->parent;
	struct crop_data *cd = ci->home->data;
	int ret;

	if (!ci->mark && !ci->mark2)
		/* No mark, do give it straight to parent */
		return home_call(p, ci->key, ci->focus, ci->num,
				 NULL, ci->str, ci->num2, NULL, ci->str2,
				 0,0, ci->comm2);

	/* Always force marks to be in range */
	crop(ci->mark, cd);
	crop(ci->mark2, cd);

	ret = home_call(p, ci->key, ci->focus, ci->num,
			ci->mark, ci->str, ci->num2, ci->mark2, ci->str2, 0,0, ci->comm2);
	if (crop(ci->mark, cd) || crop(ci->mark2, cd)) {
		if (strcmp(ci->key, "doc:set-ref") != 0)
			ret = Einval;
	}
	return ret;
}

static struct map *crop_map safe;
DEF_LOOKUP_CMD(crop_handle, crop_map);

DEF_CMD(crop_attach)
{
	struct pane *p;
	struct crop_data *cd;

	if (!ci->mark || !ci->mark2)
		return Enoarg;
	alloc(cd, pane);
	p = pane_register(ci->focus, 0, &crop_handle.c, cd);
	if (!p) {
		free(cd);
		return Efail;
	}
	cd->start = mark_dup(ci->mark);
	cd->end = mark_dup(ci->mark2);

	return comm_call(ci->comm2, "callback:attach", p);
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, &crop_attach, 0, NULL, "attach-crop");
	if ((void*)crop_map)
		return;
	crop_map = key_alloc();
	key_add_prefix(crop_map, "doc:", &crop_generic);
	key_add(crop_map, "Close", &crop_close);
	key_add(crop_map, "Free", &edlib_do_free);
	key_add(crop_map, "doc:write_file", &crop_write);
	key_add(crop_map, "doc:char", &crop_char);
	key_add(crop_map, "doc:content", &crop_content);
	key_add(crop_map, "Notify:clip", &crop_clip);
}
