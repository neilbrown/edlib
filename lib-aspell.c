/*
 * Copyright Neil Brown ©2020 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * aspell: edlib interface for aspell library.
 *
 */

#include <aspell.h>
#include <wctype.h>
#include "core.h"

static AspellConfig *spell_config;

/* There should be one speller per doc... */
static AspellSpeller *speller;

static void make_speller(struct pane *p safe)
{
	AspellCanHaveError *ret = new_aspell_speller(spell_config);
	if (aspell_error_number(ret) != 0)
		call("Message", p, 0, NULL, aspell_error_message(ret));
	else
		speller = to_aspell_speller(ret);
}

static int trim(const char *safe *wordp safe)
{
	const char *wp = *wordp;
	const char *start;
	int len;
	wint_t ch;

	start = wp;
	while (*wp && (ch = get_utf8(&wp, NULL)) != WEOF &&
	       !iswalpha(ch))
		start = wp;
	if (!*start)
		return 0;
	len = 1;
	/* start is the first alphabetic, wp points beyond it */
	while (*wp && (ch = get_utf8(&wp, NULL)) != WEOF) {
		if (iswalpha(ch))
			len = wp - start;
	}
	*wordp = start;
	return len;
}

DEF_CMD(spell_check)
{
	int correct;
	const char *word = ci->str;
	int len;

	if (!word)
		return Enoarg;
	if (!speller)
		return Einval;
	len = trim(&word);
	if (!len)
		return Efail;

	correct = aspell_speller_check(speller, word, len);
	return correct ? 1 : Efalse;
}

DEF_CMD(spell_suggest)
{
	const AspellWordList *l;
	AspellStringEnumeration *el;
	const char *w;
	const char *word = ci->str;
	int len;

	if (!word)
		return Enoarg;
	if (!speller)
		return Einval;
	len = trim(&word);
	if (!len)
		return Efail;

	l = aspell_speller_suggest(speller, word, len);
	el = aspell_word_list_elements(l);
	while ((w = aspell_string_enumeration_next(el)) != NULL)
		comm_call(ci->comm2, "suggest", ci->focus, 0, NULL, w);
	delete_aspell_string_enumeration(el);
	return 1;
}

void edlib_init(struct pane *ed safe)
{
	spell_config = new_aspell_config();

	aspell_config_replace(spell_config, "lang", "en_AU");

	make_speller(ed);

	call_comm("global-set-command", ed, &spell_check,
		  0, NULL, "SpellCheck");
	call_comm("global-set-command", ed, &spell_suggest,
		  0, NULL, "SpellSuggest");
}
