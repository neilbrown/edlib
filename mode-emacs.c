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

#include "core.h"
#include "keymap.h"
#include "pane.h"
#include "view.h"

#include "extras.h"



static int emacs_move(struct command *c, struct cmd_info *ci);
static int emacs_delete(struct command *c, struct cmd_info *ci);

static struct move_command {
	struct command	cmd;
	int		type;
	int		direction;
	wint_t		k1, k2, k3;
} move_commands[] = {
	{CMD(emacs_move, "forward-char"), MV_CHAR, 1,
	 KCTRL('F'), FUNC_KEY(KEY_RIGHT), 0},
	{CMD(emacs_move, "backward-char"), MV_CHAR, -1,
	 KCTRL('B'), FUNC_KEY(KEY_LEFT), 0},
	{CMD(emacs_move, "forward_word"), MV_WORD, 1,
	 META('f'), META(FUNC_KEY(KEY_RIGHT)), 0},
	{CMD(emacs_move, "backward-word"), MV_WORD, -1,
	 META('b'), META(FUNC_KEY(KEY_LEFT)), 0},
	{CMD(emacs_move, "forward_WORD"), MV_WORD2, 1,
	 META('F'), 0, 0},
	{CMD(emacs_move, "backward-WORD"), MV_WORD2, -1,
	 META('B'), 0, 0},
	{CMD(emacs_move, "end-of-line"), MV_EOL, 1,
	 KCTRL('E'), 0, 0},
	{CMD(emacs_move, "start-of-line"), MV_EOL, -1,
	 KCTRL('A'), 0, 0},
	{CMD(emacs_move, "prev-line"), MV_LINE, -1,
	 KCTRL('P'), FUNC_KEY(KEY_UP), 0},
	{CMD(emacs_move, "next-line"), MV_LINE, 1,
	 KCTRL('N'), FUNC_KEY(KEY_DOWN), 0},
	{CMD(emacs_move, "end-of-file"), MV_FILE, 1,
	 META('>'), 0, 0},
	{CMD(emacs_move, "start-of-file"), MV_FILE, -1,
	 META('<'), 0, 0},
	{CMD(emacs_move, "page-down"), MV_VIEW_LARGE, 1,
	 FUNC_KEY(KEY_NPAGE), 0, 0},
	{CMD(emacs_move, "page-up"), MV_VIEW_LARGE, -1,
	 FUNC_KEY(KEY_PPAGE), 0, 0},

	{CMD(emacs_delete, "delete-next"), MV_CHAR, 1,
	 KCTRL('D'), FUNC_KEY(KEY_DC), 0x7f},
	{CMD(emacs_delete, "delete-back"), MV_CHAR, -1,
	 KCTRL('H'), FUNC_KEY(KEY_BACKSPACE), 0},
	{CMD(emacs_delete, "delete-word"), MV_WORD, 1,
	 META('d'), 0, 0},
	{CMD(emacs_delete, "delete-back-word"), MV_WORD, -1,
	 META(KCTRL('H')), META(FUNC_KEY(KEY_BACKSPACE)), 0},
	{CMD(emacs_delete, "delete-eol"), MV_EOL, 1,
	 KCTRL('K'), 0, 0},
};

static int emacs_move(struct command *c, struct cmd_info *ci)
{
	struct move_command *mv = container_of(c, struct move_command, cmd);
	struct pane *view_pane = ci->point_pane;
	struct point *pt = view_pane->point;
	int old_x = -1;
	struct cmd_info ci2 = {0};
	int ret = 0;

	old_x = view_pane->cx;

	ci2.focus = ci->focus;
	ci2.key = mv->type;
	ci2.numeric = mv->direction * RPT_NUM(ci);
	ci2.mark = mark_of_point(pt);
	ci2.point_pane = ci->point_pane;
	ret = key_handle_focus(&ci2);

	if (!ret)
		return 0;

	if (mv->type == MV_VIEW_LARGE && old_x >= 0) {
		/* Might have lost the cursor - place it at top or
		 * bottom of view
		 */
		ci2.focus = ci->focus;
		ci2.key = MV_CURSOR_XY;
		ci2.numeric = 1;
		ci2.x = old_x;
		if (mv->direction == 1)
			ci2.y = 0;
		else
			ci2.y = view_pane->h-1;
		ci2.point_pane = ci->point_pane;
		key_handle_xy(&ci2);
	}

	pane_damaged(ci->point_pane->focus, DAMAGED_CURSOR);

	return ret;
}

