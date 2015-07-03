
/*
 * main loop for edlib.
 */

#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <event.h>
#include <curses.h>
#include <wchar.h>

#include "list.h"
#include "pane.h"
#include "tile.h"
#include "text.h"
#include "mark.h"
#include "view.h"
#include "keymap.h"
#include "render_text.h"
#include "popup.h"

static void attach_file(struct pane *p, char *fname)
{
	int fd = open(fname, O_RDONLY);
	struct text *t = text_new();
	struct text_ref r;

	r = text_find_ref(t, 0);
	if (fd >= 0)
		text_load_file(t, fd);
	else {
		int first=1;
		text_add_str(t, &r, "File not found: ", NULL, &first);
		text_add_str(t, &r, fname, NULL, &first);
		text_add_str(t, &r, "\n", NULL, &first);
	}
	p = view_attach(p, t, 1);
	{
		struct view_data *vd = p->data;
		int i;
		for (i=0 ; i<2000; i++)
			mark_next(vd->text, mark_of_point(vd->point));
	}
	render_text_attach(p);
}

int main(int argc, char *argv[])
{
	struct event_base *base;
	struct pane *root;
	struct pane *b1, *b2, *b3;
	struct map *global_map;

	base = event_base_new();
	event_base_priority_init(base, 2);
	global_map = key_alloc();
	root = ncurses_init(base, global_map);
	tile_register(global_map);
	view_register(global_map);
	render_text_register(global_map);
	popup_init();

	b1 = tile_init(root);
	b2 = tile_split(b1, 0, 0);
	b3 = tile_split(b1, 1, 1);
	attach_file(b3, "mark.c");
	attach_file(b1, "mainloop.c");
	attach_file(b2, "text.c");

	pane_refresh(root);
	event_base_dispatch(base);
	ncurses_end();
	exit(0);
}
