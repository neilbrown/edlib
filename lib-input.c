/*
 * Copyright Neil Brown ©2015 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * Core input translation.
 * This module transalates keystrokes and mouse events into commands.
 * This involves tracking the current 'mode' state.
 *
 */

#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include "core.h"

struct input_mode {
	char		*mode safe;
	int		num, num2;
	struct pane	*focus, *source;
	struct mark	*point;
};

DEF_CMD(set_mode)
{
	struct input_mode *im = ci->home->data;

	if (!ci->str)
		return -1;
	im->mode = ci->str;
	return 1;
}

DEF_CMD(set_num)
{
	struct input_mode *im = ci->home->data;

	im->num = ci->num;
	return 1;
}

DEF_CMD(set_num2)
{
	struct input_mode *im = ci->home->data;

	im->num2 = ci->num;
	return 1;
}

DEF_CMD(keystroke)
{
	char *key;
	struct input_mode *im = ci->home->data;
	struct pane *p;
	int l;
	int ret;
	int num = im->num;
	int num2 = im->num2;
	struct mark *m;

	if (!ci->str)
		return -1;

	pane_notify("Notify:Keystroke", ci->home, 0, NULL, ci->str);

	l = strlen(im->mode) + strlen(ci->str) + 1;
	key = malloc(l);
	strcat(strcpy(key, im->mode), ci->str);

	im->mode = "";
	im->num = NO_NUMERIC;
	im->num2 = 0;

	if (im->source != ci->focus) {
		im->source = ci->focus;
		im->focus = NULL;
		im->point = NULL;
	}

	if (!im->focus) {
		p = ci->focus;
		while (p->focus)
			p = p->focus;
		im->focus = p;
		pane_add_notify(ci->home, p, "Notify:Close");
	}
	p = im->focus;

	if (!im->point)
		im->point = call_ret(mark, "doc:point", p);
	m = im->point;

	ret = call(key, p, num, m, NULL, num2);
	free(key);
	if (ret < 0)
		call("Message", ci->focus, 0, NULL, "** Command Failed **", 1);
	return 0;
}

DEF_CMD(mouse_event)
{
	struct input_mode *im = ci->home->data;
	int l;
	int x,y;
	int num, ex;
	struct pane *focus;
	char *key;

	if (!ci->str)
		return -1;

	pane_notify("Notify:Mouse-event", ci->home, 0, NULL, ci->str);

	l = strlen(im->mode) + strlen(ci->str) + 1;
	key = malloc(l);
	strcat(strcpy(key, im->mode), ci->str);
	focus = ci->focus;
	num = im->num;
	ex = im->num2;
	x = ci->x; y = ci->y;
	/* FIXME is there any point in this? */
	pane_map_xy(ci->focus, focus, &x, &y);

	im->mode = "";
	im->num = NO_NUMERIC;
	im->num2 = 0;

	while (1) {
		struct pane *t, *chld = NULL;

		list_for_each_entry(t, &focus->children, siblings) {
			if (x < t->x || x >= t->x + t->w)
				continue;
			if (y < t->y || y >= t->y + t->h)
				continue;
			if (chld == NULL || t->z > chld->z)
				chld = t;
		}
		/* descend into chld */
		if (!chld)
			break;
		x -= chld->x;
		y -= chld->y;
		focus = chld;
	}

	call(key, focus, num, NULL, NULL, ex, NULL, NULL, x, y);
	return 0;
}

DEF_CMD(request_notify)
{
	if (strcmp(ci->key, "Request:Notify:Keystroke") == 0) {
		pane_add_notify(ci->focus, ci->home, "Notify:Keystroke");
		return 1;
	}
	if (strcmp(ci->key, "Request:Notify:Mouse-event") == 0) {
		pane_add_notify(ci->focus, ci->home, "Notify:Mouse-event");
		return 1;
	}
	return 0;
}

DEF_CMD(refocus)
{
	struct input_mode *im = ci->home->data;

	im->focus = NULL;
	im->point = NULL;
	im->source = NULL;
	return 0;
}

DEF_CMD(close_focus)
{
	struct input_mode *im = ci->home->data;

	if (im->focus == ci->focus) {
		im->focus = NULL;
		im->point = NULL;
		im->source = NULL;
	}
	return 1;
}

static struct map *im_map;
static void register_map(void)
{
	if (im_map)
		return;
	im_map = key_alloc();
	key_add(im_map, "Keystroke", &keystroke);
	key_add(im_map, "Mouse-event", &mouse_event);
	key_add(im_map, "Mode:set-mode", &set_mode);
	key_add(im_map, "Mode:set-num", &set_num);
	key_add(im_map, "Mode:set-num2", &set_num2);
	key_add(im_map, "pane:refocus", &refocus);
	key_add(im_map, "Notify:Close", &close_focus);
	key_add_range(im_map, "Request:Notify:", "Request:Notify;", &request_notify);
}

DEF_LOOKUP_CMD(input_handle, im_map);
DEF_CMD(input_attach)
{
	struct input_mode *im = calloc(1,sizeof(*im));
	struct pane *p;

	register_map();

	im->mode = "";
	im->num = NO_NUMERIC;
	im->num2 = 0;

	p = pane_register(ci->focus, 0, &input_handle.c, im, NULL);
	if (p)
		return comm_call(ci->comm2, "callback:attach", p);
	return -1;
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, &input_attach, 0, NULL, "attach-input");
}
