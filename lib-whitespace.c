/*
 * Copyright Neil Brown ©2019-2020 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * This module provides highlighting of interesting white-space,
 * and possibly other spacing-related issues.
 * Currently:
 *  tabs are in a different colour (yellow-80+80)
 *  unicode spaces a different colour (red+80-80)
 *  space at EOL are RED (red)
 *  TAB after space are RED (red-80)
 *  anything beyond configured line length is RED (red-80+50 "whitespace-width" or 80)
 *  non-space as first char RED if configured ("whitespace-intent-space")
 *  >=8 spaces RED if configured ("whitespace-max-spaces")
 *  blank line adjacent to blank or start/end of file if configured ("whitespace-single-blank-lines")
 *
 * This is achieved by capturing the "start-of-line" attribute request,
 * reporting attributes that apply to leading chars, and placing a
 * mark with a "render:whitespace" attribute at the next interesting place, if
 * there is one.
 */

#define _GNU_SOURCE /*  for asprintf */
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include "core.h"

struct ws_info {
	struct mark *mymark;
	int mycol;
	int warn_width;
	int max_spaces;
	bool indent_space;
	bool single_blanks;
};

/* a0 and 2007 are non-breaking an not in iswblank, but I want them. */
#define ISWBLANK(c) ((c) == 0xa0 || (c) == 0x2007 || iswblank(c))

static void choose_next(struct pane *focus safe, struct mark *pm safe,
			struct ws_info *ws safe, int skip)
{
	struct mark *m = ws->mymark;

	if (m == NULL) {
		m = mark_dup(pm);
		ws->mymark = m;
	} else
		mark_to_mark(m, pm);
	while (skip > 0) {
		/* Need to look beyond the current location */
		wint_t ch;
		ch = doc_next(focus, m);
		skip -= 1;
		if (ch == '\t')
			ws->mycol = (ws->mycol | 7) + 1;
		else if (ch != WEOF && !is_eol(ch))
			ws->mycol += 1;
		else
			skip = 0;
	}

	while(1) {
		int cnt, rewind, rewindcol, col;
		wint_t ch = doc_following(focus, m);
		wint_t ch2;

		if (ch == WEOF || is_eol(ch))
			break;
		if (ws->mycol >= ws->warn_width) {
			/* everything from here is an error */
			attr_set_str(&m->attrs, "render:whitespace",
				     "bg:red-80+50");
			attr_set_int(&m->attrs, "attr-len", INT_MAX);
			return;
		}
		if (!ISWBLANK(ch)) {
			/* Nothing to highlight here, move forward */
			doc_next(focus, m);
			ws->mycol++;
			continue;
		}
		/* If only spaces/tabs until EOL, then RED,
		 * else keep looking
		 */

		cnt = 0;
		rewind = INT_MAX;
		rewindcol = 0;
		col = ws->mycol;
		while ((ch = doc_next(focus, m)) != WEOF &&
		       ISWBLANK(ch)) {
			if (ch != ' ' && cnt < rewind) {
				/* This may be different colours depending on
				 * what we find, so remember this location.
				 */
				rewind = cnt;
				rewindcol = col;
			}
			if (ch == '\t')
				col = (ws->mycol|7)+1;
			else
				col += 1;
			cnt += 1;
		}
		if (ch != WEOF)
			doc_prev(focus, m);
		if (ws->mycol == 0 && ws->indent_space && rewind == 0) {
			/* Indents must be space, but this is something else,
			 * so highlight all the indent
			 */
			doc_move(focus, m, -cnt);
			attr_set_str(&m->attrs, "render:whitespace",
				     "bg:red");
			attr_set_int(&m->attrs, "attr-len", cnt);
			return;
		}
		/*
		 * 'm' is just after last blank. ch is next (non-blank)
		 * char.  'cnt' is the number of blanks.
		 * 'rewind' is distance from start where first non-space seen.
		 */
		if (ch == WEOF || is_eol(ch)) {
			/* Blanks all the way to EOL.  This is highlighted unless
			 * point is at EOL
			 */
			struct mark *p = call_ret(mark, "doc:point",
						  focus);
			if (p && mark_same(m, p))
				ch = 'x';
		}
		if (ch == WEOF || is_eol(ch)) {
			doc_move(focus, m, -cnt);
			/* Set the blanks at EOL to red */
			attr_set_str(&m->attrs, "render:whitespace",
				     "bg:red");
			attr_set_int(&m->attrs, "attr-len", cnt);
			return;
		}
		if (rewind > cnt) {
			/* no non-space, nothing to do here */
			ws->mycol = col;
			continue;
		}

		/* That first blank is no RED, set normal colour */
		doc_move(focus, m, rewind - cnt);
		ws->mycol = rewindcol;

		/* handle tab */
		/* If previous is non-tab, then RED, else YELLOW */
		ch = doc_prior(focus, m);
		ch2 = doc_following(focus, m);
		if (ch2 == '\t' && ch != WEOF && ch != '\t' && ISWBLANK(ch))
			/* Tab after non-tab blank - bad */
			attr_set_str(&m->attrs, "render:whitespace",
				     "bg:red-80");
		else if (ch2 == '\t')
			attr_set_str(&m->attrs, "render:whitespace",
				     "bg:yellow-80+80");
		else
			/* non-space or tab, must be unicode blank of some sort */
			attr_set_str(&m->attrs, "render:whitespace",
				     "bg:red-80+80");
		attr_set_int(&m->attrs, "attr-len", 1);

		return;
	}
	attr_set_str(&m->attrs, "render:whitespace", NULL);
}

