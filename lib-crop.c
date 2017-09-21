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

static int in_range(struct mark *m, struct crop_data *cd safe, struct pane *p safe)
{
	if (!m)
		/* NULL is always in range */
		return 1;
	if (m->seq >= cd->start->seq && m->seq <= cd->end->seq)
		return 1;
	/* I think I want strict ordering at the point, so don't test mark_same */
	return 0;
}

static int crop(struct mark *m, struct crop_data *cd safe, struct pane *p safe)
{
	/* If mark is outside of range, move it back, and report if more was
	 * required than just updating the ->seq
	 */
	if (!m || in_range(m, cd, p))
		return 0;

	if (m->seq < cd->start->seq) {
		if (mark_same_pane(p, m, cd->start)) {
			mark_to_mark(m, cd->start);
			return 0;
		}
		mark_to_mark(m, cd->start);
	}
	if (m->seq > cd->end->seq) {
		if (mark_same_pane(p, m, cd->end)) {
			mark_to_mark(m, cd->end);
			return 0;
		}
		mark_to_mark(m, cd->end);
	}
	return 1;
}

DEF_CMD(crop_handle)
{
	struct pane *p = ci->home->parent;
	struct crop_data *cd = ci->home->data;
	int ret;

	if (strcmp(ci->key, "Close") == 0) {
		mark_free(cd->start);
		mark_free(cd->end);
		free(cd);
		return 1;
	}
	if (!p)
		return 0;

	if (strcmp(ci->key, "doc:write-file") == 0)
		return call_home(p, ci->key, ci->focus, ci->numeric,
				 ci->mark ?: cd->start,
				 ci->str, ci->extra,
				 ci->mark2 ?: cd->end, ci->str2,
				 ci->comm2);

	if (!ci->mark && !ci->mark2)
		/* No mark, do give it straight to parent */
		return comm_call_pane(p, ci->key, ci->focus, ci->numeric,
				      NULL, ci->str, ci->extra, NULL, ci->comm2);

	/* Always force marks to be in range */
	crop(ci->mark, cd, p);
	crop(ci->mark2, cd, p);

	ret = comm_call_pane(p, ci->key, ci->focus, ci->numeric,
				      ci->mark, ci->str, ci->extra, ci->mark2, ci->comm2);
	if (crop(ci->mark, cd, p) || crop(ci->mark2, cd, p)) {
		if (strcmp(ci->key, "doc:step") == 0)
			ret = CHAR_RET(WEOF);
		else if (strcmp(ci->key, "doc:set-ref") != 0)
			ret = -1;
	}
	if (strcmp(ci->key, "doc:step")==0 && ci->extra == 0 && ci->mark) {
		if (ci->numeric) {
			if (mark_same_pane(p, ci->mark, cd->end))
				ret = CHAR_RET(WEOF);
		} else {
			if (mark_same_pane(p, ci->mark, cd->start))
				ret = CHAR_RET(WEOF);
		}
	}
	return ret;
}

DEF_CMD(crop_attach)
{
	struct pane *p;
	struct crop_data *cd;

	if (!ci->mark || !ci->mark2)
		return -1;
	cd = calloc(1, sizeof(*cd));
	p = pane_register(ci->focus, 0, &crop_handle, cd, NULL);
	if (!p) {
		free(cd);
		return -1;
	}
	call("doc:set:filter", p, 1, NULL, NULL, 0);
	cd->start = mark_dup(ci->mark, 1);
	cd->end = mark_dup(ci->mark2, 1);

	return comm_call(ci->comm2, "callback:attach", p);
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, 0, NULL, "attach-crop",
		  &crop_attach);
}
