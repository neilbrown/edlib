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
	struct pane *line safe, *child;
	int height; /* height of line */
	int ascent; /* how far down to baseline */
	int hidden;
};
static struct pane *do_messageline_attach(struct pane *p);

static void pane_str(struct pane *p safe, char *s, char *attr, int x, int y)
{
	call("Draw:text", p, -1, NULL, s, 0, NULL, attr, NULL, x, y);
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

		pane_clone_children(ci->home, p);
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

	if (strcmp(ci->key, "Message") == 0 && ci->str) {
		if (ci->extra == 0 || mli->message == NULL) {
			if (!mli->message)
				call("Request:Notify:Keystroke", ci->home);
			free(mli->message);
			mli->message = strdup(ci->str);
			pane_damaged(mli->line, DAMAGED_CONTENT);
		}
		return 0; /* allow other handlers */
	}
	if (strcmp(ci->key, "Abort") == 0) {
		if (!mli->message)
			call("Request:Notify:Keystroke", ci->home);
		free(mli->message);
		mli->message = strdup("ABORTED");
		pane_damaged(mli->line, DAMAGED_CONTENT);
		return 0;
	}
	if (strcmp(ci->key, "Refresh:size") == 0) {
		if (mli->height == 0) {
			struct call_return cr;
			cr.c = text_size_callback;
			call_comm7("text-size", ci->home, -1, NULL,
				   "M", 0, "bold", &cr.c);
			mli->height = cr.y;
			mli->ascent = cr.i;
		}

		if (mli->hidden) {
			pane_resize(mli->line, 0, ci->home->h,
				    ci->home->w, mli->height);
			if (mli->child)
				pane_resize(mli->child, 0, 0,
					    ci->home->w, ci->home->h);
		} else {
			pane_resize(mli->line, 0, ci->home->h - mli->height,
				    ci->home->w, mli->height);
			if (mli->child && ci->home->h > mli->height)
				pane_resize(mli->child, 0, 0,
					    ci->home->w,
					    ci->home->h - mli->height);
		}
		return 1;
	}
	if (strcmp(ci->key, "ChildRegistered") == 0) {
		mli->child = ci->focus;
		pane_damaged(ci->home, DAMAGED_SIZE);
		pane_focus(ci->focus);
		return 1;
	}
	/* Keystroke notification clears the message line */
	if ((strcmp(ci->key, "Notify:Keystroke") == 0
	     || strcmp(ci->key, "Keystroke") == 0)
	    && mli->message) {
		free(mli->message);
		mli->message = NULL;
		pane_drop_notifiers(ci->home, "Notify:Keystroke");
		pane_damaged(mli->line, DAMAGED_CONTENT);
		return 1;
	}
	return 0;
}

DEF_CMD(messageline_line_handle)
{
	struct mlinfo *mli = ci->home->data;

	if (strcmp(ci->key, "Refresh") == 0) {
		pane_clear(mli->line, "bg:white");
		if (mli->message)
			pane_str(mli->line, mli->message, "bold,fg:red,bg:cyan",
				 0, 0 + mli->ascent);
		return 0;
	}
	return 0;
}
static struct pane *do_messageline_attach(struct pane *p)
{
	struct mlinfo *mli = calloc(1, sizeof(*mli));
	struct pane *ret;

	ret = pane_register(p, 0, &messageline_handle, mli, NULL);
	/* z=1 to avoid clone_children affecting it */
	mli->line = pane_register(ret, 1, &messageline_line_handle, mli, NULL);
	pane_focus(p);

	return ret;
}

DEF_CMD(messageline_attach)
{
	struct pane *ret;

	ret = do_messageline_attach(ci->focus);
	if (!ret)
		return -1;
	return comm_call(ci->comm2, "callback:attach", ret, 0, NULL, NULL, 0);
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, 0, NULL, "attach-messageline",
		  0, &messageline_attach);
}
