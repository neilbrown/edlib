/*
 * A buffer can be viewed in a pane.
 * The pane is (typically) a tile in a display.
 * As well as content from the buffer, a 'view' provides
 * a scroll bar and a status line.
 * These server to visually separate different views from each other.
 *
 * For now, a cheap hack to just show the scroll bar and status line.
 */

#include <unistd.h>
#include <stdlib.h>
#include <curses.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#include "core.h"
#include "pane.h"
#include "keymap.h"

#include "extras.h"

#define ARRAY_SIZE(ra) (sizeof(ra) / sizeof(ra[0]))
struct view_data {
	bool		first_change;
	int		border;
	struct command	ch_notify;
	int		ch_notify_num;
	struct pane	*pane;
};

static int view_refresh(struct pane *p, struct pane *point_pane, int damage)
{
	int i;
	int mid = (p->h-1)/2;
	struct view_data *vd = p->data;
	struct point *pt = p->point;
	int ln, l,w,c;
	char msg[60];

	if (!vd->border)
		return 0;
	if (damage & DAMAGED_SIZE) {
		pane_resize(p, 0, 0, p->parent->w, p->parent->h);
		pane_resize(p->focus, 1, 0, p->w-1, p->h-1);
	}

	count_calculate(pt->doc, NULL, mark_of_point(pt), &ln, &w, &c);
	count_calculate(pt->doc, NULL, NULL, &l, &w,  &c);

	for (i = 0; i < p->h-1; i++)
		pane_text(p, '|', A_STANDOUT, 0, i);
	mid = 1 + (p->h-4) * ln / l;
	pane_text(p, '^', 0, 0, mid-1);
	pane_text(p, '#', A_STANDOUT, 0, mid);
	pane_text(p, 'v', 0, 0, mid+1);
	pane_text(p, '+', A_STANDOUT, 0, p->h-1);
	p->cx = 0; p->cy = p->h - 1;
	for (i = 1; i < p->w; i++)
		pane_text(p, '=', A_STANDOUT, i, p->h-1);

	snprintf(msg, sizeof(msg), "L%d W%d C%d", l,w,c);
	for (i = 0; msg[i] && i+4 < p->w; i++)
		pane_text(p, msg[i], A_STANDOUT, i+4, p->h-1);
	return 0;
}

#define	CMD(func, name) {func, name, view_refresh}
#define	DEF_CMD(comm, func, name) static struct command comm = CMD(func, name)

static int view_null(struct pane *p, struct pane *point_pane, int damage)
{

	return 0;
}

static int view_notify(struct command *c, struct cmd_info *ci)
{
	struct view_data *vd;
	if (ci->key != EV_REPLACE)
		return 0;

	vd = container_of(c, struct view_data, ch_notify);
	pane_damaged(vd->pane, DAMAGED_CONTENT);
	return 0;
}

struct pane *view_attach(struct pane *par, struct doc *d, int border)
{
	struct view_data *vd;
	struct pane *p;

	vd = malloc(sizeof(*vd));
	vd->first_change = 1;
	vd->border = border;
	vd->ch_notify.func = view_notify;
	vd->ch_notify.name = "view-notify";
	vd->ch_notify.type = NULL;
	vd->ch_notify_num = doc_add_type(d, &vd->ch_notify);

	p = pane_register(par, 0, view_refresh, vd, NULL);
	point_new(d, p);
	vd->pane = p;

	pane_resize(p, 0, 0, par->w, par->h);
	p = pane_register(p, 0, view_null, vd, NULL);
	p->parent->focus = p;
	if (vd->border)
		pane_resize(p, 1, 0, par->w-1, par->h-1);
	else
		pane_resize(p, 0, 0, par->w, par->h);
	pane_damaged(p, DAMAGED_SIZE);
	/* It is expected that some other handler will take
	 * over this pane
	 */
	pane_focus(p);
	return p;
}

