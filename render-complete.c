/*
 * Copyright Neil Brown ©2015-2020 <neil@brown.name>
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

#define _GNU_SOURCE for strcasestr
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
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
	const char *prefix safe, *str;
};

static const char *add_highlight_prefix(const char *orig, int start, int plen,
					const char *attr safe)
{
	struct buf ret;
	const char *c safe;

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
	const char *start;

	if (!ci->str)
		return 1;

	start = strcasestr(ci->str, cb->prefix);
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
		cb->cmp = strncasecmp(ci->str, cb->prefix, cb->plen);
	else
		cb->cmp = strcasestr(ci->str, cb->prefix) ? 0 : 1;
	return 1;
}
DEF_CMD(render_complete_line)
{
	/* Skip any line that doesn't match, and
	 * return a highlighted version of the first one
	 * that does.
	 * Then skip over any other non-matches.
	 */
	struct complete_data *cd = ci->home->data;
	int plen = strlen(cd->prefix);
	struct rlcb cb;
	int ret;
	struct mark *m;

	if (!ci->mark)
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
	} while (cb.cmp && !mark_same(ci->mark, m));
	mark_free(m);
	cb.c = save_highlighted;
	if (call_comm(ci->key, ci->home->parent, &cb.c, ci->num, ci->mark,
		      NULL, 0, ci->mark2) == 0)
		return 0;

	ret = comm_call(ci->comm2, "callback:render", ci->focus, 0, NULL, cb.str);
	free((void*)cb.str);
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
	} while (cb.cmp && !mark_same(ci->mark, m));

	mark_free(m);
	return ret;
}

DEF_CMD(rlcb)
{
	struct rlcb *cb = container_of(ci->comm, struct rlcb, c);
	char *c = ci->str ? strdup(ci->str) : NULL;
	if (c) {
		int i;
		for (i = 0; c[i] ; i++) {
			if (c[i] == '<')
				memmove(c+i, c+i+1, strlen(c+i));
		}
	}
	if (c == NULL)
		cb->cmp = -1;
	else if (cb->prefix_only)
		cb->cmp = strncasecmp(c, cb->prefix, cb->plen);
	else
		cb->cmp = strcasestr(c, cb->prefix) ? 0 : 1;
	if (cb->cmp == 0 && cb->keep && c)
		cb->str = c;
	else
		free(c);
	return 1;
}

static int do_render_complete_prev(struct complete_data *cd safe,
				   struct mark *m safe,
				   struct pane *focus safe, int n,
				   const char **savestr)
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
		ret = call("doc:render-line-prev", focus, n2, m2);
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
		if (call_comm("doc:render-line", focus, &cb.c, NO_NUMERIC, m3)
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
	if (!ci->mark)
		return Enoarg;
	return do_render_complete_prev(cd, ci->mark, ci->home->parent, ci->num, NULL);
}

DEF_CMD(complete_free)
{
	struct complete_data *cd = ci->home->data;

	free(cd->prefix);
	unalloc(cd, pane);
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

DEF_CMD(complete_ignore_replace)
{
	return 1;
}

DEF_CMD(complete_escape)
{
	/* submit the original prefix back*/
	struct complete_data *cd = ci->home->data;

	/* This pane might be closed before the reply string is used,
	 * so we need to save it.
	 */
	call("popup:close", ci->home->parent, NO_NUMERIC, NULL,
	     strsave(ci->home, cd->prefix));
	return 1;
}

DEF_CMD(complete_char)
{
	struct complete_data *cd = ci->home->data;
	char *np;
	struct call_return cr;
	int pl = strlen(cd->prefix);
	const char *suffix = ksuffix(ci, "K-");

	np = malloc(pl + 2);
	strcpy(np, cd->prefix);
	np[pl] = *suffix;
	np[pl+1] = 0;
	cr = call_ret(all, "Complete:prefix", ci->focus, !cd->prefix_only, NULL, np);
	if (cr.i == 0) {
		/* No matches, revert */
		np[pl] = 0;
		call("Complete:prefix", ci->focus, !cd->prefix_only, NULL, np);
	} else if (cr.s && strlen(cr.s) > strlen(np))
		call("Complete:prefix", ci->focus, !cd->prefix_only, NULL, cr.s);
	free(np);
	return 1;
}

DEF_CMD(complete_bs)
{
	struct complete_data *cd = ci->home->data;
	char *np;
	int pl = strlen(cd->prefix);

	np = malloc(pl + 1);
	strcpy(np, cd->prefix);
	np[pl-1] = 0;
	call("Complete:prefix", ci->focus, !cd->prefix_only, NULL, np);
	free(np);
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

	if (!ci->mark)
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
		if (home_call(ci->home, "doc:render-line",
			      ci->focus, NO_NUMERIC, ci->mark, NULL,
			      0, NULL, NULL, 0,0, &cr.c) <= 0)
			rpt = 1;
		rpt -= 1;
	}
	return 1;
}

