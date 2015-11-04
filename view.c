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
#include "attr.h"

#include "extras.h"

struct view_data {
	int		border;
	struct command	ch_notify;
	int		ch_notify_num;
	struct pane	*pane;
	int		scroll_bar_y;
};

static struct map *view_map;
struct pane *view_attach(struct pane *par, struct doc *d, struct point *pt, int border);

static int do_view_refresh(struct command *cm, struct cmd_info *ci)
{
	struct pane *p = ci->home;
	int damage = ci->extra;
	int i;
	int mid;
	struct view_data *vd = p->data;
	struct point *pt;
	int ln, l, w, c;
	char msg[60];

	if (vd->pane != p)
		vd->pane = p; /* FIXME having to do this is horrible */

	if (strcmp(ci->key, "Close") == 0) {
		pt = *ci->pointp;
		doc_del_view(pt->doc, &vd->ch_notify);
		free(vd);
		return 1;
	}
	if (strcmp(ci->key, "Clone") == 0) {
		struct pane *parent = ci->focus;
		struct pane *p2;
		if (!p->point)
			return 0;
		p2 = view_attach(parent, NULL, p->point, 1);
		return pane_clone(p->focus->focus, p2);
	}
	if (strcmp(ci->key, "Refresh") != 0)
		return 0;
	pt = *ci->pointp;
	if (p->focus == NULL && !list_empty(&p->children))
		p->focus = list_first_entry(&p->children, struct pane, siblings);

	if (damage & DAMAGED_SIZE) {
		pane_check_size(p);
		if (vd->border)
			pane_resize(p->focus, 1, 0, p->w-1, p->h-1);
		else
			pane_check_size(p->focus);
	}
	if (!vd->border)
		return 0;

	count_calculate(pt->doc, NULL, mark_of_point(pt));
	count_calculate(pt->doc, NULL, NULL);
	ln = attr_find_int(*mark_attr(mark_of_point(pt)), "lines");
	l = attr_find_int(pt->doc->attrs, "lines");
	w = attr_find_int(pt->doc->attrs, "words");
	c = attr_find_int(pt->doc->attrs, "chars");
	if (l <= 0)
		l = 1;
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
	vd->scroll_bar_y = mid;
	return 0;
}
DEF_CMD(view_refresh, do_view_refresh, "view-refresh");

static int do_view_null(struct command *c, struct cmd_info *ci)
{
	return 0;
}
DEF_CMD(view_null, do_view_null, "view-no-refresh");

static int view_notify(struct command *c, struct cmd_info *ci)
{
	struct view_data *vd;
	if (strcmp(ci->key, "Replace") != 0)
		return 0;

	vd = container_of(c, struct view_data, ch_notify);
	pane_damaged(vd->pane, DAMAGED_CONTENT);
	return 0;
}

struct pane *view_attach(struct pane *par, struct doc *d, struct point *pt, int border)
{
	struct view_data *vd;
	struct pane *p;

	if (!d)
		d = pt->doc;
	vd = malloc(sizeof(*vd));
	vd->border = border;
	vd->ch_notify.func = view_notify;
	vd->ch_notify.name = "view-notify";
	vd->ch_notify_num = doc_add_view(d, &vd->ch_notify);

	p = pane_register(par, 0, &view_refresh, vd, NULL);
	p->keymap = view_map;
	if (pt)
		point_dup(pt, &p->point);
	else
		point_new(d, &p->point);
	vd->pane = p;

	pane_resize(p, 0, 0, par->w, par->h);
	p = pane_register(p, 0, &view_null, vd, NULL);
	if (vd->border)
		pane_resize(p, 1, 0, par->w-1, par->h-1);
	else
		pane_resize(p, 0, 0, par->w, par->h);
	pane_damaged(p, DAMAGED_SIZE);
	return p;
}

static int view_char(struct command *c, struct cmd_info *ci)
{
	struct point *pt = *ci->pointp;
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
	struct point *pt = *ci->pointp;
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
	struct point *pt = *ci->pointp;
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
	struct point *pt = *ci->pointp;
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
	struct point *pt = *ci->pointp;
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
	struct point *pt = *ci->pointp;
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
	struct point *pt = *ci->pointp;
	wint_t ch = 1;
	int rpt = RPT_NUM(ci);

	rpt *= ci->home->h-2;
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

static int view_replace(struct command *c, struct cmd_info *ci)
{
	struct point *pt = *ci->pointp;
	bool first_change = (ci->extra == 0);

	doc_replace(pt, ci->mark, ci->str, &first_change);
	return 1;
}
DEF_CMD(comm_replace, view_replace, "do-replace");

static int view_click(struct command *c, struct cmd_info *ci)
{
	struct pane *p = ci->home;
	struct view_data *vd = p->data;
	int mid = vd->scroll_bar_y;
	struct cmd_info ci2 = {0};

	if (ci->x != 0)
		return 0;

	ci2.focus = ci2.home = p->focus;
	ci2.key = "Move-View-Small";
	ci2.numeric = RPT_NUM(ci);
	ci2.mark = mark_of_point(*ci->pointp);
	ci2.pointp = ci->pointp;
	p = p->focus;
	if (ci->y == mid-1) {
		/* scroll up */
		ci2.numeric = -ci2.numeric;
	} else if (ci->y < mid-1) {
		/* big scroll up */
		ci2.numeric = -ci2.numeric;
		ci2.key = "Move-View-Large";
	} else if (ci->y == mid+1) {
		/* scroll down */
	} else if (ci->y > mid+1 && ci->y < p->h-1) {
		ci2.key = "Move-View-Large";
	} else
		return 0;
	return key_handle_focus(&ci2);
}
DEF_CMD(comm_click, view_click, "view-click");

void view_register(struct map *m)
{
	view_map = key_alloc();

	key_add(m, "Move-Char", &comm_char);
	key_add(m, "Move-Word", &comm_word);
	key_add(m, "Move-WORD", &comm_WORD);
	key_add(m, "Move-EOL", &comm_eol);
	key_add(m, "Move-Line", &comm_line);
	key_add(m, "Move-File", &comm_file);
	key_add(m, "Move-View-Large", &comm_page);

	key_add(view_map, "Replace", &comm_replace);

	key_add(view_map, "Click-1", &comm_click);
	key_add(view_map, "Press-1", &comm_click);

}