static int view_char(struct command *c, struct cmd_info *ci)
{
	struct point *pt = ci->point_pane->point;
	int rpt = RPT_NUM(ci);

	while (rpt > 0) {
		if (mark_next(pt->doc, ci->mark) == WEOF)
			break;
		rpt -= 1;
	}
	while (rpt < 0) {
		if (mark_prev(pt->doc, ci->mark) == WEOF)
			break;
		rpt += 1;
	}

	return 1;
}
DEF_CMD(comm_char, view_char, "move-char");

static int view_word(struct command *c, struct cmd_info *ci)
{
	struct point *pt = ci->point_pane->point;
	int rpt = RPT_NUM(ci);

	/* We skip spaces, then either alphanum or non-space/alphanum */
	while (rpt > 0) {
		while (iswspace(doc_following(pt->doc, ci->mark)))
			mark_next(pt->doc, ci->mark);
		if (iswalnum(doc_following(pt->doc, ci->mark))) {
			while (iswalnum(doc_following(pt->doc, ci->mark)))
				mark_next(pt->doc, ci->mark);
		} else {
			wint_t wi;
			while ((wi=doc_following(pt->doc, ci->mark)) != WEOF &&
			       !iswspace(wi) && !iswalnum(wi))
				mark_next(pt->doc, ci->mark);
		}
		rpt -= 1;
	}
	while (rpt < 0) {
		while (iswspace(doc_prior(pt->doc, ci->mark)))
			mark_prev(pt->doc, ci->mark);
		if (iswalnum(doc_prior(pt->doc, ci->mark))) {
			while (iswalnum(doc_prior(pt->doc, ci->mark)))
				mark_prev(pt->doc, ci->mark);
		} else {
			wint_t wi;
			while ((wi=doc_prior(pt->doc, ci->mark)) != WEOF &&
			       !iswspace(wi) && !iswalnum(wi))
				mark_prev(pt->doc, ci->mark);
		}
		rpt += 1;
	}

	return 1;
}
DEF_CMD(comm_word, view_word, "move-word");

static int view_WORD(struct command *c, struct cmd_info *ci)
{
	struct point *pt = ci->point_pane->point;
	int rpt = RPT_NUM(ci);

	/* We skip spaces, then non-spaces */
	while (rpt > 0) {
		wint_t wi;
		while (iswspace(doc_following(pt->doc, ci->mark)))
			mark_next(pt->doc, ci->mark);

		while ((wi=doc_following(pt->doc, ci->mark)) != WEOF &&
		       !iswspace(wi))
			mark_next(pt->doc, ci->mark);
		rpt -= 1;
	}
	while (rpt < 0) {
		wint_t wi;
		while (iswspace(doc_prior(pt->doc, ci->mark)))
			mark_prev(pt->doc, ci->mark);
		while ((wi=doc_prior(pt->doc, ci->mark)) != WEOF &&
		       !iswspace(wi))
			mark_prev(pt->doc, ci->mark);
		rpt += 1;
	}

	return 1;
}
DEF_CMD(comm_WORD, view_WORD, "move-WORD");

static int view_eol(struct command *c, struct cmd_info *ci)
{
	struct point *pt = ci->point_pane->point;
	wint_t ch = 1;
	int rpt = RPT_NUM(ci);

	while (rpt > 0 && ch != WEOF) {
		while ((ch = mark_next(pt->doc, ci->mark)) != WEOF &&
		       ch != '\n')
			;
		rpt -= 1;
	}
	while (rpt < 0 && ch != WEOF) {
		while ((ch = mark_prev(pt->doc, ci->mark)) != WEOF &&
		       ch != '\n')
			;
		rpt += 1;
	}
	if (ch == '\n') {
		if (RPT_NUM(ci) > 0)
			mark_prev(pt->doc, ci->mark);
		else if (RPT_NUM(ci) < 0)
			mark_next(pt->doc, ci->mark);
	}
	return 1;
}
DEF_CMD(comm_eol, view_eol, "move-end-of-line");

