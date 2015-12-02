/*
 * Copyright Neil Brown <neil@brown.name> 2015
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * trim a line off the bottom of a pane and capture messages
 * to go there.  They disappear on the next keystroke.
 *
 * Later it might be good to allow boarderless popups to appear here.
 */

#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include "core.h"

struct mlinfo {
	char *message;
	struct pane *line;
};

static void pane_str(struct pane *p, char *s, char *attr, int x, int y)
{
	mbstate_t ps = {0};
	int err;
	wchar_t ch;

	while ((err = mbrtowc(&ch, s, 5, &ps)) > 0) {
		pane_text(p, ch, attr, x, y);
		s += err;
		x += 1;
	}
}

DEF_CMD(messageline_handle)
{
	struct mlinfo *mli = ci->home->data;

	if (strcmp(ci->key, "Message") == 0) {
		if (ci->extra == 0 || mli->message == NULL) {
			free(mli->message);
			mli->message = strdup(ci->str);
			pane_damaged(mli->line, DAMAGED_CONTENT);
		}
		return 0; /* allow other handlers */
	}
	if (strcmp(ci->key, "Abort") == 0) {
		free(mli->message);
		mli->message = strdup("ABORTED");
		pane_damaged(mli->line, DAMAGED_CONTENT);
		return 0;
	}
	if (strcmp(ci->key, "Refresh") == 0) {
		if (ci->home == mli->line) {
			pane_resize(ci->home, 0, ci->home->parent->h - 1,
				    ci->home->parent->w, 1);
			pane_clear(mli->line, "");
			if (mli->message)
				pane_str(mli->line, mli->message, "bold", 0, 0);
		} else {
			pane_resize(ci->home, 0, 0, ci->home->parent->w,
				    ci->home->parent->h - 1);
		}
		return 1;
	}
	/* Anything else clears the message */
	if (strncmp(ci->key, "pane", 4) != 0 && mli->message) {
		free(mli->message);
		mli->message = NULL;
		pane_clear(mli->line, "");
	}
	return 0;
}

DEF_CMD(messageline_attach)
{
	struct mlinfo *mli = malloc(sizeof(*mli));
	struct pane *p = ci->focus;

	mli->message = NULL;
	ci->focus = pane_register(p, 0, &messageline_handle, mli, NULL);
	mli->line = pane_register(p, 1, &messageline_handle, mli, NULL);
	pane_focus(ci->focus);
	return 1;
}


void edlib_init(struct editor *ed)
{
	key_add(ed->commands, "attach-messageline", &messageline_attach);
}
