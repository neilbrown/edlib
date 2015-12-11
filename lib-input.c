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
	char	*mode;
	int	numeric, extra;
};


DEF_CMD(set_mode)
{
	struct input_mode *im = ci->home->data;

	im->mode = ci->str;
	return 1;
}

DEF_CMD(set_numeric)
{
	struct input_mode *im = ci->home->data;

	im->numeric = ci->numeric;
	return 1;
}

DEF_CMD(set_extra)
{
	struct input_mode *im = ci->home->data;

	im->extra = ci->extra;
	return 1;
}

DEF_CMD(keystroke)
{
	struct cmd_info ci2 = {0};
	struct input_mode *im = ci->home->data;
	int l;

	l = strlen(im->mode) + strlen(ci->str) + 1;
	ci2.key = malloc(l);
	strcat(strcpy(ci2.key, im->mode), ci->str);
	ci2.focus = ci->home;
	ci2.numeric = im->numeric;
	ci2.extra = im->extra;

	im->mode = "";
	im->numeric = NO_NUMERIC;
	im->extra = 0;
	key_handle_focus_point(&ci2);
	free(ci2.key);
	return 0;
}

DEF_CMD(mouse_event)
{
	struct input_mode *im = ci->home->data;
	int l;
	struct cmd_info ci2 = {0};


	l = strlen(im->mode) + strlen(ci->str) + 1;
	ci2.key = malloc(l);
	strcat(strcpy(ci2.key, im->mode), ci->str);
	ci2.focus = ci->home;
	ci2.numeric = im->numeric;
	ci2.extra = im->extra;
	ci2.x = ci->x; ci2.y = ci->y;
	pane_map_xy(ci->focus, ci2.focus, &ci2.x, &ci2.y);

	im->mode = "";
	im->numeric = NO_NUMERIC;
	im->extra = 0;

	key_handle_xy_point(&ci2);
	return 0;
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
	key_add(im_map, "Mode:set-numeric", &set_numeric);
	key_add(im_map, "Mode:set-extra", &set_extra);
}

DEF_LOOKUP_CMD(input_handle, im_map);
DEF_CMD(input_attach)
{
	struct input_mode *im = malloc(sizeof(*im));
	struct pane *p;

	register_map();

	im->mode = "";
	im->numeric = NO_NUMERIC;
	im->extra = 0;

	p = pane_register(ci->focus, 0, &input_handle.c, im, NULL);
	if (p)
		return comm_call(ci->comm2, "callback:attach", p, 0, NULL, NULL, 0);
	return -1;
}

void edlib_init(struct editor *ed)
{
	key_add(ed->commands, "attach-input", &input_attach);
}