static int view_line(struct command *c, struct cmd_info *ci)
{
	struct point *pt = ci->point_pane->point;
	wint_t ch = 1;
	int rpt = RPT_NUM(ci);

	while (rpt > 0 && ch != WEOF) {
		while ((ch = mark_next(pt->doc, ci->mark)) != WEOF &&
		       ch != '\n')
			;
		rpt -= 1;
	}
	while (rpt < 0 && ch != WEOF) {
		while ((ch = mark_prev(pt->doc, ci->mark)) != WEOF &&
		       ch != '\n')
			;
		rpt += 1;
	}
	return 1;
}
DEF_CMD(comm_line, view_line, "move-by-line");

static int view_file(struct command *c, struct cmd_info *ci)
{
	struct point *pt = ci->point_pane->point;
	wint_t ch = 1;
	int rpt = RPT_NUM(ci);

	if (ci->mark == NULL)
		ci->mark = mark_of_point(pt);
	while (rpt > 0 && ch != WEOF) {
		while ((ch = mark_next(pt->doc, ci->mark)) != WEOF)
			;
		rpt = 0;
	}
	while (rpt < 0 && ch != WEOF) {
		while ((ch = mark_prev(pt->doc, ci->mark)) != WEOF)
			;
		rpt = 0;
	}
	return 1;
}
DEF_CMD(comm_file, view_file, "move-end-of-file");

static int view_page(struct command *c, struct cmd_info *ci)
{
	struct point *pt = ci->point_pane->point;
	wint_t ch = 1;
	int rpt = RPT_NUM(ci);

	rpt *= ci->focus->h-2;
	while (rpt > 0 && ch != WEOF) {
		while ((ch = mark_next(pt->doc, ci->mark)) != WEOF &&
		       ch != '\n')
			;
		rpt -= 1;
	}
	while (rpt < 0 && ch != WEOF) {
		while ((ch = mark_prev(pt->doc, ci->mark)) != WEOF &&
		       ch != '\n')
			;
		rpt += 1;
	}
	return 1;
}
DEF_CMD(comm_page, view_page, "move-page");

static int view_move(struct command *c, struct cmd_info *ci);
static int view_delete(struct command *c, struct cmd_info *ci);

#define CTRL(X) ((X) & 0x1f)
static struct move_command {
	struct command	cmd;
	int		type;
	int		direction;
	wint_t		k1, k2, k3;
} move_commands[] = {
	{CMD(view_move, "forward-char"), MV_CHAR, 1,
	 CTRL('F'), FUNC_KEY(KEY_RIGHT), 0},
	{CMD(view_move, "backward-char"), MV_CHAR, -1,
	 CTRL('B'), FUNC_KEY(KEY_LEFT), 0},
	{CMD(view_move, "forward_word"), MV_WORD, 1,
	 META('f'), META(FUNC_KEY(KEY_RIGHT)), 0},
	{CMD(view_move, "backward-word"), MV_WORD, -1,
	 META('b'), META(FUNC_KEY(KEY_LEFT)), 0},
	{CMD(view_move, "forward_WORD"), MV_WORD2, 1,
	 META('F'), 0, 0},
	{CMD(view_move, "backward-WORD"), MV_WORD2, -1,
	 META('B'), 0, 0},
	{CMD(view_move, "end-of-line"), MV_EOL, 1,
	 CTRL('E'), 0, 0},
	{CMD(view_move, "start-of-line"), MV_EOL, -1,
	 CTRL('A'), 0, 0},
	{CMD(view_move, "prev-line"), MV_LINE, -1,
	 CTRL('P'), FUNC_KEY(KEY_UP), 0},
	{CMD(view_move, "next-line"), MV_LINE, 1,
	 CTRL('N'), FUNC_KEY(KEY_DOWN), 0},
	{CMD(view_move, "end-of-file"), MV_FILE, 1,
	 META('>'), 0, 0},
	{CMD(view_move, "start-of-file"), MV_FILE, -1,
	 META('<'), 0, 0},
	{CMD(view_move, "page-down"), MV_VIEW_LARGE, 1,
	 FUNC_KEY(KEY_NPAGE), 0, 0},
	{CMD(view_move, "page-up"), MV_VIEW_LARGE, -1,
	 FUNC_KEY(KEY_PPAGE), 0, 0},

	{CMD(view_delete, "delete-next"), MV_CHAR, 1,
	 CTRL('D'), FUNC_KEY(KEY_DC), 0x7f},
	{CMD(view_delete, "delete-back"), MV_CHAR, -1,
	 CTRL('H'), FUNC_KEY(KEY_BACKSPACE), 0},
	{CMD(view_delete, "delete-word"), MV_WORD, 1,
	 META('d'), 0, 0},
	{CMD(view_delete, "delete-back-word"), MV_WORD, -1,
	 META(CTRL('H')), META(FUNC_KEY(KEY_BACKSPACE)), 0},
	{CMD(view_delete, "delete-eol"), MV_EOL, 1,
	 CTRL('K'), 0, 0},
};

