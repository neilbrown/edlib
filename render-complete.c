/*
 * Copyright Neil Brown ©2015 <neil@brown.name>
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
#include "misc.h"

struct complete_data {
	char *prefix;
};

struct rlcb {
	struct command c;
	int keep, plen, cmp;
	char *prefix, *str;
};

static char *add_highlight_prefix(char *orig, int plen, char *attr)
{
	struct buf ret;

	if (orig == NULL)
		return orig;
	buf_init(&ret);
	buf_concat(&ret, attr);
	while (plen > 0 && *orig) {
		if (*orig == '<')
			buf_append_byte(&ret, *orig++);
		buf_append_byte(&ret, *orig++);
		plen -= 1;
	}
	buf_concat(&ret, "</>");
	buf_concat(&ret, orig);
	return buf_final(&ret);
}

DEF_CMD(save_highlighted)
{
	struct call_return *cr = container_of(ci->comm, struct call_return, c);
	cr->s = add_highlight_prefix(ci->str, cr->i, "<fg:red>");
	return 1;
}

DEF_CMD(rcl_cb)
{
	struct rlcb *cb = container_of(ci->comm, struct rlcb, c);
	if (ci->str == NULL)
		cb->cmp = 0;
	else
		cb->cmp = strncmp(ci->str, cb->prefix, cb->plen);
	return 1;
}
DEF_CMD(render_complete_line)
{
	/* The first line *must* match the prefix.
	 * skip over any following lines that don't
	 */
	struct cmd_info ci2 = {0};
	struct complete_data *cd = ci->home->data;
	int plen = strlen(cd->prefix);
	struct call_return cr;
	struct rlcb cb;
	int ret;

	if (!ci->mark)
		return -1;

	ci2.key = ci->key;
	ci2.mark = ci->mark;
	ci2.mark2 = ci->mark2;
	ci2.focus = ci->home->parent;
	ci2.numeric = ci->numeric;
	cr.c = save_highlighted;
	cr.i = plen;
	cr.s = NULL;
	ci2.comm2 = &cr.c;
	if (key_handle(&ci2) == 0)
		return 0;

	ret = comm_call(ci->comm2, "callback:render", ci->focus, 0, NULL, cr.s, 0);
	if (ci->numeric != NO_NUMERIC)
		return ret;
	/* Need to continue over other matching lines */
	ci2.mark = mark_dup(ci->mark, 1);
	while (1) {
		ci2.numeric = ci->numeric;
		ci2.focus = ci->home->parent;
		cb.c = rcl_cb;
		cb.plen = plen;
		cb.prefix = cd->prefix;
		cb.cmp = 0;
		ci2.comm2 = &cb.c;
		key_handle(&ci2);
		if (cb.cmp == 0)
			break;

		/* have a non-match, so move the mark over it. */
		mark_to_mark(ci->mark, ci2.mark);
	}
	mark_free(ci2.mark);
	return ret;
}

