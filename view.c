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

#include "list.h"
#include "text.h"
#include "pane.h"
#include "mark.h"
#include "keymap.h"
#include "popup.h"

#define ARRAY_SIZE(ra) (sizeof(ra) / sizeof(ra[0]))
struct view_data {
	struct text	*text;
	struct point	*point;
	int		first_change;
	int		border;
};

static int view_refresh(struct pane *p, int damage)
{
	int i;
	int mid = (p->h-1)/2;
	struct view_data *vd = p->data;

	if (!vd->border)
		return 0;
	if (damage & DAMAGED_SIZE) {
		pane_resize(p, 0, 0, p->parent->w, p->parent->h);
		pane_resize(p->focus, 1, 0, p->w-1, p->h-1);
	}

	for (i = 0; i < p->h-1; i++)
		pane_text(p, '|', A_STANDOUT, 0, i);
	pane_text(p, '^', 0, 0, mid-1);
	pane_text(p, '#', A_STANDOUT, 0, mid);
	pane_text(p, 'v', 0, 0, mid+1);
	pane_text(p, '+', A_STANDOUT, 0, p->h-1);
	p->cx = 0; p->cy = p->h - 1;
	for (i = 1; i < p->w; i++)
		pane_text(p, '=', A_STANDOUT, i, p->h-1);

	return 0;
}

#define	CMD(func, name) {func, name, view_refresh}
#define	DEF_CMD(comm, func, name) static struct command comm = CMD(func, name)

static int view_null(struct pane *p, int damage)
{
	struct view_data *vd = p->data;
	struct text_ref ref = point_ref(vd->point);

	return 0;

	{
	int r = 0, c = 0;
	wint_t wi;
	while (r < p->h-2 && (wi = text_next(vd->text, &ref)) != WEOF) {
		if (wi == '\n') {
			r += 1;
			c = 0;
		} else if (wi == '\t') {
			c = (c+9)/8 * 8;
		}  else {
			pane_text(p, wi, 0, c+1, r);
			c += 1;
		}
	}
	}
}

struct pane *view_attach(struct pane *par, struct text *t, int border)
{
	struct view_data *vd;
	struct pane *p;

	vd = malloc(sizeof(*vd));
	vd->text = t;
	vd->first_change = 1;
	vd->border = border;
	point_new(t, &vd->point);
	p = pane_register(par, 0, view_refresh, vd, NULL);

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
	struct pane *p = ci->focus;
	struct view_data *vd = p->data;
	int rpt = ci->repeat;

	if (rpt == INT_MAX)
		rpt = 1;
	while (rpt > 0) {
		if (mark_next(vd->text, ci->mark) == WEOF)
			break;
		rpt -= 1;
	}
	while (rpt < 0) {
		if (mark_prev(vd->text, ci->mark) == WEOF)
			break;
		rpt += 1;
	}

	return 1;
}
DEF_CMD(comm_char, view_char, "move-char");

static int view_word(struct command *c, struct cmd_info *ci)
{
	struct pane *p = ci->focus;
	struct view_data *vd = p->data;
	int rpt = ci->repeat;

	if (rpt == INT_MAX)
		rpt = 1;
	/* We skip spaces, then either alphanum or non-space/alphanum */
	while (rpt > 0) {
		while (iswspace(mark_following(vd->text, ci->mark)))
			mark_next(vd->text, ci->mark);
		if (iswalnum(mark_following(vd->text, ci->mark))) {
			while (iswalnum(mark_following(vd->text, ci->mark)))
				mark_next(vd->text, ci->mark);
		} else {
			wint_t wi;
			while ((wi=mark_following(vd->text, ci->mark)) != WEOF &&
			       !iswspace(wi) && !iswalnum(wi))
				mark_next(vd->text, ci->mark);
		}
		rpt -= 1;
	}
	while (rpt < 0) {
		while (iswspace(mark_prior(vd->text, ci->mark)))
			mark_prev(vd->text, ci->mark);
		if (iswalnum(mark_prior(vd->text, ci->mark))) {
			while (iswalnum(mark_prior(vd->text, ci->mark)))
				mark_prev(vd->text, ci->mark);
		} else {
			wint_t wi;
			while ((wi=mark_prior(vd->text, ci->mark)) != WEOF &&
			       !iswspace(wi) && !iswalnum(wi))
				mark_prev(vd->text, ci->mark);
		}
		rpt += 1;
	}

	return 1;
}
DEF_CMD(comm_word, view_word, "move-word");

