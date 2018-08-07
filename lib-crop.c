/*
 * Copyright Neil Brown ©2016 <neil@brown.name>
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

static int in_range(struct mark *m, struct crop_data *cd safe)
{
	if (!m)
		/* NULL is always in range */
		return 1;
	if (m->seq >= cd->start->seq && m->seq <= cd->end->seq)
		return 1;
	/* I think I want strict ordering at the point, so don't test mark_same */
	return 0;
}

static int crop(struct mark *m, struct crop_data *cd safe)
{
	/* If mark is outside of range, move it back, and report if more was
	 * required than just updating the ->seq
	 */
	if (!m || in_range(m, cd))
		return 0;

	if (m->seq < cd->start->seq) {
		if (mark_same(m, cd->start)) {
			mark_to_mark(m, cd->start);
			return 0;
		}
		mark_to_mark(m, cd->start);
	}
	if (m->seq > cd->end->seq) {
		if (mark_same(m, cd->end)) {
			mark_to_mark(m, cd->end);
			return 0;
		}
		mark_to_mark(m, cd->end);
	}
	return 1;
}

DEF_CMD(crop_close)
{
	struct crop_data *cd = ci->home->data;

	mark_free(cd->start);
	mark_free(cd->end);
	free(cd);
	return 1;
}

DEF_CMD(crop_write)
{
	struct pane *p = ci->home->parent;
	struct crop_data *cd = ci->home->data;

	if (!p)
		return 0;

	return home_call(p, ci->key, ci->focus, ci->num,
			 ci->mark ?: cd->start,
			 ci->str, ci->num2,
			 ci->mark2 ?: cd->end, ci->str2,
			 0,0, ci->comm2);
}

DEF_CMD(crop_step)
{
	struct pane *p = ci->home->parent;
	struct crop_data *cd = ci->home->data;
	int ret;

	if (!p)
		return 0;

	if (!ci->mark && !ci->mark2)
		return 0;

	/* Always force marks to be in range */
	crop(ci->mark, cd);
	crop(ci->mark2, cd);

	ret = home_call(p, ci->key, ci->focus, ci->num,
			ci->mark, ci->str, ci->num2, ci->mark2, ci->str2, 0,0, ci->comm2);
	if (crop(ci->mark, cd) || crop(ci->mark2, cd))
		ret = CHAR_RET(WEOF);

	if (ci->num2 == 0 && ci->mark) {
		if (ci->num) {
			if (mark_same(ci->mark, cd->end))
				ret = CHAR_RET(WEOF);
		} else {
			if (mark_same(ci->mark, cd->start))
				ret = CHAR_RET(WEOF);
		}
	}
	return ret;
}

DEF_CMD(crop_clip)
{
	struct crop_data *cd = ci->home->data;

	mark_clip(cd->start, ci->mark, ci->mark2);
	mark_clip(cd->end, ci->mark, ci->mark2);
	return 0;
}

DEF_CMD(crop_generic)
{
	struct pane *p = ci->home->parent;
	struct crop_data *cd = ci->home->data;
	int ret;

	if (!p)
		return 0;

	if (!ci->mark && !ci->mark2)
		/* No mark, do give it straight to parent */
		return home_call(p, ci->key, ci->focus, ci->num,
				 NULL, ci->str, ci->num2, NULL, NULL, 0,0, ci->comm2);

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
	cd = calloc(1, sizeof(*cd));
	p = pane_register(ci->focus, 0, &crop_handle.c, cd, NULL);
	if (!p) {
		free(cd);
		return Esys;
	}
	call("doc:set:filter", p, 1);
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
	key_add_range(crop_map, "doc:", "doc;", &crop_generic);
	key_add(crop_map, "Close", &crop_close);
	key_add(crop_map, "doc:write_file", &crop_write);
	key_add(crop_map, "doc:step", &crop_step);
	key_add(crop_map, "Notify:clip", &crop_clip);
}
