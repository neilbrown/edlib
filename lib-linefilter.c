/*
 * Copyright Neil Brown ©2015-2020 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * line-filter: hide (un)selected lines from display
 *
 * This can be layered over render-format or similar and will restrict
 * which lines are shown, based on some attribute visible at the start
 * of the line.
 *
 * This module doesn't hold any marks on any document.  The marks
 * held by the rendered should be sufficient.
 */

#define _GNU_SOURCE for strcasestr
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "core.h"
#include "misc.h"

struct filter_data {
	char *match;
	int match_len;
	char *attr;
	bool at_start;
	bool ignore_case;
	bool explicit_set;
};

struct rlcb {
	struct command c;
	struct filter_data *fd;
	int keep, cmp;
	const char *str;
};

DEF_CMD(rlcb)
{
	struct rlcb *cb = container_of(ci->comm, struct rlcb, c);
	char *c = ci->str ? strdup(ci->str) : NULL;
	if (c && ci->num2 != -1) {
		int i;
		for (i = 0; c[i] ; i++) {
			if (c[i] == '<')
				memmove(c+i, c+i+1, strlen(c+i));
		}
	}
	if (cb->fd == NULL)
		cb->cmp = 0; /* Don't compare, just save */
	else if (c == NULL)
		cb->cmp = -1;
	else if (cb->fd->at_start && cb->fd->ignore_case)
		cb->cmp = strncasecmp(c, cb->fd->match, cb->fd->match_len);
	else if (cb->fd->at_start)
		cb->cmp = strncasecmp(c, cb->fd->match, cb->fd->match_len);
	else if (cb->fd->ignore_case)
		cb->cmp = strcasestr(c, cb->fd->match) ? 0 : 1;
	else
		cb->cmp = strstr(c, cb->fd->match) ? 0 : 1;

	if (cb->cmp == 0 && cb->keep && !cb->str && c && ci->str) {
		if (cb->keep > 1)
			/* Want the original with markup */
			strcpy(c, ci->str);
		cb->str = c;
	} else
		free(c);
	return 1;
}

DEF_CMD(render_filter_line)
{
	/* Skip any line that doesn't match, and
	 * return a highlighted version of the first one
	 * that does.
	 * Then skip over any other non-matches.
	 */
	struct filter_data *fd = ci->home->data;
	struct rlcb cb;
	int ret;
	struct mark *m;
	struct mark *m2;

	if (!ci->mark)
		return Enoarg;

	m = mark_dup(ci->mark);
	cb.c = rlcb;
	cb.fd = fd;
	cb.str = NULL;
	cb.keep = 0;
	cb.cmp = 0;

	do {
		mark_to_mark(ci->mark, m);
		cb.cmp = 0;
		if (fd->attr) {
			comm_call(&cb.c, "", ci->focus,
				  NO_NUMERIC, NULL,
				  pane_mark_attr(ci->focus, m, fd->attr),
				  -1);
			home_call(ci->home->parent, ci->key, ci->focus,
				  NO_NUMERIC, m);
		} else
			home_call_comm(ci->home->parent, ci->key, ci->focus,
				       &cb.c, NO_NUMERIC, m);
	} while (cb.cmp && !mark_same(ci->mark, m));

	mark_free(m);
	cb.keep = 2;
	cb.str = NULL;
	cb.fd = NULL;
	m2 = ci->mark2;
	if (home_call_comm(ci->home->parent, ci->key, ci->focus, &cb.c,
			   ci->num, ci->mark, NULL, 0, m2) < 0)
		return Efail;

	ret = comm_call(ci->comm2, "callback:render", ci->focus, 0, NULL, cb.str);
	free((void*)cb.str);
	if (ci->num != NO_NUMERIC)
		/* Was rendering to find a cursor, don't need to skip */
		return ret;
	/* Need to continue over other non-matching lines */
	m = mark_dup(ci->mark);
	cb.keep = 0;
	cb.fd = fd;
	do {
		/* have a non-match, so move the mark over it. */
		mark_to_mark(ci->mark, m);
		cb.cmp = 0;
		if (fd->attr) {
			comm_call(&cb.c, "", ci->focus,
				  NO_NUMERIC, NULL,
				  pane_mark_attr(ci->focus, m, fd->attr),
				  -1);
			home_call(ci->home->parent, ci->key, ci->focus,
				  NO_NUMERIC, m);
		} else
			home_call_comm(ci->home->parent, ci->key, ci->focus,
				       &cb.c, NO_NUMERIC, m);
	} while (cb.cmp && !mark_same(ci->mark, m));

	mark_free(m);
	return ret ?: 1;
}

