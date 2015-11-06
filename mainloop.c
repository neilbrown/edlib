
/*
 * main loop for edlib.
 */

#include <unistd.h>
#include <stdlib.h>
#include <locale.h>
#include <fcntl.h>
#include <event.h>
#include <curses.h>
#include <wchar.h>

#include "core.h"
#include "pane.h"
#include "tile.h"
#include "view.h"

#include "extras.h"

static void attach_file(struct pane *p, char *fname, char *render)
{
	int fd = open(fname, O_RDONLY);
	struct point *pt;
	int i;

	if (fd >= 0) {
		p = doc_open(p, fd, fname, render);
		close(fd);
	} else {
		p = doc_from_text(p, fname, "File not found");
	}
	pt = p->parent->point;
	point_reset(pt);
	if (fname[0] != '.')
		for (i=0 ; i<2000; i++)
			mark_next(pt->doc, mark_of_point(pt));
}

void text_register(struct editor *ed);
void doc_dir_register(struct editor *ed);
void render_text_register(struct editor *ed);
void render_hex_register(struct editor *ed);
void render_dir_register(struct editor *ed);
int main(int argc, char *argv[])
{
	struct event_base *base;
	struct pane *root;
	struct pane *b1, *b2, *b3, *b4;
	struct map *global_map;
	struct editor *ed = editor_new();

	setlocale(LC_ALL, "");
	setlocale(LC_CTYPE, "enUS.UTF-8");
	base = event_base_new();
	event_base_priority_init(base, 2);
	global_map = key_alloc();
	ed->base = base;
	root = ncurses_init(ed, global_map);
	tile_register(global_map);
	view_register(global_map);
	text_register(ed);
	doc_dir_register(ed);
	render_dir_register(ed);
	render_text_register(ed);
	render_hex_register(ed);
	popup_init();
	emacs_register(global_map);
	pane_set_mode(root, "emacs-", 0);

	b1 = tile_init(root);
	b2 = tile_split(b1, 0, 0);
	b3 = tile_split(b1, 1, 1);
	attach_file(b3, "core-mark.c", NULL);
	attach_file(b1, ".", NULL);
	attach_file(b2, "doc-text.c", NULL);

	b4 = tile_split(b2, 1, 0);
	attach_file(b4, "doc-text.c", "hex");

	pane_refresh(root);
	event_base_dispatch(base);
	ncurses_end();
	exit(0);
}
