/*
 * Copyright Neil Brown ©2015-2019 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * render-complete - support string completion.
 *
 * This should be attached between render-lines and the pane which
 * provides the lines.  It is given a prefix and it suppresses all
 * lines which don't start with the prefix.
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
	char *prefix safe;
	int prefix_only;
};

struct rlcb {
	struct command c;
	int keep, plen, cmp;
	int prefix_only;
	char *prefix safe, *str;
};

static char *add_highlight_prefix(char *orig, int start, int plen, char *attr safe)
{
	struct buf ret;
	char *c safe;

	if (orig == NULL)
		return orig;
	buf_init(&ret);
	c = orig;
	while (start > 0 && *c) {
		if (*c == '<')
			buf_append_byte(&ret, *c++);
		buf_append_byte(&ret, *c++);
		start -= 1;
	}
	buf_concat(&ret, attr);
	while (plen > 0 && *c) {
		if (*c == '<')
			buf_append_byte(&ret, *c++);
		buf_append_byte(&ret, *c++);
		plen -= 1;
	}
	buf_concat(&ret, "</>");
	buf_concat(&ret, c);
	return buf_final(&ret);
}

DEF_CMD(save_highlighted)
{
	struct rlcb *cb = container_of(ci->comm, struct rlcb, c);
	char *start;

	if (!ci->str)
		return 1;

	start = strstr(ci->str, cb->prefix);
	if (!start)
		start = ci->str;
	cb->str = add_highlight_prefix(ci->str, start - ci->str, cb->plen, "<fg:red>");
	return 1;
}

DEF_CMD(rcl_cb)
{
	struct rlcb *cb = container_of(ci->comm, struct rlcb, c);
	if (ci->str == NULL)
		cb->cmp = 0;
	else if (cb->prefix_only)
		cb->cmp = strncmp(ci->str, cb->prefix, cb->plen);
	else
		cb->cmp = strstr(ci->str, cb->prefix) ? 0 : 1;
	return 1;
}
DEF_CMD(render_complete_line)
{
	/* Skip any line that doesn't match, and
	 * return a highlighted version of the firs one
	 * that does.
	 * Then skip over any other non-matches.
	 */
	struct complete_data *cd = ci->home->data;
	int plen = strlen(cd->prefix);
	struct rlcb cb;
	int ret;
	struct mark *m;

	if (!ci->mark || !ci->home->parent)
		return Enoarg;

	m = mark_dup(ci->mark);
	cb.plen = plen;
	cb.prefix_only = cd->prefix_only;
	cb.prefix = cd->prefix;
	cb.str = NULL;
	cb.cmp = 0;
	cb.c = rcl_cb;
	do {
		mark_to_mark(ci->mark, m);
		cb.cmp = 0;
		call_comm(ci->key, ci->home->parent, &cb.c, NO_NUMERIC, m);
	} while (cb.cmp);
	mark_free(m);
	cb.c = save_highlighted;
	if (call_comm(ci->key, ci->home->parent, &cb.c, ci->num, ci->mark,
	              NULL, 0, ci->mark2) == 0)
		return 0;

	ret = comm_call(ci->comm2, "callback:render", ci->focus, 0, NULL, cb.str);
	free(cb.str);
	if (ci->num != NO_NUMERIC)
		/* Was rendering to find a cursor, don't need to skip */
		return ret;
	/* Need to continue over other matching lines */
	m = mark_dup(ci->mark);
	cb.c = rcl_cb;
	do {
		/* have a non-match, so move the mark over it. */
		mark_to_mark(ci->mark, m);
		cb.cmp = 0;
		call_comm(ci->key, ci->home->parent, &cb.c, NO_NUMERIC, m);
	} while (cb.cmp);

	mark_free(m);
	return ret;
}

DEF_CMD(rlcb)
{
	struct rlcb *cb = container_of(ci->comm, struct rlcb, c);
	if (ci->str == NULL)
		cb->cmp = -1;
	else if (cb->prefix_only)
		cb->cmp = strncmp(ci->str, cb->prefix, cb->plen);
	else
		cb->cmp = strstr(ci->str, cb->prefix) ? 0 : 1;
	if (cb->cmp == 0 && cb->keep && ci->str)
		cb->str = strdup(ci->str);
	return 1;
}

static int do_render_complete_prev(struct complete_data *cd safe, struct mark *m safe,
				   struct pane *focus safe, int n, char **savestr)
{
	/* If 'n' is 0 we just need 'start of line' so use
	 * underlying function.
	 * otherwise call repeatedly and then render the line and see if
	 * it matches the prefix.
	 */
	struct rlcb cb;
	int ret;
	struct mark *m2, *m3;
	int n2;

	if (savestr)
		*savestr = NULL;
	m2 = m;
	n2 = 0;

