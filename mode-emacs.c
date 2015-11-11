/*
 * Copyright Neil Brown <neil@brown.name> 2015
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * Define some keystrokes to create an editor with an
 * "emacs" feel.
 *
 * We register an 'emacs' mode and associate keys with that
 * in the global keymap.
 */
#include <unistd.h>
#include <stdlib.h>
#include <wchar.h>
#include <fcntl.h>
#include <string.h>

#include "core.h"

static int emacs_move(struct command *c, struct cmd_info *ci);
static int emacs_delete(struct command *c, struct cmd_info *ci);

static struct move_command {
	struct command	cmd;
	char		*type;
	int		direction;
	char		*k1, *k2, *k3;
} move_commands[] = {
	{CMD(emacs_move), "Move-Char", 1,
	 "C-Chr-F", "Right", NULL},
	{CMD(emacs_move), "Move-Char", -1,
	 "C-Chr-B", "Left", NULL},
	{CMD(emacs_move), "Move-Word", 1,
	 "M-Chr-f", "M-Right", NULL},
	{CMD(emacs_move), "Move-Word", -1,
	 "M-Chr-b", "M-Left", NULL},
	{CMD(emacs_move), "Move-WORD", 1,
	 "M-Chr-F", NULL, NULL},
	{CMD(emacs_move), "Move-WORD", -1,
	 "M-Chr-B", NULL, NULL},
	{CMD(emacs_move), "Move-EOL", 1,
	 "C-Chr-E", "End", NULL},
	{CMD(emacs_move), "Move-EOL", -1,
	 "C-Chr-A", "Home", NULL},
	{CMD(emacs_move), "Move-Line", -1,
	 "C-Chr-P", "Up", NULL},
	{CMD(emacs_move), "Move-Line", 1,
	 "C-Chr-N", "Down", NULL},
	{CMD(emacs_move), "Move-File", 1,
	 "M-Chr->", "S-End", NULL},
	{CMD(emacs_move), "Move-File", -1,
	 "M-Chr-<", "S-Home", NULL},
	{CMD(emacs_move), "Move-View-Large", 1,
	 "Next", "C-Chr-V", NULL},
	{CMD(emacs_move), "Move-View-Large", -1,
	 "Prior", "M-Chr-v", NULL},

	{CMD(emacs_delete), "Move-Char", 1,
	 "C-Chr-D", "Del", "del"},
	{CMD(emacs_delete), "Move-Char", -1,
	 "C-Chr-H", "Backspace", NULL},
	{CMD(emacs_delete), "Move-Word", 1,
	 "M-Chr-d", NULL, NULL},
	{CMD(emacs_delete), "Move-Word", -1,
	 "M-C-Chr-H", "M-Backspace", NULL},
	{CMD(emacs_delete), "Move-EOL", 1,
	 "C-Chr-K", NULL, NULL},
};

static int emacs_move(struct command *c, struct cmd_info *ci)
{
	struct move_command *mv = container_of(c, struct move_command, cmd);
	struct pane *cursor_pane = pane_with_cursor(ci->home, NULL, NULL);
	struct point *pt = *ci->pointp;
	int old_x = -1;
	struct cmd_info ci2 = {0};
	int ret = 0;

	if (!cursor_pane)
		return 0;
	old_x = cursor_pane->cx;

	ci2.focus = ci->focus;
	ci2.key = mv->type;
	ci2.numeric = mv->direction * RPT_NUM(ci);
	ci2.mark = mark_of_point(pt);
	ci2.pointp = ci->pointp;
	ret = key_handle_focus(&ci2);

	if (!ret)
		return 0;

	if (strcmp(mv->type, "Move-View-Large") == 0 && old_x >= 0) {
		/* Might have lost the cursor - place it at top or
		 * bottom of view
		 */
		ci2.focus = cursor_pane;
		ci2.key = "Move-CursorXY";
		ci2.numeric = 1;
		ci2.x = old_x;
		if (mv->direction == 1)
			ci2.y = 0;
		else
			ci2.y = cursor_pane->h - 1;
		ci2.pointp = ci->pointp;
		key_handle_xy(&ci2);
	}

	pane_damaged(cursor_pane, DAMAGED_CURSOR);

	return ret;
}

