/*
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
#include <unistd.h>
#include <stdlib.h>
#include <memory.h>
#include <ncurses.h>

#include "list.h"
#include "pane.h"
#include "keymap.h"
#include "text.h"
#include "view.h"
#include "render_text.h"

#include "extra.h"

struct popup_info {
	char		*name;
	struct pane	*target;
	wint_t		key;
	struct text	*text;
};

static int popup_refresh(struct pane  *p, int damage);
#define	CMD(func, name) {func, name, popup_refresh}
#define	DEF_CMD(comm, func, name) static struct command comm = CMD(func, name)
static struct map *pp_map;

static int popup_refresh(struct pane *p, int damage)
{
	struct popup_info *ppi = p->data;
	int i;
	int label;

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
static int popup_no_refresh(struct pane *p, int damage)
{
	return 0;
}

struct pane *popup_register(struct pane *p, char *name, char *content, wint_t key)
{
	/* attach to root, center, one line of content, half width of pane */
	struct pane *ret, *root, *p2;
	struct popup_info *ppi = malloc(sizeof(*ppi));
	struct text *t;
	struct text_ref r;
	int first = 1;
	struct cmd_info ci;

	root = p;
	while (root->parent)
		root = root->parent;
	ppi->name = name;
	ppi->target = p;
	ppi->key = key;
	p = pane_register(root, 1, popup_refresh, ppi, NULL);

	pane_resize(p, root->w/4, root->h/2-2, root->w/2, 3);
	p = pane_register(p, 0, popup_no_refresh, NULL, NULL);
	pane_resize(p, 1, 1, p->parent->w-2, 1);
	t = text_new();
	ppi->text = t;
	r = text_find_ref(t, 0);
	text_add_str(t, &r, content, NULL, &first);
	p2 = view_attach(p, t, 0);
	render_text_attach(p2);
	ret = pane_register(p2, 0, popup_no_refresh, NULL, NULL);
	pane_resize(ret, 0, 0, p2->w, p2->h);
	ret->cx = ret->cy = -1;
	ret->keymap = pp_map;
	pane_focus(ret);
	ci.key = MV_FILE;
	ci.repeat =1;
	ci.focus = ret;
	ci.mark = NULL;
	key_handle_focus(&ci);
	return ret;
}

static int popup_done(struct command *c, struct cmd_info *ci)
{
	struct pane *p = ci->focus;
	struct popup_info *ppi = p->data;
	struct cmd_info ci2;
	ci2.focus = ppi->target;
	ci2.key = ppi->key;
	ci2.repeat = 1;
	ci2.str = text_getstr(ppi->text);
	ci2.mark = NULL;
	key_handle_focus(&ci2);
	free(ci2.str);
	/* tear down the popup */
	return 1;
}
DEF_CMD(comm_done, popup_done, "popup-done");

void popup_init(void)
{
	pp_map = key_alloc();

	key_add(pp_map, '\r', &comm_done);
}
