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
	char *orig;
	struct stk {
		struct stk *prev;
		const char *substr safe;
	} *stk safe;
	int prefix_only;
};

static struct map *rc_map;

DEF_LOOKUP_CMD(complete_handle, rc_map);

struct rlcb {
	struct command c;
	int plen;
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

DEF_CMD(render_complete_line)
{
	struct complete_data *cd = ci->home->data;
	struct rlcb cb;
	int ret;

	if (!ci->mark)
		return Enoarg;

	cb.prefix = cd->stk->substr;
	cb.plen = strlen(cd->stk->substr);
	cb.str = NULL;
	cb.c = save_highlighted;
	ret = call_comm(ci->key, ci->home->parent, &cb.c, ci->num, ci->mark,
			NULL, 0, ci->mark2);
	if (ret < 0 || !cb.str)
		return ret;

	ret = comm_call(ci->comm2, "callback:render", ci->focus, 0, NULL, cb.str);
	free((void*)cb.str);
	return ret;
}

DEF_CMD(complete_free)
{
	struct complete_data *cd = ci->home->data;
	struct stk *stk = cd->stk;

	while (stk) {
		struct stk *t = stk;
		stk = stk->prev;
		free((void*)t->substr);
		free(t);
	}

	unalloc(cd, pane);
	return 1;
}


static struct pane *complete_pane(struct pane *focus)
{
	struct pane *complete;
	struct complete_data *cd;

	alloc(cd, pane);
	complete = pane_register(focus, 0, &complete_handle.c, cd);
	if (!complete) {
		unalloc(cd, pane);
		return NULL;
	}
	cd->stk = malloc(sizeof(cd->stk[0]));
	cd->stk->prev = NULL;
	cd->stk->substr = strdup("");
	cd->prefix_only = 1;
	return complete;
}

DEF_CMD(complete_clone)
{
	struct pane *parent = ci->focus;
	struct pane *complete;

	complete = complete_pane(parent);
	if (complete)
		pane_clone_children(ci->home, complete);
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
	     strsave(ci->home, cd->orig));
	return 1;
}

DEF_CMD(complete_char)
{
	struct complete_data *cd = ci->home->data;
	char *np;
	int pl = strlen(cd->stk->substr);
	const char *suffix = ksuffix(ci, "doc:char-");

	np = malloc(pl + strlen(suffix) + 1);
	strcpy(np, cd->stk->substr);
	strcpy(np+pl, suffix);
	call("Complete:prefix", ci->focus, !cd->prefix_only, NULL, np);
	return 1;
}