static int do_filter_line_prev(struct filter_data *fd safe,
			       struct mark *m safe,
			       struct pane *home safe,
			       struct pane *focus safe, int n,
			       const char **savestr)
{
	/* Move to start of this or previous real line and
	 * check if it passes the filter.
	 * If we get an error, return that. else 0 if it doesn't
	 * pass, else 1.
	 */
	struct rlcb cb;
	int ret;

	if (savestr)
		*savestr = NULL;

	cb.c = rlcb;
	cb.str = NULL;
	cb.fd = fd;
	cb.cmp = 0;

	ret = home_call(home, "doc:render-line-prev", focus, n, m);
	if (ret < 0)
		/* Probably hit start-of-file */
		return ret;

	/* we must be looking at a possible option for the previous line */
	cb.keep = !!savestr;
	cb.str = NULL;
	if (fd->attr) {
		comm_call(&cb.c, "", focus,
			  NO_NUMERIC, NULL,
			  pane_mark_attr(focus, m, fd->attr), -1);
	} else {
		struct mark *m2 = mark_dup(m);

		ret = home_call_comm(home, "doc:render-line", focus, &cb.c,
				     NO_NUMERIC, m2);
		mark_free(m2);
		if (ret <= 0)
			return Efail;
	}
	if (savestr)
		*savestr = cb.str;

	return cb.cmp == 0;
}

DEF_CMD(render_filter_prev)
{
	struct filter_data *fd = ci->home->data;
	struct mark *m = ci->mark;
	int ret;

	if (!m)
		return Enoarg;
	if (!fd->match)
		return Efallthrough;

	/* First, make sure we are at a start-of-line */
	ret = do_filter_line_prev(fd, m, ci->home->parent, ci->focus, 0, NULL);
	if (ret < 0)
		return ret;
	while (ret == 0) {
		/* That wasn't a matching line, try again */
		ret = do_filter_line_prev(fd, m, ci->home->parent, ci->focus,
					  1, NULL);
	}
	if (!ci->num)
		/* Only wanted start of line - found */
		return 1;
	ret = 0;
	while (ret == 0) {
		ret = do_filter_line_prev(fd, m, ci->home->parent, ci->focus,
					  1, NULL);
		if (ret < 0)
			/* Error */
			return ret;
	}
	return ret;
}

DEF_CMD(filter_changed)
{
	/* Update match info from arg (Filter:set) or pane attrs (unless
	 * Filter:set has been used), then walk over a range of marks
	 * calling Notify:clip as needed, and for Filter:set, call comm2
	 * with the matching string.
	 *
	 * If no marks are given, we walk entire doc.
	 * otherwise find first visible line after all marks, and last before,
	 * and walk that range
	 */
	struct filter_data *fd = ci->home->data;
	struct mark *start, *end, *m;
	struct command *comm = NULL;
	bool found_one = False;

	if (strcmp(ci->key, "Filter:set") == 0) {
		if (!ci->str)
			return Enoarg;
		pane_damaged(ci->home, DAMAGED_CONTENT);
		comm = ci->comm2;
		fd->explicit_set = True;
		free(fd->match);
		free(fd->attr);
		fd->match = strdup(ci->str);
		fd->match_len = strlen(fd->match);
		fd->attr = ci->str2 ? strdup(ci->str2) : NULL;
		fd->at_start = !!(ci->num & 1);
		fd->ignore_case = !!(ci->num & 2);
	}
	if (!fd->explicit_set) {
		char *s;
		s = pane_attr_get(ci->focus, "filter:match");
		if (s && (!fd->match || strcmp(s, fd->match) != 0)) {
			free(fd->match);
			fd->match = strdup(s);
			fd->match_len = strlen(s);
		}
		s = pane_attr_get(ci->focus, "filter:attr");
		if (!s || !fd->attr || strcmp(s, fd->attr) != 0) {
			free(fd->attr);
			fd->attr = s ? strdup(s) : NULL;
		}
		s = pane_attr_get(ci->focus, "filter:at_start");
		fd->at_start = s && *s && strchr("Yy1Tt", *s) != NULL;
		s = pane_attr_get(ci->focus, "filter:ignore_case");
		fd->ignore_case = s && *s && strchr("Yy1Tt", *s) != NULL;
	}
	if (!fd->match)
		return 0;

	start = vmark_new(ci->focus, MARK_UNGROUPED, NULL);
	if (!start)
		return Efail;
	if (ci->mark && (!ci->mark2 || ci->mark2->seq > ci->mark->seq))
		/* mark is first */
		mark_to_mark(start, ci->mark);
	else if (ci->mark2)
		/* mark2 is first */
		mark_to_mark(start, ci->mark2);
	else if (strcmp(ci->key, "Filter:set") == 0)
		call("Move-File", ci->focus, 0, start);
	else {
		struct mark *m2;
		m = start;
		while ((m2 = mark_prev(m)) != NULL)
			m = m2;
		mark_to_mark(start, m);
	}

	end = vmark_new(ci->focus, MARK_UNGROUPED, NULL);
	if (!end) {
		mark_free(start);
		return Efail;
	}
	if (ci->mark && (!ci->mark2 || ci->mark2->seq < ci->mark->seq))
		/* mark is last */
		mark_to_mark(end, ci->mark);
	else if (ci->mark2)
		/* mark2 is last */
		mark_to_mark(end, ci->mark2);
	else if (strcmp(ci->key, "Filter:set") == 0)
		call("Move-File", ci->focus, 1, end);
	else {
		struct mark *m2;
		m = end;
		while ((m2 = mark_next(m)) != NULL)
			m = m2;
		mark_to_mark(end, m);
	}

	if (call("doc:render-line", ci->focus, NO_NUMERIC, end) > 0)
		found_one = True;

	m = mark_dup(end);
	while (m->seq > start->seq || !found_one) {
		int ret;
		const char *str = NULL;
		struct mark *m2 = mark_dup(m);

		ret = do_filter_line_prev(fd, m, ci->home->parent, ci->focus, 1,
				     comm ? &str : NULL);
		if (ret > 0) {
			/* m is a good line, m2 is like end */
			found_one = True;
			if (!mark_same(m2, end))
				call("Notify:clip", ci->focus, 0, m2, NULL,
				     0, end);
			mark_to_mark(end, m);
			if (comm && str)
				comm_call(comm, "", ci->focus, 0, m, str);
		}
		free((void*)str);
		mark_free(m2);
		if (ret < 0)
			break;
	}
	/* No matching lines found between m and end, so clip them */
	if (!mark_same(m, end))
		call("Notify:clip", ci->focus, 0, m, NULL, 0, end);
	mark_free(m);
	mark_free(start);
	mark_free(end);
	if (!found_one)
		/* filtered document is now empty - maybe someone cares */
		home_call(ci->focus, "Notify:filter:empty", ci->home);
	if (strcmp(ci->key, "view:changed") == 0 ||
	    strcmp(ci->key, "Refresh:view") == 0)
		return 0;
	return 1;
}