static int view_move(struct command *c, struct cmd_info *ci)
{
	struct pane *p = ci->focus;
	struct move_command *mv = container_of(c, struct move_command, cmd);
	struct view_data *vd = p->data;
	struct pane *view_pane = p->focus;
	struct point *pt = ci->point_pane->point;
	int old_x = -1;
	struct cmd_info ci2 = {0};
	int ret = 0;

	if (view_pane)
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

	pane_damaged(ci->focus->focus, DAMAGED_CURSOR);
	vd->first_change = 1;

	return ret;
}

static int view_delete(struct command *c, struct cmd_info *ci)
{
	struct move_command *mv = container_of(c, struct move_command, cmd);
	struct cmd_info ci2 = {0};
	int ret = 0;
	struct mark *m;

	m = mark_at_point(ci->point_pane->point, MARK_UNGROUPED);
	ci2.focus = ci->focus;
	ci2.key = mv->type;
	ci2.numeric = mv->direction * RPT_NUM(ci);
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
	ci2.mark = m;
	ci2.str = NULL;
	ci2.point_pane = ci->point_pane;
	ret = key_handle_focus(&ci2);
	mark_free(m);

	return ret;
}

static int view_insert(struct command *c, struct cmd_info *ci)
{
	char str[2];
	struct cmd_info ci2 = {0};
	int ret;

	ci2.focus = ci->focus;
	ci2.key = EV_REPLACE;
	ci2.numeric = 1;
	ci2.mark = mark_of_point(ci->point_pane->point);
	str[0] = ci->key;
	str[1] = 0;
	ci2.str = str;
	ci2.point_pane = ci->point_pane;
	ret = key_handle_focus(&ci2);

	return ret;
}
DEF_CMD(comm_insert, view_insert, "insert-key");

static int view_insert_nl(struct command *c, struct cmd_info *ci)
{
	struct pane *p = ci->focus;
	struct view_data *vd = p->data;
	char str[2];
	struct cmd_info ci2 = {0};
	int ret;

	ci2.focus = ci->focus;
	ci2.key = EV_REPLACE;
	ci2.numeric = 1;
	ci2.mark = mark_of_point(ci->point_pane->point);
	str[0] = '\n';
	str[1] = 0;
	ci2.str = str;
	ci2.point_pane = ci->point_pane;
	ret = key_handle_focus(&ci2);
	vd->first_change = 1;
	return ret;
}
DEF_CMD(comm_insert_nl, view_insert_nl, "insert-nl");

static int view_replace(struct command *c, struct cmd_info *ci)
{
	struct pane *p = ci->focus;
	struct view_data *vd = p->data;
	struct point *pt = ci->point_pane->point;

	doc_replace(pt, ci->mark, ci->str, &vd->first_change);
	return 1;
}
DEF_CMD(comm_replace, view_replace, "do-replace");