static int emacs_delete(struct command *c, struct cmd_info *ci)
{
	struct move_command *mv = container_of(c, struct move_command, cmd);
	struct cmd_info ci2 = {0};
	int ret = 0;
	struct mark *m;
	struct doc *d = ci->point_pane->point->doc;

	m = mark_at_point(ci->point_pane->point, MARK_UNGROUPED);
	ci2.focus = ci->focus;
	ci2.key = mv->type;
	ci2.numeric = mv->direction * RPT_NUM(ci);
	if (mv->type == MV_EOL && ci2.numeric == 1 &&
	    doc_following(d, m) == '\n')
		ci2.key = MV_CHAR;
	ci2.mark = m;
	ci2.point_pane = ci->point_pane;
	ret = key_handle_focus(&ci2);
	if (!ret) {
		mark_free(m);
		return 0;
	}
	ci2.focus = ci->focus;
	ci2.key = EV_REPLACE;
	ci2.numeric = 1;
	ci2.extra = ci->extra;
	ci2.mark = m;
	ci2.str = NULL;
	ci2.point_pane = ci->point_pane;
	ret = key_handle_focus(&ci2);
	mark_free(m);
	pane_set_extra(ci->point_pane, 1);

	return ret;
}

static int emacs_str(struct command *c, struct cmd_info *ci);
static struct str_command {
	struct command	cmd;
	int		type;
	char		*str;
	int		c_x;
	wint_t		k;
} str_commands[] = {
	{CMD(emacs_str, "pane-next"), EV_WINDOW, "next", 1, 'o'},
	{CMD(emacs_str, "pane-prev"), EV_WINDOW, "prev", 1, 'O'},
	{CMD(emacs_str, "pane-wider"), EV_WINDOW, "x+", 1, '}'},
	{CMD(emacs_str, "pane-narrower"), EV_WINDOW, "x-", 1, '{'},
	{CMD(emacs_str, "pane-taller"), EV_WINDOW, "y+", 1, '^'},
	{CMD(emacs_str, "pane-split-below"), EV_WINDOW, "split-y", 1, '2'},
	{CMD(emacs_str, "pane-split-right"), EV_WINDOW, "split-x", 1, '3'},
	{CMD(emacs_str, "pane-close"), EV_WINDOW, "close", 1, '0'},
	{CMD(emacs_str, "abort"), EV_MISC, "exit", 1, KCTRL('c')},
	{CMD(emacs_str, "redraw"), EV_MISC, "refresh", 0, KCTRL('l')},
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
	char str[2];
	struct cmd_info ci2 = {0};
	int ret;

	ci2.focus = ci->focus;
	ci2.key = EV_REPLACE;
	ci2.numeric = 1;
	ci2.extra = ci->extra;
	ci2.mark = mark_of_point(ci->point_pane->point);
	str[0] = ci->key;
	str[1] = 0;
	ci2.str = str;
	ci2.point_pane = ci->point_pane;
	ret = key_handle_focus(&ci2);
	pane_set_extra(ci->focus, 1);

	return ret;
}
DEF_CMD(comm_insert, emacs_insert, "insert-key");

static int emacs_insert_nl(struct command *c, struct cmd_info *ci)
{
	struct pane *p = ci->focus;
	char str[2];
	struct cmd_info ci2 = {0};
	int ret;

	ci2.focus = ci->focus;
	ci2.key = EV_REPLACE;
	ci2.numeric = 1;
	ci2.extra = ci->extra;
	ci2.mark = mark_of_point(ci->point_pane->point);
	str[0] = '\n';
	str[1] = 0;
	ci2.str = str;
	ci2.point_pane = ci->point_pane;
	ret = key_handle_focus(&ci2);
	pane_set_extra(p, 0); /* A newline starts a new undo */
	return ret;
}
DEF_CMD(comm_insert_nl, emacs_insert_nl, "insert-nl");

static int emacs_undo(struct command *c, struct cmd_info *ci)
{
	struct point *pt = ci->point_pane->point;
	doc_undo(pt, 0);
	pane_damaged(ci->focus->focus, DAMAGED_CURSOR);
	return 1;
}
DEF_CMD(comm_undo, emacs_undo, "undo");

static int emacs_redo(struct command *c, struct cmd_info *ci)
{
	struct point *pt = ci->point_pane->point;
	doc_undo(pt, 1);
	pane_damaged(ci->focus->focus, DAMAGED_CURSOR);
	return 1;
}
DEF_CMD(comm_redo, emacs_redo, "redo");

