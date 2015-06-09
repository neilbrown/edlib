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

#include "list.h"
#include "text.h"
#include "pane.h"
#include "mark.h"
#include "keymap.h"

struct view_data {
	struct text	*text;
	struct point	*point;
};

int view_refresh(struct pane *p, int damage)
{
	int i;

	if (damage & DAMAGED_SIZE) {
		pane_resize(p, 0, 0, p->parent->w, p->parent->h);
		pane_resize(p->focus, 1, 0, p->w-1, p->h-1);
	}

	for (i = 0; i < p->h-1; i++)
		pane_text(p, '|', A_STANDOUT, 0, i);
	pane_text(p, '^', 0, 0, p->h/2-1);
	pane_text(p, '#', A_STANDOUT, 0, p->h/2);
	pane_text(p, 'v', 0, 0, p->h/2+1);
	pane_text(p, '+', A_STANDOUT, 0, p->h-1);
	for (i = 1; i < p->w; i++)
		pane_text(p, '=', A_STANDOUT, i, p->h-1);
	return 0;
}

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

struct pane *view_attach(struct pane *par, struct text *t)
{
	struct view_data *vd;
	struct pane *p;

	vd = malloc(sizeof(*vd));
	vd->text = t;
	point_new(t, &vd->point);
	p = pane_register(par, 0, view_refresh, vd, NULL);

	pane_resize(p, 0, 0, par->w, par->h);
	p = pane_register(p, 0, view_null, vd, NULL);
	pane_resize(p, 1, 0, par->w-1, par->h-1);
	pane_focus(p);
	/* It is expected that some other handler will take
	 * over this pane
	 */
	return p;
}

static int view_next(struct command *c, int key, struct cmd_info *ci)
{
	struct view_data *vd;
	struct pane *p = ci->focus;
	while (p && p->refresh != view_refresh)
		p = p->parent;
	if (!p)
		return 0;
	vd = p->data;
	mark_next(vd->text, mark_of_point(vd->point));
	pane_focus(p);
	return 1;
}
static struct command comm_next = { view_next, "next-char" };

static int view_prev(struct command *c, int key, struct cmd_info *ci)
{
	struct view_data *vd;
	struct pane *p = ci->focus;
	while (p && p->refresh != view_refresh)
		p = p->parent;
	if (!p)
		return 0;
	vd = p->data;
	mark_prev(vd->text, mark_of_point(vd->point));
	pane_focus(p);
	return 1;
}
static struct command comm_prev = { view_prev, "prev-char" };

static int view_sol(struct command *c, int key, struct cmd_info *ci)
{
	struct view_data *vd;
	struct pane *p = ci->focus;
	wint_t ch;
	while (p && p->refresh != view_refresh)
		p = p->parent;
	if (!p)
		return 0;
	vd = p->data;
	while ((ch = mark_prev(vd->text, mark_of_point(vd->point))) != WEOF &&
	       ch != '\n')
		;
	if (ch == '\n')
		mark_next(vd->text, mark_of_point(vd->point));
	pane_focus(p);
	return 1;
}
static struct command comm_sol = { view_sol, "start-of-line" };

static int view_eol(struct command *c, int key, struct cmd_info *ci)
{
	struct view_data *vd;
	struct pane *p = ci->focus;
	wint_t ch;
	while (p && p->refresh != view_refresh)
		p = p->parent;
	if (!p)
		return 0;
	vd = p->data;
	while ((ch = mark_next(vd->text, mark_of_point(vd->point))) != WEOF &&
	       ch != '\n')
		;
	if (ch == '\n')
		mark_prev(vd->text, mark_of_point(vd->point));
	pane_focus(p);
	return 1;
}
static struct command comm_eol = { view_eol, "end-of-line" };

void view_register(struct map *m)
{
	key_add(m, 'F'-64, &comm_next);
	key_add(m, FK(KEY_RIGHT), &comm_next);
	key_add(m, 'B'-64, &comm_prev);
	key_add(m, FK(KEY_LEFT), &comm_prev);
	key_add(m, 'A'-64, &comm_sol);
	key_add(m, 'E'-64, &comm_eol);
}
