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

static const char shortopt[] = "gtx";

int main(int argc, char *argv[])
{
	struct pane *ed;
	struct pane *first_window = NULL;
	struct pane *p, *doc = NULL;
	bool gtk = False, term = False, x11 = False;
	int opt;
	char *base = NULL;

	if (argv[0]) {
		base = strrchr(argv[0], '/');
		if (base)
			base += 1;
		else
			base = argv[0];
	}
	ed = editor_new(base);

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
	call("config-load", ed, 0, NULL, "{COMM}.ini");

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

	if (!doc) {
		char *welcome_file = call_ret(str, "xdg-find-edlib-file", ed,
					      0, NULL, "Welcome-{COMM}.txt",
					      0, NULL, "data");
		char *WelcomeText = NULL;
		if (welcome_file) {
			int fd = open(welcome_file, O_RDONLY);
			if (fd >= 0) {
				int len = lseek(fd, 0, SEEK_END);
				lseek(fd, 0, SEEK_SET);
				if (len > 0 && len < 10000) {
					WelcomeText = malloc(len+1);
					read(fd, WelcomeText, len);
					WelcomeText[len] = 0;
				}
				close(fd);
			}
			free(welcome_file);
		}

		if (!WelcomeText)
			WelcomeText = strdup("Welcome.\n");

		doc = call_ret(pane, "doc:from-text", ed, 0, NULL,
			       "*Welcome*", 0, NULL, WelcomeText);
		free(WelcomeText);
	}
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
