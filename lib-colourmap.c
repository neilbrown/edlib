/*
 * Copyright Neil Brown ©2019 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * Provide a colour-name service.
 * A client (normally a display) call "colour:map" passing a
 * string name of a colour.  The callback is passed the chosen colour
 * as RGB values (0-1000) in num1 num2 and x, and as string in hex format #RRggBB
 * Alternate interfaces are "colour:map:bg" and "colour:map:fg"
 * The are passes a reference colour in 'str2' (#rrggbb).
 * for colour:map:bg, the reference should be the default background
 * for colour:map:fg, the reference should be the chosen background.
 * One day these might modify the reult to provide better contrast.
 *
 * Colours have a base name and modifiers.
 * The base name is either a word (e.g. "green" "rebeccapurple") or a
 * hex colour #rrggbb.  In either case, case is ignored.
 * The modifier can adjust value or saturation.
 * -nn (0-99) reduces the brightness - scales each channel towards zero.
 *    0 means black. 99 means no-change
 * +nn (0-99) reduces saturation - scales each channel towards max.
 *    0 means no change, 99 means white.
 * So "white-50" and "black+50" are both mid-grey.  "yellow-90+90" is a pale yellow.
 */

#define _GNU_SOURCE for_asprintf
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "core.h"

static struct colours {
	char *name;
	int r,g,b;
} colours[] = {
	{ "black",       0,    0,    0 },
	{ "white",    1000, 1000, 1000 },
	{ "red",      1000,    0,    0 },
	{ "green",       0, 1000,    0 },
	{ "blue",        0,    0, 1000 },
	{ "yellow",   1000, 1000,    0 },
	{ "magenta",  1000,    0, 1000 },
	{ "cyan",        0, 1000, 1000 },
	{ "darkblue",    0,    0,  550 },
	{ "purple",    500,    0,  500 },
	{ "grey",      500,  500,  500 },
	{ "pink",     1000,  800,  800 },
};

static void gethex(char *col safe, int rgb[])
{
	char b[3];
	int i;

	b[2] = 0;
	if (strlen(col) != 6)
		return;
	for (i = 0; i < 3; i++) {
		b[0] = col[i*2];
		b[1] = col[i+2+1];
		rgb[i] = strtoul(b, NULL, 16) * 1000 / 255;
	}
}

static bool find_colour(char *col, int rgb[])
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(colours); i++) {
		if (strcasecmp(col, colours[i].name) == 0) {
			rgb[0] = colours[i].r;
			rgb[1] = colours[i].g;
			rgb[2] = colours[i].b;
			return True;
		}
	}
	return False;
}

static void add_value(char *n, int rgb[])
{
	int scale = atoi(n);
	int i;

	if (scale < 0 || scale > 99)
		return;
	for (i = 0; i < 3; i++)
		rgb[i] = rgb[i] * scale / 99;
}

static void add_sat(char *n, int rgb[])
{
	int scale = atoi(n);
	int i;

	if (scale < 0 || scale > 99)
		return;
	for (i = 0; i < 3; i++)
		rgb[i] = 1000 - ( (99 - scale) * (1000 - rgb[i]) / 99);
}


DEF_CMD(colour_map)
{
	char *col;
	char *m, *p;
	int rgb[3] = { 500, 500, 500};

	if (!ci->str)
		return Enoarg;
	col = strdup(ci->str);
	p = strchr(col, '+');
	if (p)
		*p++ = 0;
	m = strchr(col, '-');
	if (m)
		*m++ = 0;
	if (col[0] == '#')
		gethex(col+1, rgb);
	else if (!find_colour(col, rgb))
		find_colour("grey", rgb);
	if (m)
		add_value(m, rgb);
	if (p)
		add_sat(p, rgb);
	free(col);
	asprintf(&col, "#%02x%02x%02x", rgb[0]*255/1000,
		 rgb[1]*255/1000, rgb[2]*255/1000);
	return comm_call(ci->comm2, "colour:callback", ci->focus,
			 rgb[0], NULL, col,
			 rgb[1], NULL, NULL, rgb[2], 0);
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, &colour_map,
		  0, NULL, "colour:map",
		  0, NULL, "colour:map;");
}
