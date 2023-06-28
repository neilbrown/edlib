/*
 * Copyright Neil Brown ©2015-2023 <neil@brown.name>
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
#include <stdio.h>

#include "core.h"
#include "misc.h"

static const char WelcomeText[] =
	"\n"
	"Welcome to 'edlib' - a document editor that comes in well defined pieces.\n"
	"\n"
	"Current functionality includes:\n"
	"  splitting and closing windows (C-x 0,1,2,3)\n"
	"  Resize current window (C-x },{,^)\n"
	"  Move among windows (C-x o,O  or mouse click)\n"
	"  Opening a file or directory (C-x C-f)\n"
	"    TAB performs file-name completion in a menu\n"
	"  Save files - current one (C-x C-s) or all (C-x s)\n"
	"  Open a document by name (C-x b) - with TAB completion\n"
	"  Open a file or document in another pane (C-x 4 C-f,b)\n"
	"  Kill the current document (C-x k)\n"
	"  Movement by char, word, line, page, start/end file (standard emacs keys)\n"
	"  Insert/delete text\n"
	"  C-_ to undo and redo changes\n"
	"  C-s to search (incrementally) in text document\n"
	"  Visit list of documents (C-x C-b)\n"
	"  Open file from directory list, or document from document list ('f').\n"
	"  Open file in 'hex' view from directory listing ('h').\n"
	"  Numeric prefixes with M-0 to M-9.\n"
	"  Run make (C-c C-m) or grep (M-x grep Return)\n"
	"\n"
	"And C-x C-c to close - type 's' to save or '%' to not save in the pop-up\n"
	"Mouse clicks move the cursor, and clicking on the scroll bar scrolls\n"
	;

static const char shortopt[] = "gtx";

int main(int argc, char *argv[])
{
	struct pane *ed = editor_new();
	struct pane *first_window = NULL;
	struct pane *p, *doc = NULL;
	bool gtk = False, term = False, x11 = False;
	int opt;

	if (!ed)
		exit(1);

	while ((opt = getopt(argc, argv, shortopt)) != EOF) {
		switch (opt) {
		case 'g': gtk = True;
			break;
		case 't': term = True;
			break;
		case 'x': x11 = True;
			break;
		default:
			fprintf(stderr, "Usage: edlib [-g] [-t] [file ...]\n");
			exit(2);
		}
	}
	if (!gtk && !term && !x11)
		term = 1;

	setlocale(LC_ALL, "");
	setlocale(LC_CTYPE, "enUS.UTF-8");

	call("global-load-module", ed, 0, NULL, "lib-config");
	call("config-load", ed, 0, NULL, "edlib.ini", 0, NULL, argv[0]);

	call("attach-doc-docs", ed);

	while (optind < argc) {
		char *file = argv[optind++];
		int fd = open(file, O_RDONLY);
		if (fd < 0) {
			/* '4' says 'allow create' */
			doc = call_ret(pane, "doc:open", ed, -1, NULL, file, 4);
		} else {
			doc = call_ret(pane, "doc:open", ed, fd, NULL, file);
			close(fd);
		}
	}

	if (!doc)
		doc = call_ret(pane, "doc:from-text", ed, 0, NULL,
			       "*Welcome*", 0, NULL, WelcomeText);
	if (!doc) {
		fprintf(stderr, "edlib: cannot create default document.\n");
		exit(1);
	}

	if (term) {
		char *TERM = getenv("TERM");

		p = call_ret(pane, "attach-display-ncurses", doc,
			     0, NULL, "-", 0, NULL, TERM);
		if (p) {
			char *e;
			e = getenv("SSH_CONNECTION");
			if (e && *e)
				call("window:set:REMOTE_SESSION", p,
				     0, NULL, "yes");

			call("window:set:DISPLAY", p,
			     0, NULL, getenv("DISPLAY"));
			call("window:set:XAUTHORITY", p,
			     0, NULL, getenv("XAUTHORITY"));
			if (!first_window)
				first_window = p;
			call("Display:set-noclose", p, 1, NULL,
			     "Cannot close primary display");
		}
	}

	if (gtk) {
		p = call_ret(pane, "attach-display-gtk",
			     doc, 0, NULL, getenv("DISPLAY"));
		if (!first_window)
			first_window = p;
	}

	if (x11) {
		p = call_ret(pane, "attach-display-x11",
			     doc, 0, NULL, getenv("DISPLAY"),
			     0, NULL, getenv("XAUTHORITY"));
		if (!first_window)
			first_window = p;
	}

	if (first_window) {
		call("global-multicall-startup-", first_window);
		while (call("event:run", ed) == 1)
			;
	} else
		fprintf(stderr, "edlib: cannot create a display\n");
	pane_close(ed);
	exit(0);
}