static int view_click(struct command *c, struct cmd_info *ci)
{
	struct pane *p = ci->focus;
	int mid = (p->h-1)/2;
	struct cmd_info ci2 = {0};

	if (ci->x != 0)
		return 0;

	ci2.focus = p->focus;
	ci2.key = MV_VIEW_SMALL;
	ci2.numeric = RPT_NUM(ci);
	ci2.mark = NULL;
	p = p->focus;
	if (ci->y == mid-1) {
		/* scroll up */
		ci2.numeric = -ci2.numeric;
	} else if (ci->y < mid-1) {
		/* big scroll up */
		ci2.numeric = -ci2.numeric;
		ci2.key = MV_VIEW_LARGE;
	} else if (ci->y == mid+1) {
		/* scroll down */
	} else if (ci->y > mid+1 && ci->y < p->h-1) {
		ci2.key = MV_VIEW_LARGE;
	} else
		return 0;
	ci2.point_pane = ci->point_pane;
	return key_handle_focus(&ci2);
}
DEF_CMD(comm_click, view_click, "view-click");

static int view_undo(struct command *c, struct cmd_info *ci)
{
	struct point *pt = ci->point_pane->point;
	doc_undo(pt, 0);
	pane_damaged(ci->focus->focus, DAMAGED_CURSOR);
	return 1;
}
DEF_CMD(comm_undo, view_undo, "undo");

static int view_redo(struct command *c, struct cmd_info *ci)
{
	struct point *pt = ci->point_pane->point;
	doc_undo(pt, 1);
	pane_damaged(ci->focus->focus, DAMAGED_CURSOR);
	return 1;
}
DEF_CMD(comm_redo, view_redo, "redo");

static int view_findfile(struct command *c, struct cmd_info *ci)
{
	popup_register(ci->focus, "Find File", "/home/neilb/", EV_USER_DEF(1));
	return 1;
}
DEF_CMD(comm_findfile, view_findfile, "find-file");

static int view_meta(struct command *c, struct cmd_info *ci)
{
	pane_set_mode(ci->focus, META(Kmod(ci->key)), 1);
	pane_set_numeric(ci->focus, ci->numeric);
	pane_set_extra(ci->focus, ci->extra);
	return 1;
}
DEF_CMD(comm_meta, view_meta, "meta");

void view_register(struct map *m)
{
	int c_x;
	unsigned int i;

	key_add(m, '['-64, &comm_meta);

	for (i = 0; i < ARRAY_SIZE(move_commands); i++) {
		struct move_command *mc = &move_commands[i];
		key_add(m, mc->k1, &mc->cmd);
		if (mc->k2)
			key_add(m, mc->k2, &mc->cmd);
		if (mc->k3)
			key_add(m, mc->k3, &mc->cmd);
	}
	key_add(m, MV_CHAR, &comm_char);
	key_add(m, MV_WORD, &comm_word);
	key_add(m, MV_WORD2, &comm_WORD);
	key_add(m, MV_EOL, &comm_eol);
	key_add(m, MV_LINE, &comm_line);
	key_add(m, MV_FILE, &comm_file);
	key_add(m, MV_VIEW_LARGE, &comm_page);

	key_add_range(m, ' ', '~', &comm_insert);
	key_add(m, '\t', &comm_insert);
	key_add(m, '\n', &comm_insert);
	key_add(m, '\r', &comm_insert_nl);
	key_add(m, EV_REPLACE, &comm_replace);

	key_add(m, M_CLICK(0), &comm_click);
	key_add(m, M_PRESS(0), &comm_click);

	key_add(m, CTRL('_'), &comm_undo);
	key_add(m, META(CTRL('_')), &comm_redo);

	key_register_mode("C-x", &c_x);
	key_add(m, K_MOD(c_x, 'f'), &comm_findfile);
}
