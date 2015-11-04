/*
 * Define some keystrokes to create an editor with an
 * "emacs" feel.
 *
 * We register an 'emacs' mode and associate keys with that
 * in the global keymap.
 */
#include <unistd.h>
#include <stdlib.h>
#include <curses.h>
#include <wchar.h>
#include <fcntl.h>
#include <string.h>

#include "core.h"
#include "pane.h"
#include "view.h"

#include "extras.h"



static int emacs_move(struct command *c, struct cmd_info *ci);
static int emacs_delete(struct command *c, struct cmd_info *ci);

static struct move_command {
	struct command	cmd;
	char		*type;
	int		direction;
	char		*k1, *k2, *k3;
} move_commands[] = {
	{CMD(emacs_move, "forward-char"), "Move-Char", 1,
	 "emacs-C-Chr-F", "emacs-Right", NULL},
	{CMD(emacs_move, "backward-char"), "Move-Char", -1,
	 "emacs-C-Chr-B", "emacs-Left", NULL},
	{CMD(emacs_move, "forward_word"), "Move-Word", 1,
	 "emacs-M-Chr-f", "emacs-M-Right", NULL},
	{CMD(emacs_move, "backward-word"), "Move-Word", -1,
	 "emacs-M-Chr-b", "emacs-M-Left", NULL},
	{CMD(emacs_move, "forward_WORD"), "Move-WORD", 1,
	 "emacs-M-Chr-F", NULL, NULL},
	{CMD(emacs_move, "backward-WORD"), "Move-WORD", -1,
	 "emacs-M-Chr-B", NULL, NULL},
	{CMD(emacs_move, "end-of-line"), "Move-EOL", 1,
	 "emacs-C-Chr-E", "emacs-End", NULL},
	{CMD(emacs_move, "start-of-line"), "Move-EOL", -1,
	 "emacs-C-Chr-A", "emacs-Home", NULL},
	{CMD(emacs_move, "prev-line"), "Move-Line", -1,
	 "emacs-C-Chr-P", "emacs-Up", NULL},
	{CMD(emacs_move, "next-line"), "Move-Line", 1,
	 "emacs-C-Chr-N", "emacs-Down", NULL},
	{CMD(emacs_move, "end-of-file"), "Move-File", 1,
	 "emacs-M-Chr->", "emacs-S-End", NULL},
	{CMD(emacs_move, "start-of-file"), "Move-File", -1,
	 "emacs-M-Chr-<", "emacs-S-Home", NULL},
	{CMD(emacs_move, "page-down"), "Move-View-Large", 1,
	 "emacs-Next", "emacs-C-Chr-V", NULL},
	{CMD(emacs_move, "page-up"), "Move-View-Large", -1,
	 "emacs-Prior", "emacs-M-Chr-v", NULL},

	{CMD(emacs_delete, "delete-next"), "Move-Char", 1,
	 "emacs-C-Chr-D", "emacs-Del", "emacs-del"},
	{CMD(emacs_delete, "delete-back"), "Move-Char", -1,
	 "emacs-C-Chr-H", "emacs-Backspace", NULL},
	{CMD(emacs_delete, "delete-word"), "Move-Word", 1,
	 "emacs-M-Chr-d", NULL, NULL},
	{CMD(emacs_delete, "delete-back-word"), "Move-Word", -1,
	 "emacs-M-C-Chr-H", "emacs-M-Backspace", NULL},
	{CMD(emacs_delete, "delete-eol"), "Move-EOL", 1,
	 "emacs-C-Chr-K", NULL, NULL},
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
	{CMD(emacs_str, "pane-next"), "WindowOP", "next", "emCX-Chr-o"},
	{CMD(emacs_str, "pane-prev"), "WindowOP", "prev", "emCX-Chr-O"},
	{CMD(emacs_str, "pane-wider"), "WindowOP", "x+", "emCX-Chr-}"},
	{CMD(emacs_str, "pane-narrower"), "WindowOP", "x-", "emCX-Chr-{"},
	{CMD(emacs_str, "pane-taller"), "WindowOP", "y+", "emCX-Chr-^"},
	{CMD(emacs_str, "pane-split-below"), "WindowOP", "split-y", "emCX-Chr-2"},
	{CMD(emacs_str, "pane-split-right"), "WindowOP", "split-x", "emCX-Chr-3"},
	{CMD(emacs_str, "pane-close"), "WindowOP", "close", "emCX-Chr-0"},
	{CMD(emacs_str, "abort"), "Misc", "exit", "emCX-C-Chr-C"},
	{CMD(emacs_str, "redraw"), "Misc", "refresh", "C-Chr-L"},
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
	strncpy(str,ci->key+6+4, sizeof(str));
	str[4] = 0;
	ci2.str = str;
	ci2.pointp = ci->pointp;
	ret = key_handle_focus(&ci2);
	pane_set_extra(ci->home, 1);

	return ret;
}
DEF_CMD(comm_insert, emacs_insert, "insert-key");

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
		if (strcmp(other_inserts[i].key, ci->key+6) == 0)
			break;
	if (other_inserts[i].key == NULL)
		return 0;

	ci2.str = other_inserts[i].insert;
	ci2.pointp = ci->pointp;
	ret = key_handle_focus(&ci2);
	pane_set_extra(p, 0); /* A newline starts a new undo */
	return ret;
}
DEF_CMD(comm_insert_other, emacs_insert_other, "insert-other");

