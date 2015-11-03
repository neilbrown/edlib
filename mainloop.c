
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

static struct doc *attach_file(struct pane *p, char *fname)
{
	int fd = open(fname, O_RDONLY);
	struct doc *d = doc_new(pane2ed(p), "text");
	struct point *pt;
	int i;

	p = view_attach(p, d, 1);
	pt = p->parent->point;
	if (fd >= 0)
		doc_load_file(pt, fd);
	else {
		bool first=1;
		doc_replace(pt, NULL, "File not found: ", &first);
		doc_replace(pt, NULL, fname, &first);
		doc_replace(pt, NULL, "\n", &first);
	}

	point_reset(pt);
	for (i=0 ; i<2000; i++)
		mark_next(pt->doc, mark_of_point(pt));

	render_text_attach(p, pt);
	return d;
}

static struct doc *attach_dir(struct pane *p, char *fname)
{
	int fd = open(fname, O_RDONLY|O_DIRECTORY);
	struct doc *d = doc_new(pane2ed(p), "dir");
	struct point *pt;

	p = view_attach(p, d, 1);
	pt = p->parent->point;
	if (fd >= 0)
		doc_load_file(pt, fd);
	else {
		bool first=1;
		doc_replace(pt, NULL, "Dir not found: ", &first);
		doc_replace(pt, NULL, fname, &first);
		doc_replace(pt, NULL, "\n", &first);
	}

	point_reset(pt);

	render_dir_attach(p, pt);
	return d;
}

void text_register(struct editor *ed);
void doc_dir_register(struct editor *ed);
int main(int argc, char *argv[])
{
	struct event_base *base;
	struct pane *root;
	struct pane *b1, *b2, *b3, *b4, *v;
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
	popup_init();
	emacs_register(global_map);
	pane_set_mode(root, "emacs-", 0);

	b1 = tile_init(root);
	b2 = tile_split(b1, 0, 0);
	b3 = tile_split(b1, 1, 1);
	attach_file(b3, "core-mark.c");
	attach_dir(b1, ".");

	struct doc *d = attach_file(b2, "doc-text.c");

	b4 = tile_split(b2, 1, 0);
	v = view_attach(b4, d, 1);
	render_hex_attach(v, v->parent->point);

	pane_refresh(root);
	event_base_dispatch(base);
	ncurses_end();
	exit(0);
}