static int view_WORD(struct command *c, struct cmd_info *ci)
{
	struct pane *p = ci->focus;
	struct view_data *vd = p->data;
	int rpt = ci->repeat;

	if (rpt == INT_MAX)
		rpt = 1;
	/* We skip spaces, then non-spaces */
	while (rpt > 0) {
		wint_t wi;
		while (iswspace(mark_following(vd->text, ci->mark)))
			mark_next(vd->text, ci->mark);

		while ((wi=mark_following(vd->text, ci->mark)) != WEOF &&
		       !iswspace(wi))
			mark_next(vd->text, ci->mark);
		rpt -= 1;
	}
	while (rpt < 0) {
		wint_t wi;
		while (iswspace(mark_prior(vd->text, ci->mark)))
			mark_prev(vd->text, ci->mark);
		while ((wi=mark_prior(vd->text, ci->mark)) != WEOF &&
		       !iswspace(wi))
			mark_prev(vd->text, ci->mark);
		rpt += 1;
	}

	return 1;
}
DEF_CMD(comm_WORD, view_WORD, "move-WORD");

static int view_eol(struct command *c, struct cmd_info *ci)
{
	struct pane *p = ci->focus;
	struct view_data *vd = p->data;
	wint_t ch = 1;
	int rpt = ci->repeat;

	if (rpt == INT_MAX)
		rpt = 1;
	while (rpt > 0 && ch != WEOF) {
		while ((ch = mark_next(vd->text, ci->mark)) != WEOF &&
		       ch != '\n')
			;
		rpt -= 1;
	}
	while (rpt < 0 && ch != WEOF) {
		while ((ch = mark_prev(vd->text, ci->mark)) != WEOF &&
		       ch != '\n')
			;
		rpt += 1;
	}
	if (ch == '\n') {
		if (ci->repeat > 0)
			mark_prev(vd->text, ci->mark);
		else if (ci->repeat < 0)
			mark_next(vd->text, ci->mark);
	}
	return 1;
}
DEF_CMD(comm_eol, view_eol, "move-end-of-line");

static int view_line(struct command *c, struct cmd_info *ci)
{
	struct pane *p = ci->focus;
	struct view_data *vd = p->data;
	wint_t ch = 1;
	int rpt = ci->repeat;

	if (rpt == INT_MAX)
		rpt = 1;
	while (rpt > 0 && ch != WEOF) {
		while ((ch = mark_next(vd->text, ci->mark)) != WEOF &&
		       ch != '\n')
			;
		rpt -= 1;
	}
	while (rpt < 0 && ch != WEOF) {
		while ((ch = mark_prev(vd->text, ci->mark)) != WEOF &&
		       ch != '\n')
			;
		rpt += 1;
	}
	return 1;
}
DEF_CMD(comm_line, view_line, "move-by-line");

static int view_file(struct command *c, struct cmd_info *ci)
{
	struct pane *p = ci->focus;
	struct view_data *vd = p->data;
	wint_t ch = 1;
	int rpt = ci->repeat;

	if (ci->mark == NULL)
		ci->mark = mark_of_point(vd->point);
	if (rpt == INT_MAX)
		rpt = 1;
	while (rpt > 0 && ch != WEOF) {
		while ((ch = mark_next(vd->text, ci->mark)) != WEOF)
			;
		rpt = 0;
	}
	while (rpt < 0 && ch != WEOF) {
		while ((ch = mark_prev(vd->text, ci->mark)) != WEOF)
			;
		rpt = 0;
	}
	return 1;
}
DEF_CMD(comm_file, view_file, "move-end-of-file");

