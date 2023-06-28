/*
 * Copyright Neil Brown ©2023 <neil@brown.name>
 * May be distributed under terms of GPLv2 - or indeed any
 * "Popular / Strong Community" license approved by the Open Source Initiative
 * https://opensource.org/licenses/?categories=popular-strong-community
 *
 * Parse an 'ini' file calling a call-back for each value found.
 *
 * Syntax for ini file
 * - individual lines must not exceed 256 chars.  Longer lines are
 *   silently truncated.
 * - leading white space continues the previous line, this allowing large
 *   values.  The newline and leading white space are stripped.
 *   Each line is provided separately to the callback, so precise detail of
 *   how continuation lines are merged are left up to that callback.
 * - white space around "=", at EOL, and around section name in [section]
 *   is stripped
 * - Double quotes at both ends of a value are stripped.  This allows
 *   white space at either end of a value.
 * - If first non-white is '#', line is ignored.
 * - Everything after closing ']' of section is ignored
 * - If value is not quoted, everything after first '#' is ignored
 * - blank lines are ignored
 */
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#ifndef safe
/* "safe" pointers can never be NULL */
#define safe
#endif

typedef void (*ini_handle)(void *data, char *section safe,
			   char *name safe, char *value safe,
			   const char *path safe,
			   int append);

static inline void parse_ini(const char *path safe,
			     ini_handle handle, void *data)
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