DEF_CMD(rlcb)
{
	struct rlcb *cb = container_of(ci->comm, struct rlcb, c);
	if (ci->str == NULL)
		cb->cmp = -1;
	else
		cb->cmp = strncmp(ci->str, cb->prefix, cb->plen);
	if (cb->cmp == 0 && cb->keep && ci->str)
		cb->str = strdup(ci->str);
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
	struct rlcb cb;
	struct complete_data *cd = ci->home->data;
	int ret;

	ci2.key = ci->key;
	ci2.mark = ci->mark;
	ci2.focus = ci->home->parent;
	ci2.numeric = 0;

	ci3.key = "render-line";
	ci3.focus = ci->home->parent;
	cb.c = rlcb;
	cb.str = NULL;
	cb.prefix = cd->prefix;
	cb.plen = strlen(cb.prefix);
	cb.cmp = 0;
	ci3.comm2 = &cb.c;
	while (1) {
		ret = key_handle(&ci2);
		if (ret <= 0 || ci->numeric == 0)
			/* Either hit start-of-file, or have what we need */
			break;
		/* we must be looking at a possible option for the previous
		 * line
		 */
		if (ci2.mark == ci->mark)
			ci2.mark = mark_dup(ci->mark, 1);
		ci3.mark = mark_dup(ci2.mark, 1);
		ci3.numeric = NO_NUMERIC;
		cb.keep = ci2.numeric == 1 && ci->extra == 42;
		cb.str = NULL;
		if (key_handle(&ci3) != 1) {
			mark_free(ci3.mark);
			break;
		}
		mark_free(ci3.mark);
		/* This is a horrible hack, but as it is entirely internal
		 * to this module it can say for now.
		 * Cast ci to remove any 'const' tag that I hope to add soon.
		 */
		((struct cmd_info*)ci)->str2 = cb.str;

		if (cb.cmp == 0 && ci2.numeric == 1)
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
		struct call_return cr;
		ci2.key = "render-line";
		ci2.numeric = NO_NUMERIC;
		ci2.mark = ci->mark;
		ci2.focus = ci->focus;
		ci2.home = ci->home;
		cr.c = eol_cb;
		ci2.comm2 = &cr.c;
		if (render_complete_line_func(&ci2) <= 0)
			rpt = 1;
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

	m = mark_at_point(ci->focus, NULL, MARK_UNGROUPED);
	call3("Move-File", ci->focus, 1, m);

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
	comm_call(ci->comm2, "callback:prefix", ci->focus, 0, NULL, common, 0);
	free(common);
	call3("Move-to", ci->focus, 0, m);
	mark_free(m);
	call3("render-lines:redraw", ci->focus, 0, NULL);
	return cnt + 1;
}

DEF_CMD(save_str)
{
	struct call_return *cr = container_of(ci->comm, struct call_return, c);
	cr->s = ci->str ? strdup(ci->str) : NULL;
	return 1;
}

DEF_CMD(complete_return)
{
	/* submit the selected entry to the popup */
	struct pane *p = ci->home;
	struct complete_data *cd = p->data;
	struct cmd_info ci2 = {0};
	struct call_return cr;
	int l;
	char *c1, *c2;

	ci2.key = "render-line";
	ci2.focus = ci->home;
	ci2.home = ci->home;
	ci2.mark = ci->mark;
	ci2.numeric = NO_NUMERIC;
	cr.c = save_str;
	cr.s = NULL;
	ci2.comm2 = &cr.c;
	render_complete_line_func(&ci2);
	if (!cr.s)
		return 1;
	l = strlen(cr.s);
	if (l && cr.s[l-1] == '\n')
		cr.s[l-1] = 0;
	c1 = c2 = cr.s;
	while (*c2) {
		if (*c2 != '<') {
			*c1++ = *c2++;
			continue;
		}
		c2 += 1;
		if (*c2 == '<') {
			*c1++ = *c2++;
			continue;
		}
		while (*c2 && c2[-1] != '>')
			c2++;
	}
	*c1 = 0;

	memset(&ci2, 0, sizeof(ci2));
	ci2.key = ci->key;
	ci2.focus = ci->home->parent;
	ci2.str = cr.s + strlen(cd->prefix);
	ci2.numeric = NO_NUMERIC;
	key_handle(&ci2);
	free(cr.s);
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
	struct complete_data *cd;

	if (!rc_map)
		register_map();

	cd = calloc(1, sizeof(*cd));
	complete = pane_register(ci->focus, 0, &complete_handle.c, cd, NULL);
	if (!complete) {
		free(cd);
		return -1;
	}
	pane_check_size(complete);
	cd->prefix = strdup("");

	return comm_call(ci->comm2, "callback:attach", complete, 0, NULL, NULL, 0);
}

void edlib_init(struct pane *ed)
{
	call_comm("global-set-command", ed, 0, NULL, "render-complete-attach",
		  0, &complete_attach);
}
