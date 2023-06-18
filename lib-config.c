/*
 * Copyright Neil Brown ©2023 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * Read an "ini" config file and set some attributes.
 * Sections:
 *   global - set attr on editor
 *   module - set trigger to load module
 *   file:pattern - set attributes when matching file visited
 *         (not implemented fully yet)
 *
 * When not in a section, include= will load another file.
 *
 * Syntax for ini file
 * - individual lines must not exceed 256 chars.  Longer lines are
 *   silently truncated.
 * - leading white space continues the previous line, this allowing large
 *   values.  The newline and leading white space are stripped.
 * - white space around "=", at EOL, and around section name in [section]
 *   is stripped
 * - Double quotes at both ends of a value are stripped.
 * - If first non-white is '#', line is ignored.
 * - Everything after closing ']' of section is ignored
 * - If value is no quoted, everything after first '#' is ignored
 * - blank lines are ignored
 */

#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdio.h>
#include <ctype.h>
#include "core.h"

typedef void (*ini_handle)(void *data, char *section safe,
			   char *name safe, char *value safe,
			   const char *path safe,
			   int append);
static void load_config(const char *path safe, struct pane *ed, const char *base);

static void parse_ini(const char *path safe, ini_handle handle, void *data)
{
	FILE *f = fopen(path, "r");
	char line[257];
	char section[257] = "";
	char name[257] = "";

	if (!f)
		return;
	while (fgets(line, sizeof(line), f) != NULL) {
		char *st, *eq;
		char *eol = strchr(line, '\n');
		int append, quote;

		if (!eol) {
			int ch;
			while ((ch = fgetc(f)) != '\n' &&
			       ch != EOF)
				;
		} else
			*eol = 0;
		if (line[0] == '[') {
			eol = strchr(line, ']');
			if (!eol)
				continue;
			while (eol > line && isblank(eol[-1]))
				eol -= 1;
			*eol = 0;
			st = line+1;
			while (isblank(*st))
				st += 1;
			strcpy(section, st);
			name[0] = 0;
			continue;
		}
		/* find/strip comment */
		st = line; quote = 0;
		while (*st && (quote || *st != '#')) {
			if (*st == '"')
				quote = !quote;
			st += 1;
		}
		if (*st  == '#')
			*st = 0;
		if (isblank(line[0])) {
			if (!name[0])
				/* Nothing to continue */
				continue;
			st = line;
			while (isblank(*st))
				st += 1;
			if (!*st)
				/* Blank line */
				continue;
			append = 1;
		} else {
			name[0] = 0;
			/* There must be an '=' */
			eq = strchr(line, '=');
			if (!eq)
				continue;
			st = eq + 1;
			while (eq > line && isblank(eq[-1]))
				eq -= 1;
			*eq = 0;
			if (!line[0])
				/* No name before '=' */
				continue;
			strcpy(name, line);
			append = 0;
		}
		/* A value is at 'st', to be set or appended */
		eol = st + strlen(st);
		while (isblank(*st))
			st += 1;
		while (eol > st && isblank(eol[-1]))
			eol -= 1;
		if (*st == '"' && eol > st + 1 && eol[-1] == '"') {
			st += 1;
			eol -= 1;
		}
		*eol = 0;
		handle(data, section, name, st, path, append);
	}
	fclose(f);
}

struct mod_cmd {
	char *module;
	int tried;
	struct command c;
};

DEF_CB(autoload)
{
	struct mod_cmd *mc = container_of(ci->comm, struct mod_cmd, c);

	if (mc->tried)
		return Efallthrough;
	mc->tried = 1;

	/* NOTE: this might free mc, so don't touch it again */
	call("global-load-module", ci->home, 0, NULL, mc->module);
	return home_call(ci->home, ci->key, ci->focus,
			 ci->num, ci->mark, ci->str,
			 ci->num2, ci->mark2, ci->str2,
			 ci->x, ci->y, ci->comm2);
}

static void al_free(struct command *c safe)
{
	struct mod_cmd *mc = container_of(c, struct mod_cmd, c);

	free(mc->module);
	free(mc);
}

static void handle(void *data, char *section safe, char *name safe, char *value safe,
		   const char *path safe, int append)
{
	struct pane *p = data;

	if (!p)
		return;

	if (strcmp(section, "") == 0) {
		if (strcmp(name, "include") == 0) {
			load_config(value, p, path);
			return;
		}
		return;
	}

	if (strcmp(section, "global") == 0) {
		call("global-set-attr", p, append, NULL, name,
		     0, NULL, value);
		return;
	}

	if (strcmp(section, "module") == 0 && value[0]) {
		struct mod_cmd *mc;
		if (strcmp(value, "ALWAYS") == 0) {
			call("global-load-module", p, 0, NULL, name);
			return;
		}
		mc = malloc(sizeof(*mc));
		mc->module = strdup(name);
		mc->tried = 0;
		mc->c = autoload;
		mc->c.free = al_free;
		call_comm("global-set-command", p, &mc->c, 0, NULL,
			  value);
		return;
	}

	if (strstarts(section, "file:")) {
		char *k = strconcat(NULL, "global-file-attr:", section+5);
		call(k, p, append, NULL, name, 0, NULL, value);
		return;
	}
}

static void load_config(const char *path safe, struct pane *ed, const char *base)
{
	char *sl, *p, *h;
	if (*path == '/') {
		parse_ini(path, handle, ed);
		return;
	}
	/*
	 * Relative paths can be loaded from:
	 * dirname(base)
	 * /usr/share/edlib/
	 * $HOME/.config/edlib/
	 */
	if (base && (sl = strrchr(base, '/')) != NULL) {
		sl += 1;
		p = malloc((sl - base) + strlen(path) + 1);
		memcpy(p, base, sl - base);
		strcpy(p + (sl - base), path);
		if (access(p, F_OK) == 0) {
			parse_ini(p, handle, ed);
			free(p);
			return;
		}
		free(p);
	}
	p = strconcat(NULL, "/usr/share/edlib/", path);
	if (access(p, F_OK) == 0) {
		parse_ini(p, handle, ed);
		free(p);
		return;
	}
	free(p);

	h = getenv("HOME");
	if (h == NULL || *h != '/')
		return;
	p = strconcat(NULL, h, "/.config/edlib/", path);
	if (access(p, F_OK) == 0) {
		parse_ini(p, handle, ed);
		free(p);
		return;
	}
	free(p);
}

DEF_CMD(config_load)
{
	if (ci->str)
		load_config(ci->str, ci->home, ci->str2);
	return 1;
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, &config_load,
		  0, NULL, "config-load");
}