static int view_page(struct command *c, struct cmd_info *ci)
{
	struct pane *p = ci->focus;
	struct view_data *vd = p->data;
	wint_t ch = 1;
	int rpt = ci->repeat;

	if (rpt == INT_MAX)
		rpt = 1;
	rpt *= ci->focus->h-2;
	while (rpt > 0 && ch != WEOF) {
		while ((ch = mark_next(vd->text, ci->mark)) != WEOF &&
		       ch != '\n')
			;
		rpt -= 1;
	}
	while (rpt < 0 && ch != WEOF) {
		while ((ch = mark_prev(vd->text, ci->mark)) != WEOF &&
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
#define META(X) ((X) | (1<<31))
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
	int old_x = -1;
	struct cmd_info ci2 = {0};
	int ret = 0;

	if (view_pane)
		old_x = view_pane->cx;

	ci2.focus = ci->focus;
	ci2.key = mv->type;
	ci2.repeat = mv->direction * ci->repeat;
	ci2.mark = mark_of_point(vd->point);
	ret = key_handle_focus(&ci2);

	if (!ret)
		return 0;

	if (mv->type == MV_VIEW_LARGE && old_x >= 0) {
		/* Might have lost the cursor - place it at top or
		 * bottom of view
		 */
		ci2.focus = ci->focus;
		ci2.key = MV_CURSOR_XY;
		ci2.repeat = 1;
		ci2.x = old_x;
		if (mv->direction == 1)
			ci2.y = 0;
		else
			ci2.y = view_pane->h-1;
		key_handle_xy(&ci2);
	}

	pane_damaged(ci->focus->focus, DAMAGED_CURSOR);
	vd->first_change = 1;

	return ret;
}

static int view_delete(struct command *c, struct cmd_info *ci)
{
	struct pane *p = ci->focus;
	struct move_command *mv = container_of(c, struct move_command, cmd);
	struct view_data *vd = p->data;
	struct cmd_info ci2 = {0};
	int ret = 0;
	struct mark *m;

	m = mark_at_point(vd->point, MARK_UNGROUPED);
	ci2.focus = ci->focus;
	ci2.key = mv->type;
	ci2.repeat = mv->direction * ci->repeat;
	ci2.mark = m;
	ret = key_handle_focus(&ci2);
	if (!ret) {
		mark_delete(m);
		return 0;
	}
	ci2.focus = ci->focus;
	ci2.key = EV_REPLACE;
	ci2.repeat = 1;
	ci2.mark = m;
	ci2.str = NULL;
	ret = key_handle_focus(&ci2);
	mark_delete(m);

	return ret;
}

static int view_insert(struct command *c, struct cmd_info *ci)
{
	struct pane *p = ci->focus;
	struct view_data *vd = p->data;
	char str[2];
	struct cmd_info ci2 = {0};
	int ret;

	ci2.focus = ci->focus;
	ci2.key = EV_REPLACE;
	ci2.repeat = 1;
	ci2.mark = mark_of_point(vd->point);
	str[0] = ci->key;
	str[1] = 0;
	ci2.str = str;
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
	ci2.repeat = 1;
	ci2.mark = mark_of_point(vd->point);
	str[0] = '\n';
	str[1] = 0;
	ci2.str = str;
	ret = key_handle_focus(&ci2);
	vd->first_change = 1;
	return ret;
}
DEF_CMD(comm_insert_nl, view_insert_nl, "insert-nl");

static int view_replace(struct command *c, struct cmd_info *ci)
{
	struct pane *p = ci->focus;
	struct view_data *vd = p->data;

	if (!mark_same(vd->text, ci->mark, mark_of_point(vd->point))) {
		int cnt = 0;
		/* Something here do delete.  For now I need to count it. */
		if (!mark_ordered(mark_of_point(vd->point), ci->mark)) {
			/* deleting backwards, move point */
			while (!mark_same(vd->text, ci->mark, mark_of_point(vd->point))) {
				mark_prev(vd->text, mark_of_point(vd->point));
				cnt++;
			}
		} else {
			/* deleting forwards, move mark */
			while (!mark_same(vd->text, ci->mark, mark_of_point(vd->point))) {
				mark_prev(vd->text, ci->mark);
				cnt++;
			}
		}
		point_delete_text(vd->text, vd->point, cnt, &vd->first_change);
	}
	if (ci->str)
		point_insert_text(vd->text, vd->point, ci->str, &vd->first_change);
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
	ci2.repeat = ci->repeat;
	if (ci2.repeat == INT_MAX)
		ci2.repeat = 1;
	ci2.mark = NULL;
	p = p->focus;
	if (ci->y == mid-1) {
		/* scroll up */
		ci2.repeat = -ci2.repeat;
	} else if (ci->y < mid-1) {
		/* big scroll up */
		ci2.repeat = -ci2.repeat;
		ci2.key = MV_VIEW_LARGE;
	} else if (ci->y == mid+1) {
		/* scroll down */
	} else if (ci->y > mid+1 && ci->y < p->h-1) {
		ci2.key = MV_VIEW_LARGE;
	} else
		return 0;
	return key_handle_focus(&ci2);
}
DEF_CMD(comm_click, view_click, "view-click");

static int view_undo(struct command *c, struct cmd_info *ci)
{
	struct pane *p = ci->focus;
	struct view_data *vd = p->data;
	point_undo(vd->text, vd->point, 0);
	return 1;
}
DEF_CMD(comm_undo, view_undo, "undo");

static int view_redo(struct command *c, struct cmd_info *ci)
{
	struct pane *p = ci->focus;
	struct view_data *vd = p->data;
	point_undo(vd->text, vd->point, 1);
	return 1;
}
DEF_CMD(comm_redo, view_redo, "redo");

static int view_findfile(struct command *c, struct cmd_info *ci)
{
	popup_register(ci->focus, "Find File", "/home/neilb/", EV_USER_DEF(1));
	return 1;
}
DEF_CMD(comm_findfile, view_findfile, "find-file");

void view_register(struct map *m)
{
	int meta, c_x;
	struct command *cmd = key_register_mod("meta", &meta);
	unsigned int i;

	key_add(m, '['-64, cmd);
	for (i = 0; i < ARRAY_SIZE(move_commands); i++) {
		struct move_command *mc = &move_commands[i];
		if (mc->k1 == META(mc->k1))
			mc->k1 = (mc->k1 & ((1<<22)-1)) | meta;
		if (mc->k2 == META(mc->k2))
			mc->k2 = (mc->k2 & ((1<<22)-1)) | meta;
		if (mc->k3 == META(mc->k3))
			mc->k3 = (mc->k3 & ((1<<22)-1)) | meta;
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
	key_add(m, CTRL('_') | meta, &comm_redo);

	key_register_mod("C-x", &c_x);
	key_add(m, 'f' | c_x, &comm_findfile);
}