DEF_CMD(filter_nomove)
{
	if (strcmp(ci->key, "Move-File") == 0)
		return 0;
	if (strcmp(ci->key, "Move-to") == 0)
		return 0;
	if (strcmp(ci->key, "Move-Line") == 0)
		return 0;
	return 1;
}

DEF_CMD(eol_cb)
{
	/* don't save anything */
	return 1;
}

DEF_CMD(filter_eol)
{
	int rpt = RPT_NUM(ci);

	if (!ci->mark)
		return Enoarg;
	if (rpt >= -1 && rpt <= 1)
		/* movement within the line */
		return 1;
	while (rpt < -1) {
		int ret;
		ret = do_filter_line_prev(ci->home->data, ci->mark,
					  ci->home->parent, ci->focus, 1, NULL);
		if (ret < 0)
			rpt = -1;
		if (ret > 0)
			rpt += 1;
	}
	while (rpt > 1) {
		struct call_return cr;
		cr.c = eol_cb;
		if (home_call(ci->home, "doc:render-line",
			      ci->focus, NO_NUMERIC, ci->mark, NULL,
			      0, NULL, NULL, 0,0, &cr.c) <= 0)
			rpt = 1;
		rpt -= 1;
	}
	return 1;
}

DEF_CMD(filter_damaged)
{
	pane_damaged(ci->home, DAMAGED_VIEW);
	return 1;
}

DEF_CMD(filter_attach);
DEF_CMD(filter_clone)
{
	struct pane *parent = ci->focus;

	filter_attach.func(ci);
	pane_clone_children(ci->home, parent->focus);
	return 1;
}

static struct map *filter_map;
DEF_LOOKUP_CMD(filter_handle, filter_map);

static void filter_register_map(void)
{
	if (filter_map)
		return;

	filter_map = key_alloc();
	key_add(filter_map, "doc:render-line", &render_filter_line);
	key_add(filter_map, "doc:render-line-prev", &render_filter_prev);
	key_add(filter_map, "Free", &edlib_do_free);
	key_add(filter_map, "Clone", &filter_clone);
	key_add_prefix(filter_map, "Move-", &filter_nomove);
	key_add(filter_map, "Move-EOL", &filter_eol);
	key_add(filter_map, "Filter:set", &filter_changed);
	key_add(filter_map, "view:changed", &filter_changed);
	key_add(filter_map, "Refresh:view", &filter_changed);
	key_add(filter_map, "doc:replaced", &filter_damaged);
}

REDEF_CMD(filter_attach)
{
	struct pane *filter;
	struct filter_data *fd;

	filter_register_map();
	alloc(fd, pane);

	filter = pane_register(ci->focus, 0, &filter_handle.c, fd);
	if (!filter) {
		unalloc(fd, pane);
		return Efail;
	}
	pane_damaged(filter, DAMAGED_VIEW);
	call("doc:request:doc:replaced", filter);

	return comm_call(ci->comm2, "", filter);
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, &filter_attach,
		  0, NULL, "attach-linefilter");
}
