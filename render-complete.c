/*
 * Copyright Neil Brown ©2015-2023 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * render-complete - support string completion.
 *
 * This should be attached between render-lines and the pane which
 * provides the lines.  It is given a string and it suppresses all
 * lines which don't match the string.  Matching can be case-insensitive,
 * and may require the string to be at the start of the line.
 *
 * The linefilter module is used manage the selective display of lines.
 * This module examine the results provided by linefilter and extends the
 * string to the maximum that still matches the same set of lines.
 * Keystrokes can extend or contract the match, which will cause display
 * to be updated.
 *
 * This module doesn't hold any marks on any document.  The marks
 * held by the rendered should be sufficient.
 */

#define _GNU_SOURCE for strcasestr
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#define PANE_DATA_TYPE struct complete_data
#include "core.h"
#include "misc.h"

struct complete_data {
	char *orig;
	char *attr;
	struct stk {
		struct stk *prev;
		const char *substr safe;
	} *stk;
	int prefix_only;
};
#include "core-pane.h"

static struct map *rc_map;

DEF_LOOKUP_CMD(complete_handle, rc_map);

struct rlcb {
	struct command c;
	int plen;
	const char *prefix safe, *str;
};

static void strip_attrs(char *c safe)
{
	char *n = c;
	if (*c == ack) {
		for (; *c; c++) {
			if (*c == ack || *c == etx)
				continue;
			if (*c != soh) {
				*n++ = *c;
				continue;
			}
			while (*c != stx)
				c++;
		}
	} else {
		for (; *c ; c++) {
			if (*c == '<' && c[1] == '<') {
				*n++ = *c++;
				continue;
			}
			if (*c != '<') {
				*n++ = *c;
				continue;
			}
			while (*c != '>')
				c++;
		}
	}
	*n = 0;
}

static const char *add_highlight(const char *orig, int start, int len,
				 const char *attr safe, int *offset, int *cpos)
{
	/* Create a copy of 'orig' with all non-attr chars from start for len
	 * given the extra 'attr'.  start and len count non-attr chars.
	 * If offset!=NULL, stop when we get to that place in the result,
	 * and update *offset with that place in orig.
	 * If cpos, then when we reach cpos in orig, report len of result.
	 */
	struct buf ret;
	const char *c safe;
	bool use_lt = True;
	int cp = -1;

	if (!len)
		return orig;

	if (cpos) {
		cp = *cpos;
		*cpos = -1;
	}

	if (!len)
		return orig;

	if (orig == NULL)
		return NULL;
	buf_init(&ret);
	c = orig;
	if (*c == ack) {
		use_lt = False;
		buf_append_byte(&ret, ack);
		c++;
	}
	while (*c && (!offset || ret.len < *offset)) {
		if (cp >= 0 && (c-orig) >= cp && *cpos == -1)
			*cpos = ret.len;
		if ((use_lt && (*c != '<' || c[1] == '<')) ||
		    (!use_lt && (*c != ack && *c != soh && *c != etx))) {
			/* This is regular text */
			if (start > 0)
				start -= 1;
			else if (start == 0) {
				if (use_lt) {
					buf_append(&ret, '<');
					buf_concat(&ret, attr);
					buf_append(&ret, '>');
				} else {
					buf_append(&ret, soh);
					buf_concat(&ret, attr);
					buf_append(&ret, stx);
				}
				start = -1;
			}
			if (use_lt && *c == '<')
				buf_append_byte(&ret, *c++);
			buf_append_byte(&ret, *c++);
			if (start < 0 && len > 0) {
				len -= 1;
				if (len == 0) {
					if (use_lt)
						buf_concat(&ret, "</>");
					else
						buf_append(&ret, etx);
				}
			}
			continue;
		}
		/* Not regular text. */
		if (start < 0 && len > 0) {
			/* Close the attr highlight */
			start = 0;
			if (use_lt)
				buf_concat(&ret, "</>");
			else
				buf_append(&ret, etx);
		}
		if (!use_lt) {
			buf_append(&ret, *c);
			if (*c == ack || *c == etx) {
				c++;
				continue;
			}
			c++;
			while (*c && *c != etx)
				buf_append(&ret, *c++);
			if (*c)
				buf_append(&ret, *c++);
		} else {
			while (*c && *c != '>')
				buf_append_byte(&ret, *c++);
			if (*c)
				buf_append(&ret, *c++);
		}
	}
	if (offset)
		*offset = c - orig;
	return buf_final(&ret);
}

DEF_CMD(get_offset)
{
	if (ci->num < 0)
		return 1;
	else
		return ci->num + 1;
}

