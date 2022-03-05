/*
 * Copyright Neil Brown ©2020-2021 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * aspell: edlib interface for aspell library.
 *
 */

#include <aspell.h>
#include <wctype.h>
#include "core.h"

static AspellConfig *spell_config;

struct aspell_data {
	AspellSpeller *speller safe;
	bool need_save;
};

static int trim(const char *safe *wordp safe)
{
	const char *wp = *wordp;
	const char *start;
	int len;
	wint_t ch;

	start = wp;
	while (*wp && (ch = get_utf8(&wp, NULL)) < WERR &&
	       !iswalpha(ch))
		start = wp;
	if (!*start)
		return 0;
	len = 1;
	/* start is the first alphabetic, wp points beyond it */
	while (*wp && (ch = get_utf8(&wp, NULL)) < WERR) {
		if (iswalpha(ch))
			len = wp - start;
	}
	*wordp = start;
	return len;
}

static struct map *aspell_map safe;
DEF_LOOKUP_CMD(aspell_handle, aspell_map);

DEF_CMD(aspell_attach_helper)
{
	struct aspell_data *as;
	AspellCanHaveError *ret;
	struct pane *p;

	ret = new_aspell_speller(spell_config);
	if (aspell_error_number(ret)) {
		LOG("Cannot create speller: %s", aspell_error_message(ret));
		return Efail;
	}
	alloc(as, pane);
	as->speller = safe_cast to_aspell_speller(ret);
	p = pane_register(ci->focus, 0, &aspell_handle.c, as);
	if (p) {
		call("doc:request:aspell:check", p);
		call("doc:request:aspell:suggest", p);
		call("doc:request:aspell:set-dict", p);
		call("doc:request:aspell:add-word", p);
		call("doc:request:aspell:save", p);
	}
	return 1;
}

DEF_CMD(aspell_close)
{
	struct aspell_data *as = ci->home->data;

	if (as->need_save)
		aspell_speller_save_all_word_lists(as->speller);
	delete_aspell_speller(as->speller);
	return 1;
}

DEF_CMD(aspell_check)
{
	struct aspell_data *as = ci->home->data;
	int correct;
	const char *word = ci->str;
	int len;

	if (!word)
		return Enoarg;
	len = trim(&word);
	if (!len)
		return Efail;

	correct = aspell_speller_check(as->speller, word, len);
	return correct ? 1 : Efalse;
}

DEF_CMD(spell_check)
{
	int rv = call("doc:notify:aspell:check", ci->focus, 0, NULL, ci->str);

	if (rv != Efallthrough)
		return rv;
	call_comm("doc:attach-helper", ci->focus, &aspell_attach_helper);
	return call("doc:notify:aspell:check", ci->focus, 0, NULL, ci->str);
}

DEF_CMD(aspell_suggest)
{
	struct aspell_data *as = ci->home->data;
	const AspellWordList *l;
	AspellStringEnumeration *el;
	const char *w;
	const char *word = ci->str;
	int len;

	if (!word)
		return Enoarg;
	len = trim(&word);
	if (!len)
		return Efail;

	l = aspell_speller_suggest(as->speller, word, len);
	el = aspell_word_list_elements(l);
	while ((w = aspell_string_enumeration_next(el)) != NULL)
		comm_call(ci->comm2, "suggest", ci->focus, 0, NULL, w);
	delete_aspell_string_enumeration(el);
	return 1;
}

DEF_CMD(spell_suggest)
{
	int rv = call_comm("doc:notify:aspell:suggest", ci->focus, ci->comm2,
			   0, NULL, ci->str);

	if (rv != Efallthrough)
		return rv;
	call_comm("doc:attach-helper", ci->focus, &aspell_attach_helper);
	return call_comm("doc:notify:aspell:suggest", ci->focus, ci->comm2,
			 0, NULL, ci->str);
}

DEF_CMD(aspell_save)
{
	struct aspell_data *as = ci->home->data;
	if (as->need_save) {
		as->need_save = False;
		aspell_speller_save_all_word_lists(as->speller);
	}
	return Efalse;
}

