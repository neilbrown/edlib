
/*
 * Copyright Neil Brown ©2015-2020 <neil@brown.name>
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

static char WelcomeText[] =
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

static char shortopt[] = "gt";

static struct pane *make_stack(struct pane *p, struct pane *doc)
{
	if (p)
		p = call_ret(pane, "attach-messageline", p);
	if (p)
		p = call_ret(pane, "attach-global-keymap", p);
	if (p)
		call("attach-mode-emacs", p);
	if (p)
		p = call_ret(pane, "attach-tile", p);
	if (p && doc)
		p = home_call_ret(pane, doc, "doc:attach-view", p, 1);
	return p;
}

int main(int argc, char *argv[])
{
	struct pane *ed = editor_new();
	struct pane *first_window = NULL;
	struct pane *p, *doc = NULL;
	bool gtk = False, term = False;
	int opt;

	if (!ed)
		exit(1);

	while ((opt = getopt(argc, argv, shortopt)) != EOF) {
		switch (opt) {
		case 'g': gtk = True;
			break;
		case 't': term = True;
			break;
		default:
			fprintf(stderr, "Usage: edlib [-g] [-t] [file ...]\n");
			exit(2);
		}
	}
	if (!gtk && !term)
		term = 1;

	setlocale(LC_ALL, "");
	setlocale(LC_CTYPE, "enUS.UTF-8");


	call("attach-doc-docs", ed);
	call("global-load-module", ed, 0, NULL, "lib-linecount");
	call("global-load-module", ed, 0, NULL, "lib-search");
	call("global-load-module", ed, 0, NULL, "lib-popup");
	call("global-load-module", ed, 0, NULL, "lang-python");
	call("global-load-module", ed, 0, NULL, "doc-text");
	call("global-load-module", ed, 0, NULL, "doc-dir");
	call("global-load-module", ed, 0, NULL, "render-hex");
	call("global-load-module", ed, 0, NULL, "render-present");
	call("global-load-module", ed, 0, NULL, "render-lines");
	call("global-load-module", ed, 0, NULL, "module-notmuch");
	call("global-load-module", ed, 0, NULL, "doc-email");
	call("global-load-module", ed, 0, NULL, "lib-viewer");
	call("global-load-module", ed, 0, NULL, "lib-qprint");
	call("global-load-module", ed, 0, NULL, "lib-copybuf");
	call("global-load-module", ed, 0, NULL, "lib-colourmap");
	call("global-load-module", ed, 0, NULL, "lib-textfill");
	call("global-load-module", ed, 0, NULL, "lib-autosave");
	call("global-load-module", ed, 0, NULL, "render-format");

	call("global-load-module", ed, 0, NULL, "render-c-mode");
	call("global-load-module", ed, 0, NULL, "lib-make");
	call("global-load-module", ed, 0, NULL, "lib-server");
	call("global-load-module", ed, 0, NULL, "lib-utf8");
	call("global-load-module", ed, 0, NULL, "lib-charset");

	call("global-load-module", ed, 0, NULL, "config");

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

	if (term) {
		struct pane *disp = NULL;
		p = call_ret(pane, "attach-input", ed);
		if (p) {
			attr_set_str(&p->attrs, "TERM", getenv("TERM"));
			attr_set_str(&p->attrs, "DISPLAY", getenv("DISPLAY"));
			disp = call_ret(pane, "attach-display-ncurses",
					p);
		}
		if (disp) {
			p = make_stack(disp, doc);
			if (p && !first_window)
				first_window = p;
			call("Display:set-noclose", disp, 1, NULL,
			     "Cannot close primary display");
		}
	}
	if (gtk) {
		struct pane *disp = NULL;
		p = call_ret(pane, "attach-input", ed);
		if (p) {
			attr_set_str(&p->attrs, "DISPLAY", getenv("DISPLAY"));
			disp = call_ret(pane, "attach-display-pygtk",
					p);
		}
		if (disp) {
			p = make_stack(disp, doc);
			if (p && !first_window)
				first_window = p;
		}
	}

	if (first_window) {
		call("global-multicall-startup-", first_window);
		time_start(TIME_REFRESH);
		pane_refresh(ed);
		time_stop(TIME_REFRESH);
		while (call("event:run", ed) == 1) {
			time_start(TIME_IDLE);
			call("global-multicall-on_idle-", ed);
			time_stop(TIME_IDLE);
			time_start(TIME_REFRESH);
			pane_refresh(ed);
			time_stop(TIME_REFRESH);
		}
	}
	pane_close(ed);
	exit(0);
}