static int emacs_delete(struct command *c, struct cmd_info *ci)
{
	struct move_command *mv = container_of(c, struct move_command, cmd);
	struct cmd_info ci2 = {0};
	int ret = 0;
	struct mark *m;
	struct doc *d = ci->pointp[0]->doc;

	m = mark_at_point(*ci->pointp, MARK_UNGROUPED);
	ci2.focus = ci->focus;
	ci2.key = mv->type;
	ci2.numeric = mv->direction * RPT_NUM(ci);
	if (strcmp(mv->type, "Move-EOL") == 0 && ci2.numeric == 1 &&
	    doc_following(d, m) == '\n')
		ci2.key = "Move-Char";
	ci2.mark = m;
	ci2.pointp = ci->pointp;
	ret = key_handle_focus(&ci2);
	if (!ret) {
		mark_free(m);
		return 0;
	}
	ci2.focus = ci->focus;
	ci2.key = "Replace";
	ci2.numeric = 1;
	ci2.extra = ci->extra;
	ci2.mark = m;
	ci2.str = NULL;
	ci2.pointp = ci->pointp;
	ret = key_handle_focus(&ci2);
	mark_free(m);
	pane_set_extra(ci->home, 1);

	return ret;
}

static int emacs_str(struct command *c, struct cmd_info *ci);
static struct str_command {
	struct command	cmd;
	char		*type;
	char		*str;
	char		*k;
} str_commands[] = {
	{CMD(emacs_str), "WindowOP", "next", "emCX-Chr-o"},
	{CMD(emacs_str), "WindowOP", "prev", "emCX-Chr-O"},
	{CMD(emacs_str), "WindowOP", "x+", "emCX-Chr-}"},
	{CMD(emacs_str), "WindowOP", "x-", "emCX-Chr-{"},
	{CMD(emacs_str), "WindowOP", "y+", "emCX-Chr-^"},
	{CMD(emacs_str), "WindowOP", "close-others", "emCX-Chr-1"},
	{CMD(emacs_str), "WindowOP", "split-y", "emCX-Chr-2"},
	{CMD(emacs_str), "WindowOP", "split-x", "emCX-Chr-3"},
	{CMD(emacs_str), "WindowOP", "close", "emCX-Chr-0"},
	{CMD(emacs_str), "Misc", "exit", "emCX-C-Chr-C"},
	{CMD(emacs_str), "Misc", "refresh", "C-Chr-L"},
	{CMD(emacs_str), "Abort", NULL, "C-Chr-G"},
	{CMD(emacs_str), "NOP", NULL, "M-Chr-G"},
	{CMD(emacs_str), "NOP", NULL, "emCX-C-Chr-G"},
	{CMD(emacs_str), "NOP", NULL, "emCX4-C-Chr-G"},
};

static int emacs_str(struct command *c, struct cmd_info *ci)
{
	struct str_command *sc = container_of(c, struct str_command, cmd);
	struct cmd_info ci2;

	ci2 = *ci;
	ci2.key = sc->type;
	ci2.str = sc->str;
	return key_handle_focus(&ci2);
}

static int emacs_insert(struct command *c, struct cmd_info *ci)
{
	char str[5];
	struct cmd_info ci2 = {0};
	int ret;

	ci2.focus = ci->focus;
	ci2.key = "Replace";
	ci2.numeric = 1;
	ci2.extra = ci->extra;
	ci2.mark = mark_of_point(*ci->pointp);
	strncpy(str,ci->key+4, sizeof(str));
	str[4] = 0;
	ci2.str = str;
	ci2.pointp = ci->pointp;
	ret = key_handle_focus(&ci2);
	pane_set_extra(ci->home, 1);

	return ret;
}
DEF_CMD(comm_insert, emacs_insert);

