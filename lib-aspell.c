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

static inline bool is_word_body(wint_t ch)
{
	/* alphabetics, appostrophies */
	return iswalpha(ch) || ch == '\'';
}

static inline bool is_word_initial(wint_t ch)
{
	/* Word must start with an alphabetic */
	return iswalpha(ch);
}

static inline bool is_word_final(wint_t ch)
{
	/* Word must end with an alphabetic */
	return iswalpha(ch);
}


DEF_CMD(spell_this)
{
	/* Find a word "here" to spell. It must include the first
	 * permitted character after '->mark'.
	 * '->mark' is moved to the end and if ->mark2 is available, it
	 * is moved to the start.
	 * If ->comm2 is available, the word is returned as a string.
	 * This command should ignore any view-specific or doc-specific rules
	 * about where words are allowed, but should honour any rules about
	 * what characters can constitute a word.
	 */
	wint_t ch;
	struct mark *m2;

	if (!ci->mark)
		return Enoarg;
	while ((ch = doc_next(ci->focus, ci->mark)) != WEOF &&
	       !is_word_initial(ch))
		;
	if (ch == WEOF)
		return Efalse;
	if (ci->mark2) {
		m2 = ci->mark2;
		mark_to_mark(m2, ci->mark);
	} else
		m2 = mark_dup(ci->mark);
	while ((ch = doc_following(ci->focus, ci->mark)) != WEOF &&
	       is_word_body(ch))
		doc_next(ci->focus, ci->mark);
	while ((ch = doc_prior(ci->focus, ci->mark)) != WEOF &&
	       !is_word_final(ch))
		doc_prev(ci->focus, ci->mark);

	while ((ch = doc_prior(ci->focus, m2)) != WEOF &&
	       is_word_body(ch))
		doc_prev(ci->focus, m2);
	while ((ch = doc_following(ci->focus, m2)) != WEOF &&
	       !is_word_initial(ch))
		doc_next(ci->focus, m2);
	if (ci->comm2)
		call_comm("doc:get-str", ci->focus, ci->comm2,
			  0, m2, NULL, 0, ci->mark);
	if (m2 != ci->mark2)
		mark_free(m2);
	return 1;
}

DEF_CMD(spell_next)
{
	/* Find the next word-start after ->mark.
	 * A view or doc might over-ride this to skip over
	 * content that shouldn't be spell-checked.
	 */
	wint_t ch;

	if (!ci->mark)
		return Enoarg;
	while ((ch = doc_next(ci->focus, ci->mark)) != WEOF &&
	       !is_word_initial(ch))
		;
	if (ch == WEOF)
		return Efalse;
	doc_prev(ci->focus, ci->mark);
	return 1;
}

void edlib_init(struct pane *ed safe)
{
	spell_config = new_aspell_config();

	aspell_config_replace(spell_config, "lang", "en_AU");

	make_speller(ed);

	call_comm("global-set-command", ed, &spell_check,
		  0, NULL, "Spell:Check");
	call_comm("global-set-command", ed, &spell_suggest,
		  0, NULL, "Spell:Suggest");
	call_comm("global-set-command", ed, &spell_this,
		  0, NULL, "Spell:ThisWord");
	call_comm("global-set-command", ed, &spell_next,
		  0, NULL, "Spell:NextWord");
}
