
/*
 * Copyright Neil Brown <neil@brown.name> 2015
 * May be distrubuted under terms of GPLv2 - see file:COPYING
 *
 * main loop for edlib.
 */

#include <unistd.h>
#include <stdlib.h>
#include <locale.h>
#include <fcntl.h>
#include <event.h>
#include <curses.h>
#include <wchar.h>
#include <dirent.h>
#include <string.h>
#include <dlfcn.h>

#include "core.h"
#include "extras.h"

char WelcomeText[] =
	"\n"
	"Welcome to 'edlib' - the beginning of what one day might be an editor\n"
	"\n"
	"Current functionality includes:\n"
	"  splitting and closing windows (C-x 0,2,3)\n"
	"  Resize current window (C-x },{,^\n"
	"  Move among windows (C-x o,O  or mouse click)\n"
	"  Opening a file or directrory (C-x C-f)\n"
	"  Movement by char, word, line, page, start/end file (standard emacs keys)\n"
	"  Insert/delete text\n"
	"  C-_ and M-C-_ to undo and redo changes\n"
	"  Visit list of documents (C-x C-b)\n"
	"  Open file from directory list, or document from document list ('f').\n"
	"  Numeric prefixes with M-0 to M-9.\n"
	"\n"
	"And C-x C-c to close (without saving anything)\n"
	"Mouse clicks move the cursor, and clicking on the scroll bar scrolls\n"
	;
static void load_libs(struct editor *ed)
{
	DIR *dir;
	struct dirent de, *res;
	char buf[PATH_MAX];

	dir = opendir("lib");
	if (!dir)
		return;
	while (readdir_r(dir, &de, &res) == 0 && res) {
		void *h;
		void (*s)(struct editor *e);
		int l = strlen(res->d_name);
		if (l <= 3 || strcmp(res->d_name + l-3, ".so") != 0)
			continue;
		strcpy(buf, "lib/");
		strcat(buf, res->d_name);
		h = dlopen(buf, RTLD_NOW);
		if (h == NULL) continue;
		s = dlsym(h, "edlib_init");
		if (s)
			s(ed);
	}
}

int main(int argc, char *argv[])
{
	struct event_base *base;
	struct pane *root;
	struct pane *b, *p;

	struct editor *ed = editor_new();

	setlocale(LC_ALL, "");
	setlocale(LC_CTYPE, "enUS.UTF-8");
	base = event_base_new();
	event_base_priority_init(base, 2);
	ed->base = base;
	root = ncurses_init(ed);
	tile_register();
	view_register();
	popup_init();
	load_libs(ed);
	ed->map = emacs_register();

	b = tile_init(root);
	p = doc_from_text(b, "*Welcome*", WelcomeText);
	if (p) {
		pane_refresh(root);
		event_base_dispatch(base);
	}
	ncurses_end();
	exit(0);
}