DEF_CMD(complete_bs)
{
	struct complete_data *cd = ci->home->data;
	struct stk *stk = cd->stk;

	if (!stk->prev)
		return 1;
	cd->stk = stk->prev;
	free((void*)stk->substr);
	free(stk);
	call("Complete:prefix", ci->home);
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

struct setcb {
	struct command c;
	struct complete_data *cd safe;
	const char *ss safe;
	int best_match;
	char *common;
	/* common_pre is the longest common prefix to 'common' that
	 * appears in all matches in which 'common' appears.  It is
	 * allocated with enough space to append 'common' after the
	 * prefix.
	 */
	char *common_pre;
	struct mark *bestm;
	int cnt;
};

DEF_CMD(set_cb)
{
	struct setcb *cb = container_of(ci->comm, struct setcb, c);
	struct complete_data *cd = cb->cd;
	const char *ss = cb->ss;
	int len = strlen(ss);
	const char *c = ci->str;
	const char *match;
	int this_match = 0;
	int l;

	if (!c)
		return Enoarg;
	if (cd->prefix_only) {
		match = c;
		if (strncmp(match, ss, len) == 0)
			this_match += 1;
	} else {
		match = strcasestr(c, ss);
		if (strncasecmp(c, ss, len) == 0) {
			this_match += 1;
			if (strncmp(c, ss, len) == 0)
				this_match += 1;
		} else if (strstr(c, ss))
			this_match += 1;
	}

	if (!match)
		/* should be impossible */
		return 1;

	l = strlen(match);
	if (l && match[l-1] == '\n')
		l -= 1;

	if (this_match > cb->best_match) {
		/* Only use matches at least this good to calculate
		 * 'common'
		 */
		cb->best_match = this_match;
		free(cb->common);
		cb->common = NULL;
		free(cb->common_pre);
		cb->common_pre = NULL;
	}

	if (this_match == cb->best_match) {
		/* This match can be used for 'common' and
		 * initial cursor
		 */
		mark_free(cb->bestm);
		if (ci->mark)
			cb->bestm = mark_dup(ci->mark);

		if (!cb->common) {
			cb->common = strndup(match, l);
		} else {
			cb->common[common_len(match, cb->common)] = 0;
			/* If 'match' and 'common' disagree on case of
			 * 'prefix', use that of 'prefix'
			 */
			if (memcmp(cb->common, match, len) != 0)
				memcpy(cb->common, ss, len);
		}
		if (!cb->common_pre) {
			cb->common_pre = strndup(c, l + match-c);
			strncpy(cb->common_pre, c, match-c);
			cb->common_pre[match-c] = 0;
		} else
			adjust_pre(cb->common_pre, c, match-c);
	}
	cb->cnt += 1;
	return 1;
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
	struct setcb cb;
	struct stk *stk;
	struct mark *m;

	/* Save a copy of the point so we can restore it if needed */
	m = call_ret(mark, "doc:point", ci->focus);
	if (m)
		m = mark_dup(m);

	cb.c = set_cb;
	cb.cd = cd;
	cb.best_match = 0;
	cb.common = NULL;
	cb.common_pre = NULL;
	cb.bestm = NULL;
	cb.cnt = 0;
	if (ci->str) {
		cb.ss = ci->str;
		cd->prefix_only = !ci->num;
	} else {
		cb.ss = cd->stk->substr;
	}

	call_comm("Filter:set", ci->focus, &cb.c,
		  cd->prefix_only ? 3 : 2, NULL, cb.ss);

	if (cb.cnt <= 0) {
		/* Revert */
		call("Filter:set", ci->focus,
		     cd->prefix_only ? 3 : 2, NULL, cd->stk->substr);
		if (m)
			call("Move-to", ci->focus, 0, m);
	}
	mark_free(m);

	if (cb.common_pre && cb.common && cb.cnt && ci->str) {
		strcat(cb.common_pre, cb.common);
		stk = malloc(sizeof(*stk));
		stk->substr = cb.common_pre;
		stk->prev = cd->stk;
		cd->stk = stk;
		cb.common_pre = NULL;
		call("Filter:set", ci->focus,
		     cd->prefix_only ? 3 : 2, NULL, cd->stk->substr);
		comm_call(ci->comm2, "callback:prefix", ci->focus, cb.cnt,
			  NULL, cd->stk->substr);
		if (!cd->orig)
			cd->orig = strdup(ci->str);
	} else {
		comm_call(ci->comm2, "callback:prefix", ci->focus, 0);
	}
	free(cb.common);
	free(cb.common_pre);
	if (cb.bestm) {
		call("Move-to", ci->focus, 0, cb.bestm);
		mark_free(cb.bestm);
	}

	call("view:changed", ci->focus);

	return cb.cnt + 1;
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

static void register_map(void)
{
	rc_map = key_alloc();

	key_add(rc_map, "doc:render-line", &render_complete_line);
	key_add(rc_map, "Free", &complete_free);
	key_add(rc_map, "Clone", &complete_clone);

	key_add(rc_map, "Replace", &complete_ignore_replace);
	key_add(rc_map, "K:ESC", &complete_escape);
	key_add_range(rc_map, "doc:char- ", "doc:char-~", &complete_char);
	key_add(rc_map, "K:Backspace", &complete_bs);

	key_add(rc_map, "K:Enter", &complete_return);

	key_add(rc_map, "Complete:prefix", &complete_set_prefix);
}

DEF_CMD(complete_attach)
{
	struct pane *p = ci->focus;
	struct pane *complete;

	if (!rc_map)
		register_map();

	p = call_ret(pane, "attach-linefilter", p);
	if (!p)
		return Efail;
	complete = complete_pane(p);
	if (!complete) {
		pane_close(p);
		return Efail;
	}

	return comm_call(ci->comm2, "callback:attach", complete);
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, &complete_attach,
		  0, NULL, "attach-render-complete");
}
