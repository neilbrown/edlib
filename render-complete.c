/*
 * Copyright Neil Brown <neil@brown.name> 2015
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * render-complete - support string completion.
 *
 * This should be attached between render-lines and the pane which
 * provides the lines.  It is given a prefix and it suppresses all
 * lines which start with the prefix.
 * All events are redirected to the controlling window (where the text
 * to be completed is being entered)
 *
 * This module doesn't hold any marks on any document.  The marks
 * held by the rendered should be sufficient.
 */

#include <stdlib.h>
#include <string.h>
#include "core.h"

struct complete_data {
	char *prefix;
};

DEF_CMD(render_complete_line)
{
	/* The first line *must* match the prefix.
	 * skip over any following lines that don't
	 */
	struct cmd_info ci2 = {0};
	struct complete_data *cd = ci->home->data;
	struct doc *d = doc_from_pane(ci->home);
	int plen;

	if (!d || !ci->mark)
		return -1;

	ci2.key = ci->key;
	ci2.mark = ci->mark;
	ci2.mark2 = ci->mark2;
	ci2.focus = ci->home->parent;
	ci2.numeric = ci->numeric;
	if (key_handle(&ci2) == 0)
		return 0;
	ci->str = ci2.str;
	if (ci->numeric != NO_NUMERIC)
		return 1;
	/* Need to continue over other matching lines */
	plen = strlen(cd->prefix);
	ci2.mark = mark_dup(ci->mark, 1);
	while (1) {
		ci2.numeric = ci->numeric;
		ci2.focus = ci->home->parent;
		key_handle(&ci2);
		if (ci2.str == NULL ||
		    strncmp(ci2.str, cd->prefix, plen) == 0)
			break;
		/* have a match, so move the mark to here. */
		mark_to_mark(ci->mark, ci2.mark);
	}
	mark_free(ci2.mark);
	return 1;
}

DEF_CMD(render_complete_prev)
{
	/* If ->numeric is 0 we just need 'start of line' so use
	 * underlying function.
	 * otherwise call repeatedly and then render the line and see if
	 * it matches the prefix.
	 */
	struct cmd_info ci2 = {0}, ci3 = {0};
	struct complete_data *cd = ci->home->data;
	int plen;
	int ret;

	ci2.key = ci->key;
	ci2.mark = ci->mark;
	ci2.focus = ci->home->parent;
	ci2.numeric = 0;

	ci3.key= "render-line";
	ci3.focus = ci->home->parent;
	while (1) {
		int cmp;

		ret = key_handle(&ci2);
		if (ret <= 0 || ci->numeric == 0)
			/* Either hit start-of-file, or have what we need */
			break;
		/* we must be looking at a possible option for the previous
		 * line
		 */
		if (ci2.mark == ci->mark)
			ci2.mark = mark_dup(ci->mark, 1);
		plen = strlen(cd->prefix);
		ci3.mark = mark_dup(ci2.mark, 1);
		ci3.numeric = NO_NUMERIC;
		if (key_handle(&ci3) == 0) {
			mark_free(ci3.mark);
			break;
		}
		mark_free(ci3.mark);
		if (ci3.str == NULL)
			cmp = -1;
		else
			cmp = strncmp(ci3.str, cd->prefix, plen);
		if (cmp != 0 || ci2.numeric != 1 || ci->extra != 42)
			free(ci3.str);
		else
			ci->str2 = ci3.str;
		if (cmp == 0 && ci2.numeric == 1)
			/* This is a valid new start-of-line */
			break;
		/* step back once more */
		ci2.numeric = 1;
	}
	if (ci2.mark != ci->mark) {
		if (ret > 0)
			/* move ci->mark back to ci2.mark */
			mark_to_mark(ci->mark, ci2.mark);
		mark_free(ci2.mark);
	}
	return ret;
}

DEF_CMD(complete_close)
{
	struct pane *p = ci->home;
	struct complete_data *cd = p->data;
	free(cd->prefix);
	free(cd);
	return 1;
}

DEF_CMD(complete_attach);
DEF_CMD(complete_clone)
{
	struct pane *parent = ci->focus;
	struct pane *p = ci->home, *c;

	complete_attach.func(ci);
	c = pane_child(p);
	if (c)
		return pane_clone(c, parent->focus);
	return 1;
}

DEF_CMD(complete_nomove)
{
	if (strcmp(ci->key, "Move-File") == 0)
		return 0;
	return 1;
}

DEF_CMD(complete_eol)
{
	int rpt = RPT_NUM(ci);

	if (rpt >= -1 && rpt <= 1)
		/* movement within the line */
		return 1;
	while (rpt < -1) {
		struct cmd_info ci2 = {0};
		ci2.key = "render-line-prev";
		ci2.numeric = 1;
		ci2.mark = ci->mark;
		ci2.focus = ci->focus;
		ci2.home = ci->home;
		if (render_complete_prev_func(&ci2) < 0)
			rpt = -1;
		rpt += 1;
	}
	while (rpt > 1) {
		struct cmd_info ci2 = {0};
		ci2.key = "render-line";
		ci2.numeric = NO_NUMERIC;
		ci2.mark = ci->mark;
		ci2.focus = ci->focus;
		ci2.home = ci->home;
		if (render_complete_line_func(&ci2) < 0)
			rpt = 1;
		else
			free(ci2.str);
		rpt -= 1;
	}
	return 1;
}