DEF_CMD(render_complete_line)
{
	struct complete_data *cd = ci->home->data;
	char *line, *l2, *start = NULL;
	const char *hl;
	const char *match;
	int ret, startlen;
	struct mark *m;
	int offset = 0;

	if (!ci->mark || !cd->stk)
		return Enoarg;

	m = mark_dup(ci->mark);
	line = call_ret(str, ci->key, ci->home->parent, -1, m);
	if (!line) {
		mark_free(m);
		return Efail;
	}
	match = cd->stk->substr;
	l2 = strsave(ci->home, line);
	if (l2){
		strip_attrs(l2);
		start = strcasestr(l2, match);
	}
	if (!start)
		startlen = 0;
	else
		startlen = start - l2;
	if (ci->num >= 0) {
		/* Only want 'num' bytes from start, with ->mark positioned.
		 * So need to find how many bytes of 'line' produce num bytes
		 * of highlighted line.
		 */
		int num = ci->num;
		hl = add_highlight(line, startlen, strlen(match), "fg:red", &num, NULL);
		if (hl != line)
			free((void*)hl);
		free(line);
		mark_free(m);
		line = call_ret(str, ci->key, ci->home->parent,
				num, ci->mark);
	} else if (ci->mark2) {
		/* Only want up-to the cursor, which might be in the middle of
		 * the highlighted region.  Now we know where that is, we can
		 * highlight whatever part is still visible.
		 */
		mark_free(m);
		offset = call_comm(ci->key, ci->home->parent, &get_offset,
				ci->num, ci->mark, NULL,
				0, ci->mark2);
		if (offset >= 1)
			offset -= 1;
		else
			offset = -1;
	} else {
		mark_to_mark(ci->mark, m);
		mark_free(m);
	}
	if (!line)
		return Efail;
	hl = add_highlight(line, startlen, strlen(match), "fg:red", NULL, &offset);

	ret = comm_call(ci->comm2, "callback:render", ci->focus,
			offset, NULL, hl);
	if (hl != line)
		free((void*)hl);
	free(line);
	return ret;
}

DEF_CMD(complete_close)
{
	struct complete_data *cd = ci->home->data;
	struct stk *stk = cd->stk;

	while (stk) {
		struct stk *t = stk;
		stk = stk->prev;
		free((void*)t->substr);
		free(t);
	}
	cd->stk = NULL;

	free(cd->attr);
	cd->attr = NULL;
	return 1;
}

static struct pane *complete_pane(struct pane *focus safe)
{
	struct pane *complete;
	struct complete_data *cd;

	complete = pane_register(focus, 0, &complete_handle.c);
	if (!complete)
		return NULL;
	cd = complete->data;
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
	int pl;
	const char *suffix = ksuffix(ci, "doc:char-");

	if (!cd->stk)
		return Efail;
	pl = strlen(cd->stk->substr);
	np = malloc(pl + strlen(suffix) + 1);
	strcpy(np, cd->stk->substr);
	strcpy(np+pl, suffix);
	call("Complete:prefix", ci->focus, !cd->prefix_only, NULL, np,
	     0, NULL, cd->attr);
	free(np);
	return 1;
}

DEF_CMD(complete_bs)
{
	struct complete_data *cd = ci->home->data;
	struct stk *stk = cd->stk;
	char *old = NULL;

	if (!stk || !stk->prev)
		return 1;
	if (stk->substr[0] && !stk->prev->substr[0]) {
		old = (void*)stk->substr;
		old[strlen(old)-1] = 0;
	} else {
		cd->stk = stk->prev;
		free((void*)stk->substr);
		free(stk);
	}
	call("Complete:prefix", ci->home, 0, NULL, NULL, 1,
	     NULL, cd->attr);
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

DEF_CB(set_cb)
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
	 * If ci->num with ->str, allow substrings, else prefix-only
	 * if ci->num2, don't autocomplete, just display matches
	 */
	struct pane *p = ci->home;
	struct complete_data *cd = p->data;
	struct setcb cb;
	struct stk *stk;
	struct mark *m;

	if (!cd->stk)
		return Efail;
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
	if (ci->str2 && (!cd->attr || strcmp(cd->attr, ci->str2) != 0)) {
		free(cd->attr);
		cd->attr = strdup(ci->str2);
	}

	call_comm("Filter:set", ci->focus, &cb.c,
		  cd->prefix_only ? 3 : 2, NULL, cb.ss, 0, NULL, cd->attr);

	if (cb.cnt <= 0) {
		/* Revert */
		call("Filter:set", ci->focus,
		     cd->prefix_only ? 3 : 2, NULL, cd->stk->substr,
		     0, NULL, cd->attr);
		if (m)
			call("Move-to", ci->focus, 0, m);
	}
	mark_free(m);

	if (cb.common_pre && cb.common && cb.cnt && ci->str) {
		if (ci->num2 == 0)
			strcat(cb.common_pre, cb.common);
		stk = malloc(sizeof(*stk));
		stk->substr = cb.common_pre;
		stk->prev = cd->stk;
		cd->stk = stk;
		cb.common_pre = NULL;
		call("Filter:set", ci->focus,
		     cd->prefix_only ? 3 : 2, NULL, cd->stk->substr,
			  0, NULL, cd->attr);
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

DEF_CB(save_str)
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

	if (!ci->mark)
		return Enoarg;

	cr.c = save_str;
	cr.s = NULL;
	/* Go to start of line */
	home_call(ci->home, "doc:render-line-prev", ci->home, 0, ci->mark);
	home_call(ci->home, "doc:render-line",
		  ci->home, -1, ci->mark, NULL, 0, NULL,
		  NULL, 0,0, &cr.c);
	if (!cr.s)
		return 1;
	strip_attrs(cr.s);
	l = strlen(cr.s);
	if (l && cr.s[l-1] == '\n')
		cr.s[l-1] = 0;

	call("popup:close", ci->home->parent, NO_NUMERIC, NULL,
	     cr.s, 0);
	free(cr.s);
	return 1;
}

static void register_map(void)
{
	rc_map = key_alloc();

	key_add(rc_map, "doc:render-line", &render_complete_line);
	key_add(rc_map, "Close", &complete_close);
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
