/*
 * Copyright Neil Brown <neil@brown.name> 2015
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * Minor mode for emacs incremental search.
 *
 * emacs-search creates a popup search box and the attaches this mode.
 * We send a popup-get-target message to collect the target pane.
 * We have a stack of "string,pos" for repeated search requests.
 * We capture "Refresh" to repeat search.
 * We send "Move-View-Pos" to target to get it to refresh to a new location.
 * We capture:
 *   C-s - if we have a match, save end of match as new start
 *   Backspace - is search string is same as saved start, pop
 *   C-w - collect word from target and add to search string
 *   C-c - collect char from target and add to search string.
 *   C-r - search backwards.. tricky.
 *
 */

#include <stdlib.h>
#include <string.h>
#include "core.h"

struct es_info {
	struct stk {
		struct stk *next;
		struct mark *m; /* Start of search */
		int len; /* current length of match string */
	} *s;
	struct mark *start; /* where searching starts */
	struct point *end; /* where last success ended */
	struct pane *target;
	struct command watch;
};

static struct map *es_map;

static int do_search_forward(struct command *c, struct cmd_info *ci)
{
	return 1;
}
DEF_CMD(search_forward, do_search_forward);

static int do_search_retreat(struct command *c, struct cmd_info *ci)
{
	struct es_info *esi = ci->home->data;

	if (esi->s == NULL)
		return 0;
	return 1;
}
DEF_CMD(search_retreat, do_search_retreat);

static int do_search_add(struct command *c, struct cmd_info *ci)
{
	struct es_info *esi = ci->home->data;
	struct doc *d = esi->end->doc;
	wint_t wch;
	char b[5];
	struct cmd_info ci2 = {0};

	do {
		/* TEMP HACK - please fix */
		d->ops->set_attr(esi->end, "highlight", NULL);
		wch = mark_next(d, mark_of_point(esi->end));
		if (wch == WEOF)
			return 1;
		if (wch == '\n') {
			/* ugly hack */
			mark_prev(d, mark_of_point(esi->end));
			return 1;
		}
		b[0] = wch;
		b[1] = 0;
		ci2.key = "Replace";
		ci2.str = b;
		ci2.numeric = 1;
		ci2.mark = NULL;
		ci2.pointp = ci->pointp;
		ci2.focus = ci->focus;
		key_handle_focus(&ci2);
	} while (strcmp(ci->key, "C-Chr-C") != 0 && wch != ' ');
	return 1;
}
DEF_CMD(search_add, do_search_add);

static int do_search_backward(struct command *c, struct cmd_info *ci)
{
	return 1;
}
DEF_CMD(search_backward, do_search_backward);

static int do_search_refresh(struct command *c, struct cmd_info *ci)
{
	pane_check_size(ci->focus);
	return 1;
}
DEF_CMD(search_refresh, do_search_refresh);

static int do_search_close(struct command *c, struct cmd_info *ci)
{
	struct es_info *esi = ci->focus->data;

	doc_del_view(esi->target->point->doc, &esi->watch);
	/* TEMP HACK - please fix */
	esi->target->point->doc->ops->set_attr(esi->end, "highlight", NULL);
	point_free(esi->end);
	mark_free(esi->start);
	while (esi->s) {
		struct stk *n = esi->s;
		esi->s = n->next;
		mark_free(n->m);
		free(n);
	}
	return 1;
}
DEF_CMD(search_close, do_search_close);

static int do_search_again(struct command *c, struct cmd_info *ci)
{
	/* document has changed, retry search */
	struct es_info *esi = container_of(c, struct es_info, watch);
	struct cmd_info ci2 = {0};
	struct doc *d = esi->end->doc;

	/* TEMP HACK - please fix */
	d->ops->set_attr(esi->end, "highlight", NULL);
	ci2.pointp = &esi->end;
	ci2.mark = mark_dup(esi->start, 1);
	ci2.str = doc_getstr((*ci->pointp)->doc, NULL, NULL);
	ci2.key = "text-search";
	if (!key_lookup(pane2ed(esi->target)->commands, &ci2))
		ci2.extra = -1;
	if (ci2.extra > 0) {
		point_to_mark(esi->end, ci2.mark);
		/* TEMP HACK - please fix */
		d->ops->set_attr(esi->end, "highlight","fg:red,inverse");
		ci2.key = "Move-View-Pos";
		ci2.focus = esi->target;
		key_handle_focus(&ci2);
	}
	mark_free(ci2.mark);
	free(ci2.str);
	return 1;
}

static void emacs_search_init_map(void)
{
	es_map = key_alloc();
	key_add(es_map, "C-Chr-S", &search_forward);
	key_add(es_map, "Backspace", &search_retreat);
	key_add(es_map, "C-Chr-W", &search_add);
	key_add(es_map, "C-Chr-C", &search_add);
	key_add(es_map, "C-Chr-R", &search_backward);
	key_add(es_map, "Refresh", &search_refresh);
	key_add(es_map, "Close", &search_close);
}

static int do_search_handle(struct command *c, struct cmd_info *ci)
{
	return key_lookup(es_map, ci);
}
DEF_CMD(search_handle, do_search_handle);

static int emacs_search(struct command *c, struct cmd_info *ci)
{
	struct pane *p;
	struct es_info *esi;
	struct cmd_info ci2 = {0};
	struct point **ptp;

	if (!es_map)
		emacs_search_init_map();
	ci2.key = "popup:get-target";
	ci2.focus = ci->focus;
	if (key_handle_focus(&ci2) == 0)
		return 0;
	esi = malloc(sizeof(*esi));
	while (ci2.focus && ! ci2.focus->point)
		ci2.focus = ci2.focus->parent;
	if (!ci2.focus) {
		free(esi);
		return 0;
	}
	esi->target = ci2.focus;
	point_dup(esi->target->point, &esi->end);
	esi->start = mark_dup(mark_of_point(esi->end), 1);
	esi->s = NULL;
	esi->watch.func = do_search_again;
	ptp = pane_point(ci->focus);
	doc_add_view((*ptp)->doc, &esi->watch);

	while (pane_child(ci->focus))
		ci->focus = pane_child(ci->focus);
	p = pane_register(ci->focus, 0, &search_handle, esi, NULL);
	ci->home = p;
	return 1;
}
DEF_CMD(comm_emacs_search, emacs_search);

void emacs_search_init(struct editor *ed)
{
	key_add(ed->commands, "attach-emacs-search", &comm_emacs_search);
}