	cb.c = rlcb;
	cb.str = NULL;
	cb.prefix = cd->prefix;
	cb.prefix_only = cd->prefix_only;
	cb.plen = strlen(cb.prefix);
	cb.cmp = 0;
	while (1) {
		ret = call("render-line-prev", focus, n2, m2);
		if (ret <= 0 || n == 0)
			/* Either hit start-of-file, or have what we need */
			break;
		/* we must be looking at a possible option for the previous
		 * line
		 */
		if (m2 == m)
			m2 = mark_dup(m);
		m3 = mark_dup(m2);
		cb.keep = n2 == 1 && savestr;
		cb.str = NULL;
		if (call_comm("render-line", focus, &cb.c, NO_NUMERIC, m3)
		    != 1) {
			mark_free(m3);
			break;
		}
		mark_free(m3);

		if (savestr)
			*savestr = cb.str;

		if (cb.cmp == 0 && n2 == 1)
			/* This is a valid new start-of-line */
			break;
		/* step back once more */
		n2 = 1;
	}
	if (m2 != m) {
		if (ret > 0)
			/* move m back to m2 */
			mark_to_mark(m, m2);
		mark_free(m2);
	}
	return ret;
}

DEF_CMD(render_complete_prev)
{
	/* If ->num is 0 we just need 'start of line' so use
	 * underlying function.
	 * otherwise call repeatedly and then render the line and see if
	 * it matches the prefix.
	 */
	struct complete_data *cd = ci->home->data;
	if (!ci->mark || !ci->home->parent)
		return Enoarg;
	return do_render_complete_prev(cd, ci->mark, ci->home->parent, ci->num, NULL);
}

DEF_CMD(complete_close)
{
	struct complete_data *cd = ci->home->data;

	free(cd->prefix);
	free(cd);
	return 1;
}

DEF_CMD(complete_attach);
DEF_CMD(complete_clone)
{
	struct pane *parent = ci->focus;

	complete_attach.func(ci);
	pane_clone_children(ci->home, parent->focus);
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

	if (!ci->mark || !ci->focus->parent)
		return Enoarg;
	if (rpt >= -1 && rpt <= 1)
		/* movement within the line */
		return 1;
	while (rpt < -1) {
		if (do_render_complete_prev(ci->home->data, ci->mark,
					    ci->focus->parent, 1, NULL) < 0)
			rpt = -1;
		rpt += 1;
	}
	while (rpt > 1) {
		struct call_return cr;
		cr.c = eol_cb;
		if (home_call(ci->home, "render-line",
			      ci->focus, NO_NUMERIC, ci->mark, NULL,
			      0, NULL, NULL, 0,0, &cr.c) <= 0)
			rpt = 1;
		rpt -= 1;
	}
	return 1;
}

static int common_len(char *a safe, char *b safe)
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
	 * to the first match at start-of-line, or first match
	 * If there is no match, return -1.
	 * Otherwise return number of matches in ->num2 and
	 * the longest common prefix in ->str.
	 */
	struct pane *p = ci->home;
	struct complete_data *cd = p->data;
	struct mark *m;
	struct mark *m2 = NULL;
	char *c;
	int cnt = 0;
	char *common = NULL;
	int at_start = 0;

	if (!ci->str)
		return Enoarg;
	free(cd->prefix);
	cd->prefix = strdup(ci->str);
	cd->prefix_only = !ci->num;

	m = mark_at_point(ci->focus, NULL, MARK_UNGROUPED);
	if (!m || !p->parent)
		return Esys;
	/* Move to end-of-document */
	call("Move-File", ci->focus, 1, m);

	while (do_render_complete_prev(cd, m, p->parent, 1, &c) > 0 && c) {
		int l;
		char *match = c;
		if (!cd->prefix_only)
			match = strstr(match, cd->prefix);
		if (!match)
			break;
		l = strlen(match);
		if (l && match[l-1] == '\n')
			l -= 1;
		if (!cd->prefix_only && strncmp(c, cd->prefix, strlen(cd->prefix)) == 0) {
			if (m2)
				mark_free(m2);
			m2 = mark_dup(m);
			if (!at_start) {
				/* If there are matches at the start, the common prefix
				 * calculation only noticed them
				 */
				at_start = 1;
				free(common);
				common = NULL;
			}
		} else if (at_start)
			at_start = 2;
		if (at_start != 2) {
			if (common == NULL)
				common = strndup(match, l);
			else
				common[common_len(match, common)] = 0;
		}
		cnt += 1;
	}
	comm_call(ci->comm2, "callback:prefix", ci->focus, 0, NULL, common);
	free(common);
	if (m2) {
		call("Move-to", ci->focus, 0, m2);
		mark_free(m2);
	} else
		call("Move-to", ci->focus, 0, m);
	mark_free(m);
	
	pane_damaged(ci->focus, DAMAGED_VIEW);
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
	struct call_return cr;
	int l;
	char *c1, *c2;

	if (!ci->mark || !ci->home->parent)
		return Enoarg;

	cr.c = save_str;
	cr.s = NULL;
	home_call(ci->home, "render-line",
		  ci->home, NO_NUMERIC, ci->mark, NULL, 0, NULL,
		  NULL, 0,0, &cr.c);
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

	call("popup:close", ci->home->parent, NO_NUMERIC, NULL,
	      cr.s, 0);
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

	key_add(rc_map, "Enter", &complete_return);

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
		return Esys;
	}
	cd->prefix = strdup("");
	cd->prefix_only = 1;

	return comm_call(ci->comm2, "callback:attach", complete);
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, &complete_attach, 0, NULL, "attach-render-complete");
}
