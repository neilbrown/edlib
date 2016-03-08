
/*
 * Copyright Neil Brown ©2015 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * main loop for edlib.
 */

#include <unistd.h>
#include <stdlib.h>
#include <locale.h>
#include <fcntl.h>
#include <wchar.h>
#include <dirent.h>
#include <string.h>

#include "core.h"

static char WelcomeText[] =
	"\n"
	"Welcome to 'edlib' - the beginning of what one day might be an editor\n"
	"\n"
	"Current functionality includes:\n"
	"  splitting and closing windows (C-x 0,1,2,3)\n"
	"  Resize current window (C-x },{,^)\n"
	"  Move among windows (C-x o,O  or mouse click)\n"
	"  Opening a file or directrory (C-x C-f)\n"
	"    TAB performs file-name completion in a menu\n"
	"  Open a document by name (C-x b) - with TAB completion\n"
	"  Open a file or document in another pane (C-x 4 C-f,b)\n"
	"  Kill the current document (C-x k)\n"
	"  Movement by char, word, line, page, start/end file (standard emacs keys)\n"
	"  Insert/delete text\n"
	"  C-_ and M-C-_ to undo and redo changes\n"
	"  C-s to search (incrementally) in text document\n"
	"  Visit list of documents (C-x C-b)\n"
	"  Open file from directory list, or document from document list ('f').\n"
	"  Open file in 'hex' view from directory listing ('h').\n"
	"  Numeric prefixes with M-0 to M-9.\n"
	"\n"
	"And C-x C-c to close (without saving anything)\n"
	"Mouse clicks move the cursor, and clicking on the scroll bar scrolls\n"
	;

DEF_CMD(take_pane)
{
	struct call_return *cr = container_of(ci->comm, struct call_return, c);
	cr->p = ci->focus;
	return 1;
}

int main(int argc, char *argv[])
{
	struct pane *root, *global;
	struct pane *b, *p= NULL;
	struct cmd_info ci = {0};
	struct call_return cr;
	struct pane *ed;
	struct pane *vroot = editor_new();
	int gtk = 0;

	if (argc > 1 && strcmp(argv[1], "-g") == 0)
		gtk = 1;

	ed = vroot;
	setlocale(LC_ALL, "");
	setlocale(LC_CTYPE, "enUS.UTF-8");

	doc_new(ed, "docs");
	call5("global-load-module", ed, 0, NULL, "lib-line-count", 0);
	call5("global-load-module", ed, 0, NULL, "lib-search", 0);
	call5("global-load-module", ed, 0, NULL, "lang-python", 0);

	if (gtk) {
		call5("python-load", vroot, 0, NULL, "python/display-pygtk.py", 0);
		call3("pygtkevent:activate", vroot, 0, NULL);
		vroot = pane_attach(vroot, "input", NULL, NULL);
		ci.key = "display-pygtk";
	} else {
		call5("global-load-module", ed, 0, NULL, "lib-libevent", 0);
		call5("global-load-module", ed, 0, NULL, "display-ncurses", 0);
		call3("libevent:activate", vroot, 0, NULL);
		ci.key = "display-ncurses";
	}
	ci.home = ci.focus = vroot;
	cr.c = take_pane;
	cr.p = NULL;
	ci.comm2 = &cr.c;
	if (key_handle(&ci) <= 0)
		exit(1);
	root = cr.p;
	global = pane_attach(root, "messageline", NULL, NULL);
	global = pane_attach(global, "global-keymap", NULL, NULL);

	call5("global-load-module", ed, 0, NULL, "mode-emacs", 0);
	call5("global-set-keymap", global, 0, NULL, "mode-emacs", 0);

	b = pane_attach(global, "tile", NULL, NULL);
	if (b)
		p = doc_from_text(b, "*Welcome*", WelcomeText);
	if (p) {
		memset(&ci, 0, sizeof(ci));
		ci.home = ci.focus = p;
		ci.key = "python-load";
		ci.str = "python/test.py";
		key_handle(&ci);

		pane_refresh(ed);
		while (call3("event:run", vroot, 0, NULL) == 1)
			;
	}
	pane_close(ed);
	exit(0);
}