static int common_len(char *a, char *b)
{
	int len = 0;
	while (*a && *a == *b) {
		a += 1;
		b += 1;
		len += 1;
	}
	return len;
}

DEF_CMD(complete_set_prefix)
{
	/* Set the prefix, force a full refresh, and move point
	 * to the first match.
	 * If there is no match, return -1.
	 * Otherwise return number of matches in ->extra and
	 * the longest common prefix in ->str.
	 */
	struct pane *p = ci->home;
	struct complete_data *cd = p->data;
	struct cmd_info ci2 = {0};
	struct mark *m;
	int cnt = 0;
	char *common = NULL;

	free(cd->prefix);
	cd->prefix = strdup(ci->str);

	ci2.key = "doc:dup-point";
	ci2.focus = ci->home;
	ci2.extra = MARK_UNGROUPED;
	key_handle_focus(&ci2);
	m = ci2.mark;

	memset(&ci2, 0, sizeof(ci2));

	ci2.key = "Move-File";
	ci2.focus = ci->home;
	ci2.numeric = 1;
	ci2.mark = m;
	key_handle_focus(&ci2);

	memset(&ci2, 0, sizeof(ci2));
	ci2.key = "render-line-prev";
	ci2.numeric = 1;
	ci2.mark = m;
	ci2.focus = p;
	ci2.home = p;
	ci2.extra = 42; /* request copy of line in str2 */
	while (render_complete_prev_func(&ci2) > 0) {
		char *c = ci2.str2;
		int l = strlen(c);
		if (c[l-1] == '\n')
			l -= 1;
		if (common == NULL)
			common = strndup(c, l);
		else
			common[common_len(c, common)] = 0;
		cnt += 1;
	}
	ci->extra = cnt;
	ci->str = common;
	memset(&ci2, 0, sizeof(ci2));
	ci2.key = "Move-to";
	ci2.mark = m;
	ci2.focus = ci->home;
	key_handle_focus(&ci2);
	mark_free(m);
	memset(&ci2, 0, sizeof(ci2));
	ci2.key = "render-lines:redraw";
	ci2.focus = ci->focus;
	key_handle_focus(&ci2);
	return 1;
}

DEF_CMD(complete_return)
{
	/* submit the selected entry to the popup */
	struct pane *p = ci->home;
	struct complete_data *cd = p->data;
	struct cmd_info ci2 = {0};
	char *str;
	int l;

	ci2.key = "render-line";
	ci2.focus = ci->home;
	ci2.home = ci->home;
	ci2.mark = ci->mark;
	ci2.numeric = NO_NUMERIC;
	render_complete_line_func(&ci2);
	str = ci2.str;
	if (!str)
		return 1;
	l = strlen(str);
	if (l && str[l-1] == '\n')
		str[l-1] = 0;

	memset(&ci2, 0, sizeof(ci2));
	ci2.key = ci->key;
	ci2.focus = ci->home->parent;
	ci2.str = str + strlen(cd->prefix);
	ci2.numeric = NO_NUMERIC;
	key_handle(&ci2);
	free(str);
	return 1;
}

static struct map *rc_map;

DEF_LOOKUP_CMD(complete_handle, rc_map)

static void register_map(void)
{
	rc_map = key_alloc();

	key_add(rc_map, "render-line", &render_complete_line);
	key_add(rc_map, "render-line-prev", &render_complete_prev);
	key_add(rc_map, "Close", &complete_close);
	key_add(rc_map, "Clone", &complete_clone);

	key_add_range(rc_map, "Move-", "Move-\377", &complete_nomove);
	key_add(rc_map, "Move-EOL", &complete_eol);

	key_add(rc_map, "popup:Return", &complete_return);

	key_add(rc_map, "Complete:prefix", &complete_set_prefix);
}

REDEF_CMD(complete_attach)
{
	struct pane *complete;
	struct pane *lines;
	struct pane *parent;
	struct complete_data *cd;
	struct cmd_info ci2 = {0};

	if (!rc_map)
		register_map();

	/* Need to interpose a new pane between the 'render-lines' pane,
	 * which we assume is 'ci->focus' and its parent, so we can
	 * re-interpret lines.
	 * Find the 'render-line-prev' pane by sending a render-line request
	 * (with NULLs so it fails) and grabbing 'home'
	 */
	ci2.key = "render-line";
	ci2.focus = ci->focus;
	if (key_handle_focus(&ci2) == 0)
		return -1;
	parent = ci2.home;
	lines = pane_child(parent);
	mark_free(ci2.mark);


	cd = calloc(1, sizeof(*cd));
	complete = pane_register(parent, 0, &complete_handle.c, cd, NULL);
	if (!complete) {
		free(cd);
		return -1;
	}
	pane_reparent(lines, complete);
	pane_check_size(complete);
	cd->prefix = strdup("");

	ci->focus = complete;
	return 1;
}

void edlib_init(struct editor *ed)
{
	key_add(ed->commands, "render-complete-attach", &complete_attach);
}