static int emacs_findfile(struct command *c, struct cmd_info *ci)
{
	int fd;
	struct doc *d;
	struct point *pt;
	struct pane *p;

	if (ci->key != EV_USER_DEF(1)) {
		popup_register(ci->point_pane, "Find File", "/home/neilb/", EV_USER_DEF(1));
		return 1;
	}
	fd = open(ci->str, O_RDONLY);
	d = doc_new("text");
	p = ci->point_pane->parent;
	pane_close(ci->point_pane);
	p = view_attach(p, d, 1);
	pt = p->parent->point;
	if (fd >= 0)
		doc_load_file(pt, fd);
	else {
		bool first=1;
		doc_replace(pt, NULL, "File not found: ", &first);
		doc_replace(pt, NULL, ci->str, &first);
		doc_replace(pt, NULL, "\n", &first);
	}
	point_reset(pt);
	render_text_attach(p, pt);
	return 1;
}
DEF_CMD(comm_findfile, emacs_findfile, "find-file");

static int emacs_meta(struct command *c, struct cmd_info *ci)
{
	pane_set_mode(ci->focus, META(Kmod(ci->key)), 1);
	pane_set_numeric(ci->focus, ci->numeric);
	pane_set_extra(ci->focus, ci->extra);
	return 1;
}
DEF_CMD(comm_meta, emacs_meta, "meta");

static int emacs_raw(struct command *c, struct cmd_info *ci)
{
	struct cmd_info ci2 = *ci;

	ci2.key = Kkey(ci->key);
	if (ci->x >= 0)
		return key_handle_xy(&ci2);
	else
		return key_handle_focus(&ci2);
}
DEF_CMD(comm_raw, emacs_raw, "modeless-passthrough");

static int emacs_num(struct command *c, struct cmd_info *ci)
{
	int rpt = RPT_NUM(ci);

	if (ci->numeric == NO_NUMERIC)
		rpt = 0;
	rpt = rpt * 10 + Kkey(ci->key) - '0';
	pane_set_numeric(ci->focus, rpt);
	pane_set_extra(ci->focus, ci->extra);
	return 1;
}
DEF_CMD(comm_num, emacs_num, "numeric-prefix");

void emacs_register(struct map *m)
{
	unsigned i;
	int c_x;
	int emacs;
	struct command *cx_cmd = key_register_mode("C-x", &c_x);

	key_register_mode("emacs", &emacs);
	key_add(m, K_MOD(emacs, KCTRL('X')), cx_cmd);
	key_add(m, K_MOD(emacs, KCTRL('[')), &comm_meta);

	for (i = 0; i < ARRAY_SIZE(move_commands); i++) {
		struct move_command *mc = &move_commands[i];
		key_add(m, K_MOD(emacs, mc->k1), &mc->cmd);
		if (mc->k2)
			key_add(m, K_MOD(emacs, mc->k2), &mc->cmd);
		if (mc->k3)
			key_add(m, K_MOD(emacs, mc->k3), &mc->cmd);
	}

	for (i = 0; i < ARRAY_SIZE(str_commands); i++) {
		struct str_command *sc = &str_commands[i];
		if (sc->c_x)
			key_add(m, K_MOD(c_x, sc->k), &sc->cmd);
		else
			key_add(m, K_MOD(emacs, sc->k), &sc->cmd);
	}

	key_add_range(m, K_MOD(emacs, ' '), K_MOD(emacs, '~'), &comm_insert);
	key_add(m, K_MOD(emacs, '\t'), &comm_insert);
	key_add(m, K_MOD(emacs, '\n'), &comm_insert);
	key_add(m, K_MOD(emacs, '\r'), &comm_insert_nl);

	key_add(m, K_MOD(emacs, KCTRL('_')), &comm_undo);
	key_add(m, K_MOD(emacs, META(KCTRL('_'))), &comm_redo);

	key_add(m, K_MOD(c_x, KCTRL('f')), &comm_findfile);
	key_add(m, EV_USER_DEF(1), &comm_findfile);

	/* A simple mouse click just gets resent without a mode */
	key_add(m, K_MOD(emacs, M_CLICK(0)), &comm_raw);

	key_add_range(m, K_MOD(emacs, META('0')), K_MOD(emacs, META('9')), &comm_num);
}
