/*
 * Copyright Neil Brown ©2015 <neil@brown.name>
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
	int height; /* height of line */
	int ascent; /* how far down to baseline */
	int hidden;
};
static struct pane *do_messageline_attach(struct pane *p);

static void pane_str(struct pane *p, char *s, char *attr, int x, int y)
{
	call_xy("text-display", p, -1, s, attr, x, y);
}

DEF_CMD(text_size_callback)
{
	struct call_return *cr = container_of(ci->comm, struct call_return, c);
	cr->x = ci->x;
	cr->y = ci->y;
	cr->i = ci->extra;
	return 1;
}

DEF_CMD(messageline_handle)
{
	struct mlinfo *mli = ci->home->data;

	if (strcmp(ci->key, "Clone") == 0) {
		struct pane *p = do_messageline_attach(ci->focus);
		struct pane *child = pane_child(ci->home);
		if (p && child)
			return comm_call_pane(child, "Clone", p,
					      0, NULL, NULL, 0, NULL);
		return 1;
	}

	if (strcmp(ci->key, "Display:border") == 0) {
		if (ci->numeric > 0)
			mli->hidden = 0;
		else
			mli->hidden = 1;
		pane_damaged(ci->home, DAMAGED_SIZE);
		return 0; /* Allow other panes to remove other borders */
	}

	if (strcmp(ci->key, "Message") == 0) {
		if (ci->extra == 0 || mli->message == NULL) {
			if (!mli->message)
				call3("Request:Notify:Keystroke", ci->home, 0, NULL);
			free(mli->message);
			mli->message = strdup(ci->str);
			pane_damaged(mli->line, DAMAGED_CONTENT);
		}
		return 0; /* allow other handlers */
	}
	if (strcmp(ci->key, "Abort") == 0) {
		if (!mli->message)
			call3("Request:Notify:Keystroke", ci->home, 0, NULL);
		free(mli->message);
		mli->message = strdup("ABORTED");
		pane_damaged(mli->line, DAMAGED_CONTENT);
		return 0;
	}
	if (strcmp(ci->key, "Refresh") == 0) {
		if (mli->height == 0) {
			struct call_return cr;
			cr.c = text_size_callback;
			call_comm7("text-size", ci->home, -1, NULL,
				   "M", 0, "bold", &cr.c);
			mli->height = cr.y;
			mli->ascent = cr.i;
		}
		if (ci->home == mli->line) {
			if (mli->hidden)
				pane_resize(ci->home, 0, ci->home->parent->h,
					    ci->home->parent->w, mli->height);
			else
				pane_resize(ci->home, 0, ci->home->parent->h - mli->height,
					    ci->home->parent->w, mli->height);
			pane_clear(mli->line, "bg:white");
			if (mli->message)
				pane_str(mli->line, mli->message, "bold,fg:red,bg:cyan",
					 0, 0 + mli->ascent);
		} else {
			pane_resize(ci->home, 0, 0, ci->home->parent->w,
				    ci->home->parent->h
				    - (mli->hidden ? 0 : mli->height));
		}
		return 1;
	}
	/* Keystroke notification clears the message line */
	if ((strcmp(ci->key, "Notify:Keystroke") == 0
	     || strcmp(ci->key, "Keystroke") == 0)
	    && mli->message) {
		free(mli->message);
		mli->message = NULL;
		pane_drop_notifiers(ci->home, "Notify:Keystroke");
		return 1;
	}
	return 0;
}

static struct pane *do_messageline_attach(struct pane *p)
{
	struct mlinfo *mli = malloc(sizeof(*mli));
	struct pane *ret;

	mli->message = NULL;
	mli->height = 0;
	mli->hidden = 0;
	ret = pane_register(p, 0, &messageline_handle, mli, NULL);
	mli->line = pane_register(p, 1, &messageline_handle, mli, NULL);
	pane_focus(p);

	return ret;
}

DEF_CMD(messageline_attach)
{
	struct pane *ret;

	ret = do_messageline_attach(ci->focus);
	return comm_call(ci->comm2, "callback:attach", ret, 0, NULL, NULL, 0);
}


void edlib_init(struct pane *ed)
{
	call_comm("global-set-command", ed, 0, NULL, "attach-messageline",
		  0, &messageline_attach);
}