static struct {
	char *key;
	char *insert;
} other_inserts[] = {
	{"Tab", "\t"},
	{"LF", "\n"},
	{"Return", "\n"},
	{NULL, NULL}
};

static int emacs_insert_other(struct command *c, struct cmd_info *ci)
{
	struct pane *p = ci->home;
	struct cmd_info ci2 = {0};
	int ret;
	int i;

	ci2.focus = ci->focus;
	ci2.key = "Replace";
	ci2.numeric = 1;
	ci2.extra = ci->extra;
	ci2.mark = mark_of_point(*ci->pointp);
	for (i = 0; other_inserts[i].key; i++)
		if (strcmp(other_inserts[i].key, ci->key) == 0)
			break;
	if (other_inserts[i].key == NULL)
		return 0;

	ci2.str = other_inserts[i].insert;
	ci2.pointp = ci->pointp;
	ret = key_handle_focus(&ci2);
	pane_set_extra(p, 0); /* A newline starts a new undo */
	return ret;
}
DEF_CMD(comm_insert_other, emacs_insert_other);

static int emacs_undo(struct command *c, struct cmd_info *ci)
{
	struct point *pt = *ci->pointp;
	doc_undo(pt, 0);
	return 1;
}
DEF_CMD(comm_undo, emacs_undo);

static int emacs_redo(struct command *c, struct cmd_info *ci)
{
	struct point *pt = *ci->pointp;
	doc_undo(pt, 1);
	return 1;
}
DEF_CMD(comm_redo, emacs_redo);

static int emacs_findfile(struct command *c, struct cmd_info *ci)
{
	int fd;
	struct pane *p, *par;
	struct cmd_info ci2 = {0};

	if (strncmp(ci->key, "File Found", 10) != 0) {
		char *path = NULL;
		char buf[PATH_MAX];
		struct point **ptp;
		struct doc *d;

		if (ci->pointp) {
			path = doc_attr((*ci->pointp)->doc, NULL, 0, "filename");
			if (path) {
				strcpy(buf, path);
				path = strrchr(buf, '/');
				if (path)
					path[1] = '\0';
				path = buf;
			}
		}
		if (!path)
			path = realpath(".", buf);
		if (!path)
			path = "/";
		p = pane_attach(ci->focus, "popup", NULL, "D2");
		if (!p)
			return 0;

		ptp = pane_point(p);
		d = (*ptp)->doc;
		if (strncmp(ci->key, "emCX4-", 6) == 0) {
			attr_set_str(&p->attrs, "prefix",
				     "Find File Other Window: ", -1);
			attr_set_str(&p->attrs, "done-key",
				     "File Found Other Window", -1);
		} else {
			attr_set_str(&p->attrs, "prefix", "Find File: ", -1);
			attr_set_str(&p->attrs, "done-key", "File Found", -1);
		}
		doc_set_name(d, "Find File");
		if (path) {
			struct cmd_info ci2 = {0};
			ci2.key = "Replace";
			ci2.focus = p;
			ci2.str = path;
			key_handle_focus(&ci2);
		}
		return 1;
	}

	if (strcmp(ci->key, "File Found Other Window") == 0) {
		ci2.key = "OtherPane";
		ci2.focus = ci->focus;
		key_handle_focus(&ci2);
		p = ci2.focus;
	} else
		p = ci->focus;

	par = p;
	while (p && !p->point)
		p = p->parent;
	if (p && p->parent)
		par = p->parent;
	/* par is the tile */
	if (par->focus)
		pane_close(par->focus);

	fd = open(ci->str, O_RDONLY);
	if (fd >= 0) {
		p = doc_open(par, fd, ci->str, NULL);
		close(fd);
	} else
		p = doc_from_text(par, ci->str, "File not found\n");
	pane_focus(p);
	return 1;
}
DEF_CMD(comm_findfile, emacs_findfile);