DEF_CMD(ws_attrs)
{
	struct ws_info *ws = ci->home->data;

	if (!ci->str || !ci->mark)
		return Enoarg;
	if (strcmp(ci->str, "start-of-line") == 0) {
		if (ws->mymark)
			mark_free(ws->mymark);
		ws->mymark = NULL;
		ws->mycol = 0;
		choose_next(ci->focus, ci->mark, ws, 0);
		return Efallthrough;
	}
	if (ci->mark == ws->mymark &&
	    strcmp(ci->str, "render:whitespace") == 0) {
		char *s = strsave(ci->focus, ci->str2);
		int len = attr_find_int(ci->mark->attrs, "attr-len");
		if (len <= 0)
			len = 1;
		choose_next(ci->focus, ci->mark, ws, len);
		return comm_call(ci->comm2, "attr:callback", ci->focus, len,
				 ci->mark, s, 10);
	}
	return Efallthrough;
}

DEF_CMD(ws_close)
{
	struct ws_info *ws = ci->home->data;

	mark_free(ws->mymark);
	ws->mymark = NULL;
	return 1;
}

static struct map *ws_map safe;
DEF_LOOKUP_CMD(whitespace_handle, ws_map);

static struct pane *ws_attach(struct pane *f safe)
{
	struct ws_info *ws;
	struct pane *p;
	char *w;

	alloc(ws, pane);

	w = pane_attr_get(f, "whitespace-width");
	if (w) {
		ws->warn_width = atoi(w);
		if (ws->warn_width < 8)
			ws->warn_width = INT_MAX;
	} else
		ws->warn_width = 80;

	w = pane_attr_get(f, "whitespace-indent-space");
	if (w && strcasecmp(w, "no") != 0)
		ws->indent_space = True;
	w = pane_attr_get(f, "whitespace-max-spaces");
	if (w) {
		ws->max_spaces = atoi(w);
		if (ws->max_spaces < 1)
			ws->max_spaces = 7;
	} else
		ws->max_spaces = INT_MAX;

	w = pane_attr_get(f, "whitespace-single-blank-lines");
	if (w && strcasecmp(w, "no") != 0)
		ws->single_blanks = True;

	p = pane_register(f, 0, &whitespace_handle.c, ws);
	if (!p)
		unalloc(ws, pane);
	return p;
}

DEF_CMD(ws_clone)
{
	struct pane *p;

	p = ws_attach(ci->focus);
	if (p)
		pane_clone_children(ci->home, p);
	return 1;
}

DEF_CMD(whitespace_attach)
{
	struct pane *p;

	p = ws_attach(ci->focus);
	if (!p)
		return Efail;
	return comm_call(ci->comm2, "callback:attach", p);

}

DEF_CMD(whitespace_activate)
{
	struct pane *p;
	char *v, *vn = NULL;

	p = call_ret(pane, "attach-whitespace", ci->focus);
	if (!p)
		return Efail;
	v = pane_attr_get(p, "view-default");
	asprintf(&vn, "%s%swhitespace", v?:"", v?",":"");
	if (vn) {
		call("doc:set:view-default", p, 0, NULL, vn);
		free(vn);
	}
	return 1;
}

void edlib_init(struct pane *ed safe)
{
	ws_map = key_alloc();

	key_add(ws_map, "map-attr", &ws_attrs);
	key_add(ws_map, "Close", &ws_close);
	key_add(ws_map, "Free", &edlib_do_free);
	key_add(ws_map, "Clone", &ws_clone);
	call_comm("global-set-command", ed, &whitespace_attach,
		  0, NULL, "attach-whitespace");
	call_comm("global-set-command", ed, &whitespace_activate,
		  0, NULL, "interactive-cmd-whitespace-mode");

}

