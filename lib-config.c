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
 * When not in a section, or in the "include" section, include= will
 * load another file.
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
static void load_config(const char *path safe, void *data, const char *base);

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

static bool __glob_match(const char *patn safe, const char *path safe)
{
	while(1) {
		switch (*patn) {
		case '\0':
			return *path == '\0';
		case '?':
			if (!*path || *path == '/')
				return False;
			patn += 1;
			path += 1;
			break;
		case '*':
			if (patn[1] == '*') {
				if (__glob_match(patn+2, path))
					return True;
			} else {
				if (__glob_match(patn+1, path))
					return True;
				if (*path == '/')
					return False;
			}
			if (!*path)
				return False;
			path += 1;
			break;
		default:
			if (*patn != *path)
				return False;
			patn += 1;
			path += 1;
			break;
		}
	}
}

static bool glob_match(const char *patn safe, const char *path safe)
{
	int ret;
	if (patn[0] != '/' && !strstarts(patn, "**")) {
		/* must match basename */
		const char *sl = strrchr(path, '/');
		if (sl)
			path = sl + 1;
	}
	ret = __glob_match(patn, path);
	return ret;
}

struct config_data {
	struct command c;
	struct command appeared;
	struct pane *root safe;
	struct trigger {
		char *path safe;
		struct attrset *attrs;
		struct trigger *next;
	} *triggers, *last_trigger;
};

static void add_trigger(struct config_data *cd safe, char *path safe,
			char *name safe, char *val safe, int append)
{
	struct trigger *t = cd->last_trigger;

	if (strstarts(name, "TESTING ")) {
		if (!edlib_testing(cd->root))
			return;
		name += 8;
	}
	if (strstarts(name, "NOTESTING ")) {
		if (edlib_testing(cd->root))
			return;
		name += 10;
	}
	if (!t || strcmp(t->path, path) != 0) {
		alloc(t, pane);
		t->path = strdup(path);
		t->next = NULL;
		if (cd->last_trigger)
			cd->last_trigger->next = t;
		else
			cd->triggers = t;
		cd->last_trigger = t;
	}
	if (append) {
		const char *old = attr_find(t->attrs, name);
		if (old) {
			val = strconcat(NULL, old, val);
			attr_set_str(&t->attrs, name, val);
			free(val);
		} else
			attr_set_str(&t->attrs, name, val);
	} else
		attr_set_str(&t->attrs, name, val);
}

static void config_file(char *path safe, struct pane *doc safe,
			struct config_data *cd safe)
{
	struct trigger *t;

	for (t = cd->triggers; t; t = t->next)
		if (glob_match(t->path, path)) {
			const char *val;
			const char *k = "";
			while ((k = attr_get_next_key(t->attrs, k, -1, &val)) != NULL) {
				if (strstarts(k, "APPEND "))
					call("doc:append:", doc, 0, NULL, val,
					     0, NULL, k + 7);
				else
					call("doc:set:", doc, 0, NULL, val,
					     0, NULL, k);
			}
		}
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
	struct config_data *cd;

	if (!data)
		return;
	cd = data;

	if (strcmp(section, "") == 0 || strcmp(section,"include") == 0) {
		if (strcmp(name, "include") == 0) {
			load_config(value, data, path);
			return;
		}
		return;
	}

	if (strcmp(section, "global") == 0) {
		call("global-set-attr", cd->root, append, NULL, name,
		     0, NULL, value);
		return;
	}

	if (strcmp(section, "module") == 0 && value[0]) {
		struct mod_cmd *mc;

		mc = malloc(sizeof(*mc));
		mc->module = strdup(name);
		mc->tried = 0;
		mc->c = autoload;
		mc->c.free = al_free;
		if (strstarts(value, "PREFIX "))
			call_comm("global-set-command-prefix", cd->root, &mc->c, 0, NULL,
				  value + 7);
		else
			call_comm("global-set-command", cd->root, &mc->c, 0, NULL,
				  value);
		return;
	}

	if (strstarts(section, "file:")) {
		add_trigger(cd, section+5, name, value, append);
		return;
	}
}

static void load_config(const char *path safe, void *data, const char *base)
{
	char *sl, *p, *h;
	if (*path == '/') {
		parse_ini(path, handle, data);
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
			parse_ini(p, handle, data);
			free(p);
			return;
		}
		free(p);
	}
	p = strconcat(NULL, "/usr/share/edlib/", path);
	if (access(p, F_OK) == 0) {
		parse_ini(p, handle, data);
		free(p);
		return;
	}
	free(p);

	h = getenv("HOME");
	if (h == NULL || *h != '/')
		return;
	p = strconcat(NULL, h, "/.config/edlib/", path);
	if (access(p, F_OK) == 0) {
		parse_ini(p, handle, data);
		free(p);
		return;
	}
	free(p);
}

static void config_free(struct command *c safe)
{
	struct config_data *cd = container_of(c, struct config_data, c);
	struct trigger *t;

	while ((t = cd->triggers) != NULL) {
		cd->triggers = t->next;
		free(t->path);
		attr_free(&t->attrs);
		free(t);
	}
	free(cd);
}

DEF_CMD(config_appeared)
{
	struct config_data *cd = container_of(ci->comm, struct config_data, appeared);
	char *path = pane_attr_get(ci->focus, "filename");
	if (!path)
		return Efallthrough;
	config_file(path, ci->focus, cd);
	return Efallthrough;
}

DEF_CMD(config_load)
{
	struct config_data *cd;
	if (ci->comm == &config_load) {
		/* This is the first call - need to allocate storage
		 * and register a new command.
		 */
		alloc(cd, pane);
		cd->c = config_load;
		cd->c.free = config_free;
		cd->appeared = config_appeared;
		cd->root = ci->home;
		call_comm("global-set-command", ci->home, &cd->c, 0, NULL, "config-load");
		call_comm("global-set-command", ci->home, &cd->appeared,
			  0, NULL, "doc:appeared-config");
	} else {
		cd = container_of(ci->comm, struct config_data, c);
	}
	if (ci->str)
		load_config(ci->str, cd, ci->str2);
	return 1;
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, &config_load,
		  0, NULL, "config-load");
}