static int emacs_finddoc(struct command *c, struct cmd_info *ci)
{
	struct pane *p, *par;
	struct doc *d;
	struct point *pt;
	struct cmd_info ci2 = {0};

	if (strncmp(ci->key, "Doc Found", 9) != 0) {
		struct point **ptp;

		p = pane_attach(ci->focus, "popup", NULL, "D2");
		if (!p)
			return 0;
		ptp = pane_point(p);
		d = (*ptp)->doc;
		if (strncmp(ci->key, "emCX4-", 6) == 0) {
			attr_set_str(&p->attrs, "prefix",
				     "Find Document Other Window: ", -1);
			attr_set_str(&p->attrs, "done-key",
				     "Doc Found Other Window", -1);
		} else {
			attr_set_str(&p->attrs, "prefix", "Find Document: ", -1);
			attr_set_str(&p->attrs, "done-key", "Doc Found", -1);
		}
		doc_set_name(d, "Find Document");
		return 1;
	}

	if (strcmp(ci->key, "Doc Found Other Window") == 0) {
		ci2.key = "OtherPane";
		ci2.focus = ci->focus;
		key_handle_focus(&ci2);
		p = ci2.focus;
	} else
		p = ci->focus;

	par = p;
	while (p && !p->point)
		p = p->parent;
	if (p && p->parent)
		par = p->parent;
	/* par is the tile */

	d = doc_find(pane2ed(par), ci->str);
	if (!d)
		return 1;
	if (par->focus)
		pane_close(par->focus);
	point_new(d, &pt);
	p = pane_attach(par, "view", pt, NULL);
	if (!p) {
		point_free(pt);
		return 0;
	}
	render_attach(NULL, p);
	return 1;
}
DEF_CMD(comm_finddoc, emacs_finddoc);

static int emacs_viewdocs(struct command *c, struct cmd_info *ci)
{
	struct pane *p, *par;
	struct doc *d;
	struct point *pt;

	p = ci->focus;
	while (p && !p->point)
		p = p->parent;
	if (!p || !p->parent)
		return 0;
	par = p->parent;
	/* par is the tile */

	d = doc_find(pane2ed(p), "*Documents*");
	if (!d)
		return 1;
	pane_close(p);
	point_new(d, &pt);
	p = pane_attach(par, "view", pt, NULL);
	if (!p) {
		point_free(pt);
		return 0;
	}
	render_attach(NULL, p);
	return 1;
}
DEF_CMD(comm_viewdocs, emacs_viewdocs);

static int emacs_meta(struct command *c, struct cmd_info *ci)
{
	pane_set_mode(ci->home, "M-", 1);
	pane_set_numeric(ci->home, ci->numeric);
	pane_set_extra(ci->home, ci->extra);
	return 1;
}
DEF_CMD(comm_meta, emacs_meta);

static int emacs_num(struct command *c, struct cmd_info *ci)
{
	int rpt = RPT_NUM(ci);
	char *last = ci->key + strlen(ci->key)-1;

	if (ci->numeric == NO_NUMERIC)
		rpt = 0;
	rpt = rpt * 10 + *last - '0';
	pane_set_numeric(ci->home, rpt);
	pane_set_extra(ci->home, ci->extra);
	return 1;
}
DEF_CMD(comm_num, emacs_num);

static int emacs_kill_doc(struct command *c, struct cmd_info *ci)
{
	struct doc *d;
	if (!ci->pointp)
		return 0;
	d = (*ci->pointp)->doc;
	doc_close_views(d);
	doc_destroy(d);
	return 1;
}
DEF_CMD(comm_kill_doc, emacs_kill_doc);

