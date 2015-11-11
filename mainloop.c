
/*
 * Copyright Neil Brown <neil@brown.name> 2015
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * main loop for edlib.
 */

#include <unistd.h>
#include <stdlib.h>
#include <locale.h>
#include <fcntl.h>
#include <event.h>
#include <wchar.h>
#include <dirent.h>
#include <string.h>

#include "core.h"

char WelcomeText[] =
	"\n"
	"Welcome to 'edlib' - the beginning of what one day might be an editor\n"
	"\n"
	"Current functionality includes:\n"
	"  splitting and closing windows (C-x 0,1,2,3)\n"
	"  Resize current window (C-x },{,^)\n"
	"  Move among windows (C-x o,O  or mouse click)\n"
	"  Opening a file or directrory (C-x C-f)\n"
	"  Open a document by name (C-x b)\n"
	"  Open a file or document in another pane (C-x 4 C-f,b)\n"
	"  Kill the current document (C-x k)\n"
	"  Movement by char, word, line, page, start/end file (standard emacs keys)\n"
	"  Insert/delete text\n"
	"  C-_ and M-C-_ to undo and redo changes\n"
	"  Visit list of documents (C-x C-b)\n"
	"  Open file from directory list, or document from document list ('f').\n"
	"  Open file in 'hex' view from directory listing ('h').\n"
	"  Numeric prefixes with M-0 to M-9.\n"
	"\n"
	"And C-x C-c to close (without saving anything)\n"
	"Mouse clicks move the cursor, and clicking on the scroll bar scrolls\n"
	;

int main(int argc, char *argv[])
{
	struct event_base *base;
	struct pane *root, *global;
	struct pane *b, *p= NULL;
	struct cmd_info ci = {0};
	struct editor *ed;
	struct pane *vroot = editor_new();

	ed = pane2ed(vroot);
	setlocale(LC_ALL, "");
	setlocale(LC_CTYPE, "enUS.UTF-8");
	base = event_base_new();
	event_base_priority_init(base, 2);
	ed->base = base;

	editor_load_module(ed, "lib-line-count");
	editor_load_module(ed, "lib-search");
	editor_load_module(ed, "display-ncurses");
	ci.home = ci.focus = vroot;
	ci.key = "display-ncurses";
	if (!key_lookup(ed->commands, &ci))
		exit(1);
	root = ci.focus;
	global = pane_attach(root, "global-keymap", NULL, NULL);

	editor_load_module(ed, "mode-emacs");
	ci.focus = global;
	ci.key = "global-set-keymap";
	ci.str = "mode-emacs";
	key_handle_focus(&ci);
	b = pane_attach(global, "tile", NULL, NULL);
	if (b)
		p = doc_from_text(b, "*Welcome*", WelcomeText);
	if (p) {
		pane_refresh(root);
		event_base_dispatch(base);
	}
	pane_close(root);
	exit(0);
}