DEF_CMD(aspell_do_save)
{
	aspell_save_func(ci);
	return 1;
}

DEF_CMD(spell_save)
{
	return call_comm("doc:notify:aspell:save", ci->focus, ci->comm2,
			 ci->num, NULL, ci->str);
}

DEF_CMD(aspell_add)
{
	struct aspell_data *as = ci->home->data;
	const char *word = ci->str;
	int len;

	if (!word)
		return Enoarg;
	len = trim(&word);
	if (!len)
		return Efail;

	if (ci->num == 1) {
		aspell_speller_add_to_personal(as->speller, word, len);
		if (as->need_save)
			call_comm("event:free", ci->home, &aspell_save);
		as->need_save = True;
		call_comm("event:timer", ci->home, &aspell_save, 30*1000);
	} else
		aspell_speller_add_to_session(as->speller, word, len);
	call("doc:notify:spell:dict-changed", ci->home);
	return 1;
}

DEF_CMD(spell_add)
{
	int rv = call_comm("doc:notify:aspell:add-word", ci->focus, ci->comm2,
			   ci->num, NULL, ci->str);

	if (rv != Efallthrough)
		return rv;
	call_comm("doc:attach-helper", ci->focus, &aspell_attach_helper);
	return call_comm("doc:notify:aspell:add-word", ci->focus, ci->comm2,
			 ci->num, NULL, ci->str);
}

DEF_CMD(aspell_set_dict)
{
	struct aspell_data *as = ci->home->data;
	const char *lang = ci->str;
	AspellConfig *conf2;
	AspellCanHaveError *ret;

	if (!lang)
		return Enoarg;
	LOG("lang = %s", lang);
	conf2 = aspell_config_clone(spell_config);
	aspell_config_replace(conf2, "lang", lang);
	ret = new_aspell_speller(conf2);
	if (!aspell_error_number(ret)) {
		delete_aspell_speller(as->speller);
		as->speller = safe_cast to_aspell_speller(ret);
		call("doc:notify:spell:dict-changed", ci->focus);
	}
	delete_aspell_config(conf2);
	return 1;
}

DEF_CMD(spell_dict)
{
	int ret;
	ret = call("doc:notify:aspell:set-dict", ci->focus, 0, NULL,
		   ksuffix(ci, "interactive-cmd-dict-"));
	if (ret != Efallthrough)
		return ret;
	call_comm("doc:attach-helper", ci->focus, &aspell_attach_helper);
	return call("doc:notify:aspell:set-dict", ci->focus, 0, NULL,
		    ksuffix(ci, "interactive-cmd-dict-"));
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

	call_comm("global-set-command", ed, &spell_check,
		  0, NULL, "Spell:Check");
	call_comm("global-set-command", ed, &spell_suggest,
		  0, NULL, "Spell:Suggest");
	call_comm("global-set-command", ed, &spell_this,
		  0, NULL, "Spell:ThisWord");
	call_comm("global-set-command", ed, &spell_next,
		  0, NULL, "Spell:NextWord");
	call_comm("global-set-command", ed, &spell_add,
		  0, NULL, "Spell:AddWord");
	call_comm("global-set-command", ed, &spell_save,
		  0, NULL, "Spell:Save");

	call_comm("global-set-command", ed, &spell_dict,
		  0, NULL, "interactive-cmd-dict-",
		  0, NULL, "interactive-cmd-dict-~");

	aspell_map = key_alloc();
	key_add(aspell_map, "Close", &aspell_close);
	key_add(aspell_map, "Free", &edlib_do_free);
	key_add(aspell_map, "aspell:check", &aspell_check);
	key_add(aspell_map, "aspell:suggest", &aspell_suggest);
	key_add(aspell_map, "aspell:set-dict", &aspell_set_dict);
	key_add(aspell_map, "aspell:add-word", &aspell_add);
	key_add(aspell_map, "aspell:save", &aspell_do_save);
}