static int emacs_search(struct command *c, struct cmd_info *ci)
{
	struct cmd_info ci2 = {0};

	if (strcmp(ci->key, "Search String") != 0) {
		struct pane *p = pane_attach(ci->focus, "popup", NULL, "TR2");
		struct point **ptp;
		if (!p)
			return 0;
		attr_set_str(&p->attrs, "prefix", "Search: ", -1);
		attr_set_str(&p->attrs, "done-key", "Search String", -1);
		ptp = pane_point(p);
		doc_set_name((*ptp)->doc, "Search");
		while (pane_child(p))
			p = pane_child(p);
		pane_attach(p, "emacs-search", NULL, NULL);
		return 1;
	}

	if (!ci->str || !ci->str[0])
		return -1;
	ci2.pointp = pane_point(ci->focus);
	ci2.mark = mark_dup(mark_of_point(*ci2.pointp), 1);
	ci2.str = ci->str;
	ci2.key = "text-search";
	if (!key_lookup(pane2ed(ci->focus)->commands, &ci2))
		ci2.extra = -1;
	if (ci2.extra > 0)
		point_to_mark(*ci2.pointp, ci2.mark);
	mark_free(ci2.mark);
	return 1;
}
DEF_CMD(comm_search, emacs_search);

static struct map *emacs_map;

static void emacs_init(void)
{
	unsigned i;
	struct command *cx_cmd = key_register_prefix("emCX-");
	struct command *cx4_cmd = key_register_prefix("emCX4-");
	struct map *m = key_alloc();

	key_add(m, "C-Chr-X", cx_cmd);
	key_add(m, "emCX-Chr-4", cx4_cmd);
	key_add(m, "ESC", &comm_meta);

	for (i = 0; i < ARRAY_SIZE(move_commands); i++) {
		struct move_command *mc = &move_commands[i];
		key_add(m, mc->k1, &mc->cmd);
		if (mc->k2)
			key_add(m, mc->k2, &mc->cmd);
		if (mc->k3)
			key_add(m, mc->k3, &mc->cmd);
	}

	for (i = 0; i < ARRAY_SIZE(str_commands); i++) {
		struct str_command *sc = &str_commands[i];
		key_add(m, sc->k, &sc->cmd);
	}

	key_add_range(m, "Chr- ", "Chr-~", &comm_insert);
	key_add(m, "Tab", &comm_insert_other);
	key_add(m, "LF", &comm_insert_other);
	key_add(m, "Return", &comm_insert_other);

	key_add(m, "C-Chr-_", &comm_undo);
	key_add(m, "M-C-Chr-_", &comm_redo);

	key_add(m, "emCX-C-Chr-F", &comm_findfile);
	key_add(m, "emCX4-C-Chr-F", &comm_findfile);
	key_add(m, "emCX4-Chr-f", &comm_findfile);
	key_add(m, "File Found", &comm_findfile);
	key_add(m, "File Found Other Window", &comm_findfile);

	key_add(m, "emCX-Chr-b", &comm_finddoc);
	key_add(m, "emCX4-Chr-b", &comm_finddoc);
	key_add(m, "Doc Found", &comm_finddoc);
	key_add(m, "Doc Found Other Window", &comm_finddoc);
	key_add(m, "emCX-C-Chr-B", &comm_viewdocs);

	key_add(m, "emCX-Chr-k", &comm_kill_doc);

	key_add(m, "C-Chr-S", &comm_search);
	key_add(m, "Search String", &comm_search);

	key_add_range(m, "M-Chr-0", "M-Chr-9", &comm_num);
	emacs_map = m;
}

static int do_mode_emacs(struct command *c, struct cmd_info *ci)
{
	return key_lookup(emacs_map, ci);
}
DEF_CMD(mode_emacs, do_mode_emacs);

void emacs_search_init(struct editor *ed);
void edlib_init(struct editor *ed)
{
	if (emacs_map == NULL)
		emacs_init();
	key_add(ed->commands, "mode-emacs", &mode_emacs);
	emacs_search_init(ed);
}
