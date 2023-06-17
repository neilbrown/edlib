/*
 * Copyright Neil Brown ©2015-2021 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * trim a line off the bottom of a pane and capture messages
 * to go there.  They disappear on the next keystroke.
 *
 * Later it might be good to allow boarderless popups to appear here.
 *
 * The message displayed is:
 *  a 'modal' message until a keystroke, or
 *  a normal message which remains until it has been visible without a modal for
 *     seven seconds with keystrokes, or 30 seconds without keystrokes, or
 *  a 'default' message ... which is hardly used, or
 *  a the current time
 *
 * Refreshed about every 15 seconds, so timestamp can be a little out of date
 * but not much.
 */

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "core.h"

struct mlinfo {
	char *message;
	char *modal;	/* message displays a mode, and must
			 * remain exactly until a keystroke
			 */
	struct pane *line safe, *child;
	struct pane *log;
	int height; /* height of line */
	int ascent; /* how far down to baseline */
	int hidden;
	time_t last_message; /* message should stay for at least 10 seconds */
};
static struct pane *do_messageline_attach(struct pane *p safe);
static struct map *messageline_map, *messageline_line_map;
DEF_LOOKUP_CMD(messageline_handle, messageline_map);
DEF_LOOKUP_CMD(messageline_line_handle, messageline_line_map);

static void pane_str(struct pane *p safe, char *s, char *attr, int x, int y)
{
	call("Draw:text", p, -1, NULL, s, 0, NULL, attr, x, y);
}

DEF_CMD(messageline_clone)
{
	struct pane *p = do_messageline_attach(ci->focus);

	pane_clone_children(ci->home, p);
	return 1;
}

DEF_CMD(messageline_border)
{
	struct mlinfo *mli = ci->home->data;
	if (ci->num > 0)
		mli->hidden = 0;
	else
		mli->hidden = 1;
	/* trigger a resize of children */
	pane_damaged(ci->home, DAMAGED_SIZE);
	return Efallthrough; /* Allow other panes to remove other borders */
}

DEF_CMD(messageline_msg)
{
	struct mlinfo *mli = ci->home->data;

	if (ci->str && (strcmp(ci->key, "Message:default") != 0 ||
			mli->message == NULL)) {
		if (!mli->message) {
			call("window:request:Keystroke-notify", ci->home);
			call("window:request:Mouse-event-notify", ci->home);
		}
		if (strcmp(ci->key, "Message:modal") == 0) {
			free(mli->modal);
			mli->modal = strdup(ci->str);
		} else {
			free(mli->message);
			mli->message = strdup(ci->str);
			/* x==0 check ensures we only append message once when
			 * it comes in via a broadcast notification
			 */
			if (ci->x == 0 && mli->log)
				call("doc:log:append", mli->log,
				     0, NULL, ci->str);
		}
		time(&mli->last_message);
		pane_damaged(mli->line, DAMAGED_REFRESH);
	}
	if (strcmp(ci->key, "Message:broadcast") == 0)
		return 1; /* Acknowledge message */
	else
		return Efallthrough; /* allow other handlers */
}

DEF_CMD(messageline_abort)
{
	struct mlinfo *mli = ci->home->data;

	if (!mli->message) {
		call("window:request:Keystroke-notify", ci->home);
		call("window:request:Mouse-event-notify", ci->home);
	}
	free(mli->message);
	mli->message = strdup("ABORTED");
	free(mli->modal);
	mli->modal = NULL;
	time(&mli->last_message);
	pane_damaged(mli->line, DAMAGED_REFRESH);
	return Efallthrough;
}