static int csame(char a, char b)
{
	if (isupper(a))
		a = tolower(a);
	if (isupper(b))
		b = tolower(b);
	return a == b;
}

static int common_len(const char *a safe, const char *b safe)
{
	int len = 0;
	while (*a && csame(*a, *b)) {
		a += 1;
		b += 1;
		len += 1;
	}
	return len;
}

static void adjust_pre(char *common safe, const char *new safe, int len)
{
	int l = strlen(common);
	int newlen = 0;

	while (l && len && csame(common[l-1], new[len-1])) {
		l -= 1;
		len -= 1;
		newlen += 1;
	}
	if (l)
		memmove(common, common+l, newlen+1);
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
	const char *c;
	int cnt = 0;
	char *pfx;
	int plen;
	char *common = NULL;
	/* common_pre is the longest common prefix to 'common' that
	 * appears in all matches in which 'common' appears.  It is
	 * allocated with enough space to append 'common' after the
	 * prefix.
	 */
	char *common_pre = NULL;
	int best_match = 0;

	if (!ci->str)
		return Enoarg;
	free(cd->prefix);
	cd->prefix = strdup(ci->str);
	cd->prefix_only = !ci->num;
	pfx = cd->prefix;
	plen = strlen(pfx);

	m = mark_at_point(ci->focus, NULL, MARK_UNGROUPED);
	if (!m)
		return Efail;
	/* Move to end-of-document */
	call("Move-File", ci->focus, 1, m);

	while (do_render_complete_prev(cd, m, p->parent, 1, &c) > 0 && c) {
		int l;
		const char *match;
		int this_match = 0;

		if (cd->prefix_only) {
			match = c;
			if (strncmp(c, pfx, plen) == 0)
				this_match += 1;
		} else {
			match = strcasestr(c, pfx);
			if (strncasecmp(c, pfx, plen) == 0) {
				this_match += 1;
				if (strncmp(c, pfx, plen) == 0)
					this_match += 1;
			} else if (strstr(c, pfx))
				this_match += 1;
		}
		if (!match)
			/* should be impossible */
			break;
		l = strlen(match);
		if (l && match[l-1] == '\n')
			l -= 1;

		if (this_match > best_match) {
			/* Only use matches at least this good to calculate
			 * 'common'
			 */
			best_match = this_match;
			free(common);
			common = NULL;
			free(common_pre);
			common_pre = NULL;
		}

		if (this_match == best_match) {
			/* This match can be used for 'common' and
			 * initial cursor
			 */
			if (m2)
				mark_free(m2);
			m2 = mark_dup(m);

			if (common == NULL) {
				common = strndup(match, l);
			} else {
				common[common_len(match, common)] = 0;
				/* If 'match' and 'common' disagree on case of
				 * 'prefix', use that of 'prefix'
				 */
				if (memcmp(common, match, plen) != 0)
					memcpy(common, pfx, plen);
			}
			if (!common_pre) {
				common_pre = strndup(c, l + match-c);
				strncpy(common_pre, c, match-c);
				common_pre[match-c] = 0;
			} else
				adjust_pre(common_pre, c, match-c);
		}
		cnt += 1;
	}
	if (common_pre) {
		strcat(common_pre, common);
		comm_call(ci->comm2, "callback:prefix", ci->focus, cnt,
			  NULL, common_pre);
		free(common_pre);
	} else
		comm_call(ci->comm2, "callback:prefix", ci->focus, cnt,
			  NULL, common);
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

	if (!ci->mark)
		return Enoarg;

	cr.c = save_str;
	cr.s = NULL;
	home_call(ci->home, "doc:render-line",
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

DEF_LOOKUP_CMD(complete_handle, rc_map);

static void register_map(void)
{
	rc_map = key_alloc();

	key_add(rc_map, "doc:render-line", &render_complete_line);
	key_add(rc_map, "doc:render-line-prev", &render_complete_prev);
	key_add(rc_map, "Free", &complete_free);
	key_add(rc_map, "Clone", &complete_clone);

	key_add(rc_map, "Replace", &complete_ignore_replace);
	key_add(rc_map, "K:ESC", &complete_escape);
	key_add_range(rc_map, "K- ", "K-~", &complete_char);
	key_add(rc_map, "K:Backspace", &complete_bs);

	key_add_prefix(rc_map, "Move-", &complete_nomove);
	key_add(rc_map, "Move-EOL", &complete_eol);

	key_add(rc_map, "K:Enter", &complete_return);

	key_add(rc_map, "Complete:prefix", &complete_set_prefix);
}

REDEF_CMD(complete_attach)
{
	struct pane *complete;
	struct complete_data *cd;

	if (!rc_map)
		register_map();

	alloc(cd, pane);
	complete = pane_register(ci->focus, 0, &complete_handle.c, cd);
	if (!complete) {
		free(cd);
		return Efail;
	}
	cd->prefix = strdup("");
	cd->prefix_only = 1;

	return comm_call(ci->comm2, "callback:attach", complete);
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, &complete_attach, 0, NULL, "attach-render-complete");
}
