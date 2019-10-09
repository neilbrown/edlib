/*
 * Copyright Neil Brown ©2019 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * This module provides highlighting of interesting white-space,
 * and possibly other spacing-related issues.
 * Initially:
 *  tabs are in a different color
 *  space at EOL are READ
 *  space TAB after space are READ
 *
 * This is achieved by capturing the "start-of-line" attribute request,
 * reporting attributes that apply to leading chars, and placing a
 * mark with a "render:whitespace" attribute at the next interesting place, if
 * there is one.
 */

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "core.h"

struct ws_info {
	struct mark *mymark;
};

static void choose_next(struct pane *focus safe, struct mark *pm safe,
                        struct ws_info *ws safe)
{
	struct mark *m;

	if (ws->mymark == NULL) {
		m = mark_dup(pm);
		ws->mymark = m;
	} else {
		/* Need to look beyond the current location */
		m = ws->mymark;
		mark_to_mark(m, pm);
		mark_next_pane(focus, m);
	}

	while(1) {
		wint_t ch = doc_following_pane(focus, m);
		if (ch == WEOF || is_eol(ch))
			break;
		if (ch == ' ' || ch == '\t') {
			/* If only spaces/tabs until EOL, then RED, else keep looking */
			int cnt = 0;
			int rewind = 1000;
			while ((ch = mark_next_pane(focus, m)) == ' ' || ch == '\t') {
				if (ch == '\t' && cnt < rewind)
					rewind = cnt;
				cnt += 1;
			}
			if (ch != WEOF)
				mark_prev_pane(focus, m);
			/* 'm' is just after last spc/tab. - ch is next char.
			 * 'cnt' is the number of chars, including first, all spc or tab
			 * rewind is distance from start where first tab seen.
			 */
			if (ch == WEOF || is_eol(ch)) {
				struct mark *p = call_ret(mark, "doc:point", focus);
				if (p && mark_same(m, p))
					ch = 'x';
			}
			if (ch == WEOF || is_eol(ch)) {
				while (cnt--)
					mark_prev_pane(focus, m);
				/* Set the first space/tab to red */
				attr_set_str(&m->attrs, "render:whitespace", "bg:red");
				return;
			}
			if (rewind > cnt)
				/* no tabs, don't check all the spaces again */
				continue;

			while (cnt-- > rewind)
				mark_prev_pane(focus, m);

			/* handle tab */
			/* If previous is space, then RED, else YELLOW */
			if (doc_prior_pane(focus, m) == ' ')
				attr_set_str(&m->attrs, "render:whitespace", "bg:red");
			else
				attr_set_str(&m->attrs, "render:whitespace", "bg:yellow");
			return;
		}
		mark_next_pane(focus, m);
	}
	attr_set_str(&m->attrs, "render:whitespace", NULL);
}

DEF_CMD(ws_attrs)
{
	struct ws_info *ws = ci->home->data;

	if (!ci->str || !ci->mark)
		return 0;
	if (strcmp(ci->str, "start-of-line") == 0) {
		if (ws->mymark)
			mark_free(ws->mymark);
		ws->mymark = NULL;
		choose_next(ci->focus, ci->mark, ws);
		return 0;
	}
	if (ci->mark == ws->mymark && strcmp(ci->str, "render:whitespace") == 0) {
		char *s = strsave(ci->focus, ci->str2);
		choose_next(ci->focus, ci->mark, ws);
		return comm_call(ci->comm2, "attr:callback", ci->focus, 1,
		                 ci->mark, s, 10);
	}
	return 0;
}

DEF_CMD(ws_close)
{
	struct ws_info *ws = ci->home->data;

	mark_free(ws->mymark);
	ws->mymark = NULL;
	free(ws);
	ci->home->data = safe_cast NULL;
	return 1;
}

static struct map *ws_map safe;
DEF_LOOKUP_CMD(whitespace_handle, ws_map);

DEF_CMD(ws_clone)
{
	struct ws_info *ws = calloc(1, sizeof(*ws));
	struct pane *p;

	p = pane_register(ci->focus, 0, &whitespace_handle.c, ws, NULL);
	pane_clone_children(ci->home, p);
	return 0;
}

DEF_CMD(whitespace_attach)
{
	struct ws_info *ws = calloc(1, sizeof(*ws));

	return comm_call(ci->comm2, "callback:attach",
	                 pane_register(ci->focus, 0, &whitespace_handle.c, ws, NULL));
}

void edlib_init(struct pane *ed safe)
{
	ws_map = key_alloc();

	key_add(ws_map, "map-attr", &ws_attrs);
	key_add(ws_map, "Close", &ws_close);
	key_add(ws_map, "Clone", &ws_clone);
	call_comm("global-set-command", ed, &whitespace_attach, 0, NULL, "attach-whitespace");
}