static int emacs_undo(struct command *c, struct cmd_info *ci)
{
	struct point *pt = *ci->pointp;
	doc_undo(pt, 0);
	pane_damaged(ci->home->focus, DAMAGED_CURSOR);
	return 1;
}
DEF_CMD(comm_undo, emacs_undo, "undo");

static int emacs_redo(struct command *c, struct cmd_info *ci)
{
	struct point *pt = *ci->pointp;
	doc_undo(pt, 1);
	pane_damaged(ci->home->focus, DAMAGED_CURSOR);
	return 1;
}
DEF_CMD(comm_redo, emacs_redo, "redo");

static int emacs_findfile(struct command *c, struct cmd_info *ci)
{
	int fd;
	struct doc *d;
	struct point *pt;
	struct pane *p, *par;

	if (strcmp(ci->key, "File Found") != 0) {
		popup_register(ci->focus, "Find File", "/home/neilb/", "File Found");
		return 1;
	}
	p = ci->focus;
	while (p && !p->point)
		p = p->parent;
	if (!p || !p->parent)
		return 0;
	par = p->parent;
	/* par is the tile */
	pane_close(p);

	fd = open(ci->str, O_RDONLY);
	d = doc_new(pane2ed(ci->home), "text");
	if (fd >= 0) {
		doc_load_file(d, NULL, fd);
		close(fd);
	}
	p = view_attach(par, d, NULL, 1);
	pt = p->parent->point;
	if (fd < 0) {
		bool first=1;
		doc_replace(pt, NULL, "File not found: ", &first);
		doc_replace(pt, NULL, ci->str, &first);
		doc_replace(pt, NULL, "\n", &first);
		point_reset(pt);
	}
	render_attach("text", p, pt);
	pane_focus(p);
	return 1;
}
DEF_CMD(comm_findfile, emacs_findfile, "find-file");

static int emacs_meta(struct command *c, struct cmd_info *ci)
{
	pane_set_mode(ci->home, "emacs-M-", 1);
	pane_set_numeric(ci->home, ci->numeric);
	pane_set_extra(ci->home, ci->extra);
	return 1;
}
DEF_CMD(comm_meta, emacs_meta, "meta");

static int emacs_raw(struct command *c, struct cmd_info *ci)
{
	struct cmd_info ci2 = *ci;

	if (strncmp(ci->key, "emacs-", 6) != 0)
		return 0;
	ci2.key = ci->key + 6;
	if (ci->x >= 0)
		return key_handle_xy(&ci2);
	else
		return key_handle_focus(&ci2);
}
DEF_CMD(comm_raw, emacs_raw, "modeless-passthrough");

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
DEF_CMD(comm_num, emacs_num, "numeric-prefix");

void emacs_register(struct map *m)
{
	unsigned i;
	struct command *cx_cmd = key_register_prefix("emCX-");

	key_add(m, "emacs-C-Chr-X", cx_cmd);
	key_add(m, "emacs-ESC", &comm_meta);

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

	key_add_range(m, "emacs-Chr- ", "emacs-Chr-~", &comm_insert);
	key_add(m, "emacs-Tab", &comm_insert_other);
	key_add(m, "emacs-LF", &comm_insert_other);
	key_add(m, "emacs-Return", &comm_insert_other);

	key_add(m, "emacs-C-Chr-_", &comm_undo);
	key_add(m, "emacs-M-C-Chr-_", &comm_redo);

	key_add(m, "emCX-C-Chr-F", &comm_findfile);
	key_add(m, "File Found", &comm_findfile);

	/* A simple mouse click just gets resent without a mode */
	key_add(m, "emacs-Click-1", &comm_raw);

	key_add_range(m, "emacs-M-Chr-0", "emacs-M-Chr-9", &comm_num);
}
