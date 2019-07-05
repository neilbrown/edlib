
/*
 * Copyright Neil Brown ©2015-2019 <neil@brown.name>
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

static char shortopt[] = "gt";

static struct pane *make_stack(struct pane *p, struct pane *doc)
{
	if (p)
		p = call_pane("attach-messageline", p);
	if (p)
		p = call_pane("attach-global-keymap", p);
	if (p)
		call("attach-mode-emacs", p);
	if (p)
		p = call_pane("attach-tile", p);
	if (p && doc)
		p = doc_attach_view(p, doc, NULL, NULL, 1);
	return p;
}

int main(int argc, char *argv[])
{
	struct pane *ed = editor_new();
	struct pane *p, *doc = NULL;
	int gtk = 0, term = 0;
	int opt;

	if (!ed)
		exit(1);

	while ((opt = getopt(argc, argv, shortopt)) != EOF) {
		switch (opt) {
		case 'g': gtk = 1;
			break;
		case 't': term = 1;
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

	doc_new(ed, "docs", NULL);
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

	call("global-load-module", ed, 0, NULL, "render-c-mode");
	call("global-load-module", ed, 0, NULL, "lib-make");
	call("global-load-module", ed, 0, NULL, "lib-server");

	while (optind < argc) {
		char *file = argv[optind++];
		int fd = open(file, O_RDONLY);
		if (fd < 0) {
			perror("edlib: open");
			fprintf(stderr, "edlib: Cannot open: %s\n", file);
			exit(2);
		}
		doc = call_pane("doc:open", ed, fd, NULL, file);
		close(fd);
	}

	p = call_pane("attach-input", ed);
	if (p && !doc)
		doc = call_pane("doc:from-text", p, 0, NULL,
				"*Welcome*", 0, NULL, WelcomeText);

	if (p) {
		if (term)
			make_stack(call_pane("attach-display-ncurses", p), doc);
		if (gtk)
			make_stack(call_pane("attach-display-pygtk", p), doc);

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