DEF_CMD(messageline_refresh_size)
{
	struct mlinfo *mli = ci->home->data;
	if (mli->height == 0) {
		struct call_return cr =
			call_ret(all, "Draw:text-size", ci->home, -1, NULL, "M",
				 0, NULL, "bold");
		mli->height = cr.y;
		mli->ascent = cr.i2;
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
	pane_damaged(ci->home, DAMAGED_REFRESH);
	pane_damaged(mli->line, DAMAGED_REFRESH);
	return 1;
}

DEF_CMD(messageline_child_notify)
{
	struct mlinfo *mli = ci->home->data;
	if (ci->focus->z)
		/* Ignore */
		return 1;
	if (ci->num < 0) {
		if (ci->home->focus == ci->focus)
			ci->home->focus = NULL;
		mli->child = NULL;
	} else {
		if (mli->child)
			pane_close(mli->child);
		mli->child = ci->focus;
		ci->home->focus = ci->focus;
	}
	return 1;
}

DEF_CMD(messageline_notify)
{
	/* Keystroke notification clears the message line */
	struct mlinfo *mli = ci->home->data;
	int wait_time = 7;

	if (getenv("EDLIB_TESTING"))
		wait_time = 0;

	if (mli->modal) {
		free(mli->modal);
		mli->modal = NULL;
		if (mli->message)
			mli->last_message = time(NULL);
		pane_damaged(mli->line, DAMAGED_REFRESH);
	}
	if (mli->message &&
	    time(NULL) >= mli->last_message + wait_time) {
		free(mli->message);
		mli->message = NULL;
		pane_damaged(mli->line, DAMAGED_REFRESH);
	}
	if (!mli->message && !mli->modal) {
		pane_drop_notifiers(ci->home, "Keystroke-notify");
		pane_drop_notifiers(ci->home, "Mouse-event-notify");
	}
	return 1;
}

DEF_CMD(messageline_line_refresh)
{
	struct mlinfo *mli = ci->home->data;

	call("Draw:clear", mli->line, 0, NULL, "bg:white");
	if (mli->message && !mli->modal &&
	    time(NULL) >= mli->last_message + 30) {
		free(mli->message);
		mli->message = NULL;
		pane_drop_notifiers(ci->home, "Keystroke-notify");
		pane_drop_notifiers(ci->home, "Mouse-event-notify");
	}
	if (mli->modal)
		pane_str(mli->line, mli->modal, "bold,fg:magenta-60,bg:white",
			 0, 0 + mli->ascent);
	else if (mli->message)
		pane_str(mli->line, mli->message, "bold,fg:red,bg:cyan",
			 0, 0 + mli->ascent);
	else {
		char buf[80];
		time_t t;
		struct tm *tm;
		char *testing = getenv("EDLIB_TESTING");
		t = time(NULL);
		if (testing && *testing)
			t = 1581382278;
		tm = localtime(&t);
		if (tm)
			strftime(buf, sizeof(buf), "%H:%M %d-%b-%Y", tm);
		else
			buf[0] = 0;
		pane_str(mli->line, buf, "bold,fg:blue",
			 0, 0 + mli->ascent);
	}
	return 1;
}

DEF_CMD(force_refresh)
{
	pane_damaged(ci->home, DAMAGED_REFRESH);
	return 1;
}

static struct pane *do_messageline_attach(struct pane *p safe)
{
	struct mlinfo *mli;
	struct pane *ret, *mlp;

	alloc(mli, pane);
	ret = pane_register(p, 0, &messageline_handle.c, mli);
	if (!ret)
		return NULL;
	call("editor:request:Message:broadcast", ret);
	/* z=1 to avoid clone_children affecting it */
	mlp = pane_register(ret, 1, &messageline_line_handle.c, mli);
	if (!mlp) {
		pane_close(ret);
		return NULL;
	}
	mli->line = mlp;
	pane_focus(ret);
	if (getenv("EDLIB_TESTING") == NULL)
		/* This can introduce unwanted variablitiy in tests */
		call_comm("event:timer", mli->line, &force_refresh, 15000);

	mli->log = call_ret(pane, "docs:byname", p, 0, NULL, "*Messages*");
	if (!mli->log)
		mli->log = call_ret(pane, "log:create", p, 0, NULL,
				    "*Messages*");

	return ret;
}

DEF_CMD(messageline_attach)
{
	struct pane *ret;

	ret = do_messageline_attach(ci->focus);
	if (!ret)
		return Efail;
	return comm_call(ci->comm2, "callback:attach", ret);
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, &messageline_attach, 0, NULL,
		  "attach-messageline");

	if (messageline_map)
		return;
	messageline_map = key_alloc();
	key_add(messageline_map, "Clone", &messageline_clone);
	key_add(messageline_map, "Free", &edlib_do_free);
	key_add(messageline_map, "Display:border", &messageline_border);
	key_add(messageline_map, "Message", &messageline_msg);
	key_add(messageline_map, "Message:modal", &messageline_msg);
	key_add(messageline_map, "Message:default", &messageline_msg);
	key_add(messageline_map, "Message:broadcast", &messageline_msg);
	key_add(messageline_map, "Abort", &messageline_abort);
	key_add(messageline_map, "Refresh:size", &messageline_refresh_size);
	key_add(messageline_map, "Child-Notify",&messageline_child_notify);
	key_add(messageline_map, "Keystroke-notify", &messageline_notify);
	key_add(messageline_map, "Mouse-event-notify", &messageline_notify);

	messageline_line_map = key_alloc();
	key_add(messageline_line_map, "Refresh", &messageline_line_refresh);
}
