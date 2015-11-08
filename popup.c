/*
 * Copyright Neil Brown <neil@brown.name> 2015
 * May be distrubuted under terms of GPLv2 - see file:COPYING
 *
 * popup
 *
 * A 'popup' dialogue pane can be used to enter a file name or
 * probably lots of other things.
 * It gets a high 'z' value so it obscures whatever is behind.
 *
 * As well a interacting with its own buffer, a popup can pass events
 * on to other panes, and it can disappear.
 * For now these are combined - the <ENTER> key will make the window
 * disappear and will pass a message with the content of the text
 * as a string.
 * The target pane must not disappear while the popup is active.
 * I need to find a way to control that.
 *
 * A popup is created by popup_register()
 * This is given a name, an initial content, and an event key.
 */
#define _GNU_SOURCE /*  for asprintf */
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <memory.h>
#include <ncurses.h>

#include "core.h"
#include "extras.h"

struct popup_info {
	char		*name;
	struct pane	*target, *popup;
	char		*key;
	struct doc	*doc;
};

static struct map *pp_map;

static int do_popup_refresh(struct command *c, struct cmd_info *ci)
{
	struct pane *p = ci->home;
	struct popup_info *ppi = p->data;
	int i;
	int label;

	if (strcmp(ci->key, "Close") == 0) {
		if (ppi->doc)
			doc_destroy(ppi->doc);
		free(ppi);
		/* FIXME : drop reference on ppi->doc ?? */
		return 1;
	}

	if (strcmp(ci->key, "Refresh") != 0)
		return 0;

	pane_resize(p, p->parent->w/4, p->parent->h/2-2, p->parent->w/2, 3);

	if (p->focus == NULL && !list_empty(&p->children))
		p->focus = list_first_entry(&p->children, struct pane, siblings);

	pane_resize(p->focus, 1, 1, p->w-2, 1);

	for (i = 0; i < p->h-1; i++) {
		pane_text(p, '|', A_STANDOUT, 0, i);
		pane_text(p, '|', A_STANDOUT, p->w-1, i);
	}
	for (i = 0; i < p->w-1; i++) {
		pane_text(p, '-', A_STANDOUT, i, 0);
		pane_text(p, '-', A_STANDOUT, i ,p->h-1);
	}
	pane_text(p, '/', A_STANDOUT, 0, 0);
	pane_text(p, '\\', A_STANDOUT, 0, p->h-1);
	pane_text(p, 'X', A_STANDOUT, p->w-1, 0);
	pane_text(p, '/', A_STANDOUT, p->w-1, p->h-1);

	label = (p->w - strlen(ppi->name)) / 2;
	if (label < 1)
		label = 1;
	for (i = 0; ppi->name[i]; i++)
		pane_text(p, ppi->name[i], A_STANDOUT, label+i, 0);
	return 0;
}
DEF_CMD(popup_refresh, do_popup_refresh);

static int do_popup_no_refresh(struct command *c, struct cmd_info *ci)
{
	struct pane *p = ci->home;

	if (p->focus == NULL && !list_empty(&p->children))
		p->focus = list_first_entry(&p->children, struct pane, siblings);
	if (p->data != NULL && strcmp(ci->key, "Refresh") == 0)
		pane_check_size(p);
	return 0;
}
DEF_CMD(popup_no_refresh, do_popup_no_refresh);

struct pane *popup_register(struct pane *p, char *name, char *content, char *key)
{
	/* attach to root, center, one line of content, half width of pane */
	struct pane *ret, *root, *p2;
	struct popup_info *ppi = malloc(sizeof(*ppi));
	bool first = 1;
	struct cmd_info ci;
	struct point *pt;
	char *prefix;

	root = p;
	while (root->parent)
		root = root->parent;
	ppi->name = name;
	ppi->target = p;
	ppi->key = key;
	p = pane_register(root, 1, &popup_refresh, ppi, NULL);
	ppi->popup = p;

	pane_resize(p, root->w/4, root->h/2-2, root->w/2, 3);
	p = pane_register(p, 0, &popup_no_refresh, NULL, NULL);
	pane_resize(p, 1, 1, p->parent->w-2, 1);
	pt = doc_new(pane2ed(root), "text");
	doc_set_name(pt->doc, name);
	asprintf(&prefix, "%s: ", name);
	attr_set_str(&pt->doc->attrs, "prefix", prefix, -1);
	free(prefix);
	ppi->doc = pt->doc;
	p2 = view_attach(p, pt, 0);
	doc_replace(p2->parent->point, NULL, content, &first);
	render_attach(ppi->doc->default_render, p2, p2->parent->point);
	ret = pane_register(p2->focus, 0, &popup_no_refresh, ppi, NULL);
	pane_check_size(ret);
	ret->cx = ret->cy = -1;
	ret->keymap = pp_map;
	pane_focus(ret);
	ci.key = "Move-File";
	ci.numeric =1;
	ci.focus = ret;
	ci.mark = NULL;
	ci.pointp = &p2->parent->point;
	key_handle_focus(&ci);
	return ret;
}

static int popup_done(struct command *c, struct cmd_info *ci)
{
	struct pane *p = ci->home;
	struct popup_info *ppi = p->data;
	struct cmd_info ci2;

	if (ci->str == NULL || ci->str[0] != '\n')
		return 0;

	ci2.focus = ppi->target;
	ci2.key = ppi->key;
	ci2.numeric = 1;
	ci2.str = doc_getstr(ppi->doc, NULL, NULL);
	ci2.mark = NULL;
	key_handle_focus(&ci2);
	free(ci2.str);
	pane_close(ppi->popup);
	return 1;
}
DEF_CMD(comm_done, popup_done);

void popup_init(void)
{
	pp_map = key_alloc();

	key_add(pp_map, "Replace", &comm_done);
}
