
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
	"  Opening a file or directory (C-x C-f)\n"
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

int main(int argc, char *argv[])
{
	struct pane *ed = editor_new();
	struct pane *p;
	int gtk = 0;

	if (!ed)
		exit(1);
	if (argc > 1 && strcmp(argv[1], "-g") == 0)
		gtk = 1;

	setlocale(LC_ALL, "");
	setlocale(LC_CTYPE, "enUS.UTF-8");

	doc_new(ed, "docs");
	call5("global-load-module", ed, 0, NULL, "lib-linecount", 0);
	call5("global-load-module", ed, 0, NULL, "lib-search", 0);
	call5("global-load-module", ed, 0, NULL, "lib-popup", 0);
	call5("global-load-module", ed, 0, NULL, "lang-python", 0);
	call5("global-load-module", ed, 0, NULL, "doc-text", 0);
	call5("global-load-module", ed, 0, NULL, "doc-dir", 0);
	call5("global-load-module", ed, 0, NULL, "render-hex", 0);
	call5("global-load-module", ed, 0, NULL, "render-present", 0);

	p = call_pane("attach-input", ed, 0, NULL, 0);
	if (p) {
		if (gtk)
			p = call_pane("attach-display-pygtk", p, 0, NULL, 0);
		else
			p = call_pane("attach-display-ncurses", p, 0, NULL, 0);
	}

	if (p)
		p = call_pane("attach-messageline", p, 0, NULL, 0);
	if (p)
		p = call_pane("attach-global-keymap", p, 0, NULL, 0);

	if (p)
		call3("attach-mode-emacs", p, 0, NULL);

	if (p)
		p = call_pane("attach-tile", p, 0, NULL, 0);
	if (p)
		p = doc_from_text(p, "*Welcome*", WelcomeText);
	if (p) {
		pane_refresh(ed);
		while (call3("event:run", ed, 0, NULL) == 1)
			;
	}
	pane_close(ed);
	exit(0);
}
