/*
 * Copyright Neil Brown ©2015-2020 <neil@brown.name>
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
#include <time.h>


#include "core.h"

struct mlinfo {
	char *message;
	int modal;	/* message displays a mode, and must
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
	pane_damaged(ci->home, DAMAGED_SIZE);
	return 0; /* Allow other panes to remove other borders */
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
		free(mli->message);
		mli->message = strdup(ci->str);
		mli->modal = strcmp(ci->key, "Message:modal") == 0;
		time(&mli->last_message);
		pane_damaged(mli->line, DAMAGED_CONTENT);
		/* x==0 check ensures we only append message once when
		 * it comes in via a broadcast notification
		 */
		if (ci->x == 0 && mli->log)
			call("doc:log:append", mli->log, 0, NULL, ci->str);
	}
	return 0; /* allow other handlers */
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
	mli->modal = 0;
	time(&mli->last_message);
	pane_damaged(mli->line, DAMAGED_CONTENT);
	return 0;
}

DEF_CMD(messageline_refresh_size)
{
	struct mlinfo *mli = ci->home->data;
	if (mli->height == 0) {
		struct call_return cr =
			call_ret(all, "text-size", ci->home, -1, NULL, "M",
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
	return 1;
}

DEF_CMD(messageline_child_registered)
{
	struct mlinfo *mli = ci->home->data;
	mli->child = ci->focus;
	pane_damaged(ci->home, DAMAGED_SIZE);
	pane_focus(ci->focus);
	return 1;
}

DEF_CMD(messageline_notify)
{
	/* Keystroke notification clears the message line */
	struct mlinfo *mli = ci->home->data;

	if (mli->message &&
	    (mli->modal || time(NULL) >= mli->last_message + 7)) {
		free(mli->message);
		mli->message = NULL;
		pane_drop_notifiers(ci->home, "Keystroke-notify");
		pane_drop_notifiers(ci->home, "Mouse-event-notify");
		pane_damaged(mli->line, DAMAGED_CONTENT);
	}
	return 1;
}

DEF_CMD(messageline_line_refresh)
{
	struct mlinfo *mli = ci->home->data;

	call("pane-clear", mli->line, 0, NULL, "bg:white");
	if (mli->message && !mli->modal &&
	    time(NULL) >= mli->last_message + 30) {
		free(mli->message);
		mli->message = NULL;
		pane_drop_notifiers(ci->home, "Keystroke-notify");
		pane_drop_notifiers(ci->home, "Mouse-event-notify");
	}
	if (mli->message)
		pane_str(mli->line, mli->message, "bold,fg:red,bg:cyan",
			 0, 0 + mli->ascent);
	else {
		char buf[80];
		time_t t;
		struct tm *tm;
		char *faketime = getenv("EDLIB_FAKE_TIME");
		t = time(NULL);
		if (faketime)
			t = strtoul(faketime, NULL, 10);
		tm = localtime(&t);
		if (tm)
			strftime(buf, sizeof(buf), "%H:%M %d-%b-%Y", tm);
		else
			buf[0] = 0;
		pane_str(mli->line, buf, "bold,fg:blue",
			 0, 0 + mli->ascent);
	}
	return 0;
}

DEF_CMD(force_refresh)
{
	pane_damaged(ci->home, DAMAGED_CONTENT);
	return 1;
}

static struct pane *do_messageline_attach(struct pane *p safe)
{
	struct mlinfo *mli;
	struct pane *ret;

	alloc(mli, pane);
	ret = pane_register(p, 0, &messageline_handle.c, mli);
	call("editor:request:Message:broadcast", ret);
	/* z=1 to avoid clone_children affecting it */
	mli->line = pane_register(ret, 1, &messageline_line_handle.c, mli);
	pane_focus(ret);
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
	key_add(messageline_map, "ChildRegistered",
		&messageline_child_registered);
	key_add(messageline_map, "Keystroke-notify", &messageline_notify);
	key_add(messageline_map, "Mouse-event-notify", &messageline_notify);

	messageline_line_map = key_alloc();
	key_add(messageline_line_map, "Refresh", &messageline_line_refresh);
}
