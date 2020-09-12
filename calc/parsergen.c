#line 25 "parsergen.mdc"
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#line 72 "parsergen.mdc"
#include "mdcode.h"
#include "scanner.h"
#line 194 "parsergen.mdc"
#include <string.h>
#line 448 "parsergen.mdc"
#include <memory.h>
#line 2246 "parsergen.mdc"
#include <getopt.h>
#line 2324 "parsergen.mdc"
#include <fcntl.h>
#include <sys/mman.h>
#include <errno.h>
#line 2455 "parsergen.mdc"
#include <locale.h>
#line 155 "parsergen.mdc"
enum symtype { Unknown, Virtual, Terminal, Nonterminal };
char *symtypes = "UVTN";
#line 1087 "parsergen.mdc"
enum grammar_type { LR0, LR05, SLR, LALR, LR1 };
#line 90 "parsergen.mdc"
enum assoc {Left, Right, Non};
char *assoc_names[] = {"Left","Right","Non"};

struct symbol {
	struct text struct_name;
	int isref;
	enum assoc assoc;
	unsigned short precedence;
#line 158 "parsergen.mdc"
	enum symtype type;
#line 179 "parsergen.mdc"
	short num;
#line 197 "parsergen.mdc"
	struct text name;
	struct symbol *next;
#line 462 "parsergen.mdc"
	int first_production;
#line 861 "parsergen.mdc"
	int nullable;
#line 907 "parsergen.mdc"
	int line_like;
#line 99 "parsergen.mdc"
};
struct production {
	unsigned short precedence;
	enum assoc assoc;
	char line_like;
#line 455 "parsergen.mdc"
	struct symbol  *head;
	struct symbol **body;
	int             body_size;
	struct text     code;
	int             code_line;
#line 105 "parsergen.mdc"
};
struct grammar {
#line 201 "parsergen.mdc"
	struct symbol *syms;
	int num_syms;
#line 285 "parsergen.mdc"
	struct text current_type;
	int type_isref;
	int prec_levels;
#line 451 "parsergen.mdc"
	struct production **productions;
	int production_count;
#line 797 "parsergen.mdc"
	struct setlist *sets;
	int nextset;
#line 953 "parsergen.mdc"
	struct symset *first;
#line 1063 "parsergen.mdc"
	struct symset *follow;
#line 1202 "parsergen.mdc"
	struct itemset *items;
	int states;
#line 1505 "parsergen.mdc"
	struct symbol **symtab;
	struct itemset **statetab;
	int first_nonterm;
#line 108 "parsergen.mdc"
};
#line 166 "parsergen.mdc"
static char *reserved_words[] = {
	[TK_error]        = "ERROR",
	[TK_number]       = "NUMBER",
	[TK_ident]        = "IDENTIFIER",
	[TK_mark]         = "MARK",
	[TK_string]       = "STRING",
	[TK_multi_string] = "MULTI_STRING",
	[TK_in]           = "IN",
	[TK_out]          = "OUT",
	[TK_newline]      = "NEWLINE",
	[TK_eof]          = "$eof",
};
#line 290 "parsergen.mdc"
enum symbols { TK_virtual = TK_reserved, TK_open, TK_close };
static const char *known[] = { "$$", "${", "}$" };
#line 683 "parsergen.mdc"
struct symset {
	short cnt;
	unsigned short *syms, *data;
};
#define NO_DATA ((unsigned short *)1)
const struct symset INIT_SYMSET =  { 0, NULL, NO_DATA };
const struct symset INIT_DATASET = { 0, NULL, NULL };
#line 790 "parsergen.mdc"
struct setlist {
	struct symset ss;
	int num;
	struct setlist *next;
};
#line 1110 "parsergen.mdc"
static inline unsigned short item_num(int production, int index)
{
	return production | ((31-index) << 11);
}
static inline int item_prod(unsigned short item)
{
	return item & 0x7ff;
}
static inline int item_index(unsigned short item)
{
	return (31-(item >> 11)) & 0x1f;
}
#line 1189 "parsergen.mdc"
struct itemset {
	struct itemset *next;
	short state;
	struct symset items;
	struct symset go_to;
	enum assoc assoc;
	unsigned short precedence;
	char completed;
	char starts_line;
	int min_prefix;
};
#line 2249 "parsergen.mdc"
static const struct option long_options[] = {
	{ "LR0",	0, NULL, '0' },
	{ "LR05",	0, NULL, '5' },
	{ "SLR",	0, NULL, 'S' },
	{ "LALR",	0, NULL, 'L' },
	{ "LR1",	0, NULL, '1' },
	{ "tag",	1, NULL, 't' },
	{ "report",	0, NULL, 'R' },
	{ "output",	1, NULL, 'o' },
	{ NULL,		0, NULL, 0   }
};
const char *options = "05SL1t:Ro:";
#line 120 "parsergen.mdc"
static int text_is(struct text t, char *s)
{
	return (strlen(s) == t.len &&
		strncmp(s, t.txt, t.len) == 0);
}
static void prtxt(struct text t)
{
	printf("%.*s", t.len, t.txt);
}

static int strip_tag(struct text *t, char *tag)
{
	int skip = strlen(tag) + 1;
	if (skip >= t->len ||
	    strncmp(t->txt, tag, skip-1) != 0 ||
	    t->txt[skip-1] != ':')
		return 0;
	while (skip < t->len && t->txt[skip] == ' ')
		skip++;
	t->len -= skip;
	t->txt += skip;
	return 1;
}
#line 205 "parsergen.mdc"
static struct symbol *sym_find(struct grammar *g, struct text s)
{
	struct symbol **l = &g->syms;
	struct symbol *n;
	int cmp = 1;

	while (*l &&
		(cmp = text_cmp((*l)->name, s)) < 0)
			l = & (*l)->next;
	if (cmp == 0)
		return *l;
	n = calloc(1, sizeof(*n));
	n->name = s;
	n->type = Unknown;
	n->next = *l;
	n->num = -1;
	*l = n;
	return n;
}

static void symbols_init(struct grammar *g)
{
	int entries = sizeof(reserved_words)/sizeof(reserved_words[0]);
	int i;
	for (i = 0; i < entries; i++) {
		struct text t;
		struct symbol *s;
		t.txt = reserved_words[i];
		if (!t.txt)
			continue;
		t.len = strlen(t.txt);
		s = sym_find(g, t);
		s->type = Terminal;
		s->num = i;
	}
}
#line 294 "parsergen.mdc"
static char *dollar_line(struct token_state *ts, struct grammar *g, int isref)
{
	struct token t = token_next(ts);
	char *err;
	enum assoc assoc;
	int found;

	if (t.num != TK_ident) {
		err = "type or assoc expected after '$'";
		goto abort;
	}
	if (text_is(t.txt, "LEFT"))
		assoc = Left;
	else if (text_is(t.txt, "RIGHT"))
		assoc = Right;
	else if (text_is(t.txt, "NON"))
		assoc = Non;
	else {
		g->current_type = t.txt;
		g->type_isref = isref;
		if (text_is(t.txt, "void"))
			g->current_type.txt = NULL;
		t = token_next(ts);
		if (t.num != TK_newline) {
			err = "Extra tokens after type name";
			goto abort;
		}
		return NULL;
	}

	if (isref) {
		err = "$* cannot be followed by a precedence";
		goto abort;
	}

	// This is a precedence line, need some symbols.
	found = 0;
	g->prec_levels += 1;
	t = token_next(ts);
	while (t.num != TK_newline) {
		enum symtype type = Terminal;
		struct symbol *s;
		if (t.num == TK_virtual) {
			type = Virtual;
			t = token_next(ts);
			if (t.num != TK_ident) {
				err = "$$ must be followed by a word";
				goto abort;
			}
		} else if (t.num != TK_ident &&
		           t.num != TK_mark) {
			err = "Illegal token in precedence line";
			goto abort;
		}
		s = sym_find(g, t.txt);
		if (s->type != Unknown) {
			err = "Symbols in precedence line must not already be known.";
			goto abort;
		}
		s->type = type;
		s->precedence = g->prec_levels;
		s->assoc = assoc;
		found += 1;
		t = token_next(ts);
	}
	if (found == 0)
		err = "No symbols given on precedence line";
		goto abort;
	return NULL;
abort:
	while (t.num != TK_newline && t.num != TK_eof)
		t = token_next(ts);
	return err;
}
#line 407 "parsergen.mdc"
static void array_add(void *varray, int *cnt, void *new)
{
	void ***array = varray;
	int current = 0;
	const int step = 8;
	current = ((*cnt-1) | (step-1))+1;
	if (*cnt == current) {
		/* must grow */
		current += step;
		*array = realloc(*array, current * sizeof(void*));
	}
	(*array)[*cnt] = new;
	(*cnt) += 1;
}
#line 427 "parsergen.mdc"
static struct text collect_code(struct token_state *state,
                                struct token start)
{
	struct text code;
	struct token t;
	code.txt = start.txt.txt + start.txt.len;
	do
		t = token_next(state);
	while (t.node == start.node &&
	       t.num != TK_close && t.num != TK_error &&
	       t.num != TK_eof);
	if (t.num == TK_close && t.node == start.node)
		code.len = t.txt.txt - code.txt;
	else
		code.txt = NULL;
	return code;
}
#line 465 "parsergen.mdc"
static char *parse_production(struct grammar *g,
                              struct symbol *head,
                              struct token_state *state)
{
	/* Head has already been parsed. */
	struct token tk;
	char *err;
	struct production p, *pp;

	memset(&p, 0, sizeof(p));
	p.head = head;
	tk = token_next(state);
	while (tk.num == TK_ident || tk.num == TK_mark) {
		struct symbol *bs = sym_find(g, tk.txt);
		if (bs->type == Unknown)
			bs->type = Terminal;
		if (bs->type == Virtual) {
			err = "Virtual symbol not permitted in production";
			goto abort;
		}
		if (bs->precedence) {
			p.precedence = bs->precedence;
			p.assoc = bs->assoc;
		}
		array_add(&p.body, &p.body_size, bs);
		tk = token_next(state);
	}
	if (tk.num == TK_virtual) {
		struct symbol *vs;
		tk = token_next(state);
		if (tk.num != TK_ident) {
			err = "word required after $$";
			goto abort;
		}
		vs = sym_find(g, tk.txt);
		if (vs->num == TK_newline)
			p.line_like = 1;
		else if (vs->num == TK_out)
			p.line_like = 2;
		else if (vs->precedence == 0) {
			err = "symbol after $$ must have precedence";
			goto abort;
		} else {
			p.precedence = vs->precedence;
			p.assoc = vs->assoc;
		}
		tk = token_next(state);
	}
	if (tk.num == TK_open) {
		p.code_line = tk.line;
		p.code = collect_code(state, tk);
		if (p.code.txt == NULL) {
			err = "code fragment not closed properly";
			goto abort;
		}
		tk = token_next(state);
	}
	if (tk.num != TK_newline && tk.num != TK_eof) {
		err = "stray tokens at end of line";
		goto abort;
	}
	pp = malloc(sizeof(*pp));
	*pp = p;
	array_add(&g->productions, &g->production_count, pp);
	return NULL;
abort:
	while (tk.num != TK_newline && tk.num != TK_eof)
		tk = token_next(state);
	return err;
}
#line 697 "parsergen.mdc"
static void symset_add(struct symset *s, unsigned short key, unsigned short val)
{
	int i;
	int current = ((s->cnt-1) | 7) + 1;
	if (current == s->cnt) {
		current += 8;
		s->syms = realloc(s->syms, sizeof(*s->syms) * current);
		if (s->data != NO_DATA)
			s->data = realloc(s->data, sizeof(*s->data) * current);
	}
	i = s->cnt;
	while (i > 0 && s->syms[i-1] > key) {
		s->syms[i] = s->syms[i-1];
		if (s->data != NO_DATA)
			s->data[i] = s->data[i-1];
		i--;
	}
	s->syms[i] = key;
	if (s->data != NO_DATA)
		s->data[i] = val;
	s->cnt += 1;
}
#line 724 "parsergen.mdc"
static int symset_find(struct symset *ss, unsigned short key)
{
	int lo = 0;
	int hi = ss->cnt;

	if (hi == 0)
		return -1;
	while (lo + 1 < hi) {
		int mid = (lo + hi) / 2;
		if (ss->syms[mid] <= key)
			lo = mid;
		else
			hi = mid;
	}
	if (ss->syms[lo] == key)
		return lo;
	return -1;
}
#line 750 "parsergen.mdc"
static int symset_union(struct symset *a, struct symset *b)
{
	int i;
	int added = 0;
	for (i = 0; i < b->cnt; i++)
		if (symset_find(a, b->syms[i]) < 0) {
			unsigned short data = 0;
			if (b->data != NO_DATA)
				data = b->data[i];
			symset_add(a, b->syms[i], data);
			added++;
		}
	return added;
}
#line 767 "parsergen.mdc"
static void symset_free(struct symset ss)
{
	free(ss.syms);
	if (ss.data != NO_DATA)
		free(ss.data);
}
#line 802 "parsergen.mdc"
static int ss_cmp(struct symset a, struct symset b)
{
	int i;
	int diff = a.cnt - b.cnt;
	if (diff)
		return diff;
	for (i = 0; i < a.cnt; i++) {
		diff = (int)a.syms[i] - (int)b.syms[i];
		if (diff)
			return diff;
	}
	return 0;
}

static int save_set(struct grammar *g, struct symset ss)
{
	struct setlist **sl = &g->sets;
	int cmp = 1;
	struct setlist *s;

	while (*sl && (cmp = ss_cmp((*sl)->ss, ss)) < 0)
		sl = & (*sl)->next;
	if (cmp == 0) {
		symset_free(ss);
		return (*sl)->num;
	}

	s = malloc(sizeof(*s));
	s->ss = ss;
	s->num = g->nextset;
	g->nextset += 1;
	s->next = *sl;
	*sl = s;
	return s->num;
}
#line 842 "parsergen.mdc"
static struct symset set_find(struct grammar *g, int num)
{
	struct setlist *sl = g->sets;
	while (sl && sl->num != num)
		sl = sl->next;
	return sl->ss;
}
#line 864 "parsergen.mdc"
static void set_nullable(struct grammar *g)
{
	int check_again = 1;
	while (check_again) {
		int p;
		check_again = 0;
		for (p = 0; p < g->production_count; p++) {
			struct production *pr = g->productions[p];
			int s;

			if (pr->head->nullable)
				continue;
			for (s = 0; s < pr->body_size; s++)
				if (! pr->body[s]->nullable)
					break;
			if (s == pr->body_size) {
				pr->head->nullable = 1;
				check_again = 1;
			}
		}
	}
}
#line 910 "parsergen.mdc"
static void set_line_like(struct grammar *g)
{
	int check_again = 1;
	g->symtab[TK_newline]->line_like = 1;
	while (check_again) {
		int p;
		check_again = 0;
		for (p = 0; p < g->production_count; p++) {
			struct production *pr = g->productions[p];
			int s;

			if (pr->head->line_like)
				continue;

			for (s = 0 ; s < pr->body_size; s++) {
				if (pr->body[s]->line_like) {
					pr->head->line_like = 1;
					check_again = 1;
					break;
				}
			}
		}
	}
}
#line 957 "parsergen.mdc"
static int add_first(struct production *pr, int start,
                     struct symset *target, struct grammar *g,
                     int *to_end)
{
	int s;
	int changed = 0;
	for (s = start; s < pr->body_size; s++) {
		struct symbol *bs = pr->body[s];
		if (bs->type == Terminal) {
			if (symset_find(target, bs->num) < 0) {
				symset_add(target, bs->num, 0);
				changed = 1;
			}
			break;
		} else if (symset_union(target, &g->first[bs->num]))
			changed = 1;
		if (!bs->nullable)
			break;
	}
	if (to_end)
		*to_end = (s == pr->body_size);
	return changed;
}

static void build_first(struct grammar *g)
{
	int check_again = 1;
	int s;
	g->first = calloc(g->num_syms, sizeof(g->first[0]));
	for (s = 0; s < g->num_syms; s++)
		g->first[s] = INIT_SYMSET;

	while (check_again) {
		int p;
		check_again = 0;
		for (p = 0; p < g->production_count; p++) {
			struct production *pr = g->productions[p];
			struct symset *head = &g->first[pr->head->num];

			if (add_first(pr, 0, head, g, NULL))
				check_again = 1;
		}
	}
}
#line 1066 "parsergen.mdc"
static void build_follow(struct grammar *g)
{
	int s, again, p;
	g->follow = calloc(g->num_syms, sizeof(g->follow[0]));
	for (s = 0; s < g->num_syms; s++)
		g->follow[s] = INIT_SYMSET;
#line 1019 "parsergen.mdc"
	for (p = 0; p < g->production_count; p++) {
		struct production *pr = g->productions[p];
		int b;
	
		for (b = 0; b < pr->body_size - 1; b++) {
			struct symbol *bs = pr->body[b];
			if (bs->type == Terminal)
				continue;
			add_first(pr, b+1, &g->follow[bs->num], g, NULL);
		}
	}
#line 1039 "parsergen.mdc"
	for (again = 0, p = 0;
	     p < g->production_count;
	     p < g->production_count-1
	        ? p++ : again ? (again = 0, p = 0)
			      : p++) {
		struct production *pr = g->productions[p];
		int b;
	
		for (b = pr->body_size - 1; b >= 0; b--) {
			struct symbol *bs = pr->body[b];
			if (bs->type == Terminal)
				break;
			if (symset_union(&g->follow[bs->num],
			                 &g->follow[pr->head->num]))
				again = 1;
			if (!bs->nullable)
				break;
		}
	}
#line 1073 "parsergen.mdc"
}
#line 1128 "parsergen.mdc"
static int itemset_cmp(struct symset a, struct symset b,
                       enum grammar_type type)
{
	int i;
	int av, bv;

	for (i = 0;
	     i < a.cnt && i < b.cnt &&
	     item_index(a.syms[i]) > 0 &&
	     item_index(b.syms[i]) > 0;
	     i++) {
		int diff = a.syms[i] - b.syms[i];
		if (diff)
			return diff;
		if (type == LR1) {
			diff = a.data[i] - b.data[i];
			if (diff)
				return diff;
		}
	}
	if (i == a.cnt || item_index(a.syms[i]) == 0)
		av = -1;
	else
		av = a.syms[i];
	if (i == b.cnt || item_index(b.syms[i]) == 0)
		bv = -1;
	else
		bv = b.syms[i];
	if (av - bv)
		return av - bv;
	if (type < LR1 || av == -1)
		return 0;
	return
		a.data[i] - b.data[i];
}
#line 1206 "parsergen.mdc"
static int itemset_find(struct grammar *g, struct itemset ***where,
                        struct symset kernel, enum grammar_type type)
{
	struct itemset **ip;

	for (ip = &g->items; *ip ; ip = & (*ip)->next) {
		struct itemset *i = *ip;
		int diff;
		diff = itemset_cmp(i->items, kernel, type);
		if (diff < 0)
			continue;
		if (diff > 0)
			break;
		/* found */
		*where = ip;
		return 1;
	}
	*where = ip;
	return 0;
}
#line 1235 "parsergen.mdc"
static int add_itemset(struct grammar *g, struct symset ss,
                       enum assoc assoc, unsigned short precedence,
                       enum grammar_type type)
{
	struct itemset **where, *is;
	int i;
	int found = itemset_find(g, &where, ss, type);
	if (!found) {
		is = calloc(1, sizeof(*is));
		is->state = g->states;
		g->states += 1;
		is->items = ss;
		is->assoc = assoc;
		is->precedence = precedence;
		is->next = *where;
		is->go_to = INIT_DATASET;
		*where = is;
		return is->state;
	}
	is = *where;
	if (type != LALR) {
		symset_free(ss);
		return is->state;
	}
	for (i = 0; i < ss.cnt; i++) {
		struct symset temp = INIT_SYMSET, s;
		if (ss.data[i] == is->items.data[i])
			continue;
		s = set_find(g, is->items.data[i]);
		symset_union(&temp, &s);
		s = set_find(g, ss.data[i]);
		if (symset_union(&temp, &s)) {
			is->items.data[i] = save_set(g, temp);
			is->completed = 0;
		} else
			symset_free(temp);
	}
	symset_free(ss);
	return is->state;
}
#line 1458 "parsergen.mdc"
static void build_itemsets(struct grammar *g, enum grammar_type type)
{
	struct symset first = INIT_SYMSET;
	struct itemset *is;
	int again;
	unsigned short la = 0;
	if (type >= LALR) {
		// LA set just has eof
		struct symset eof = INIT_SYMSET;
		symset_add(&eof, TK_eof, 0);
		la = save_set(g, eof);
		first = INIT_DATASET;
	}
	// production 0, offset 0 (with no data)
	symset_add(&first, item_num(0, 0), la);
	add_itemset(g, first, Non, 0, type);
	for (again = 0, is = g->items;
	     is;
	     is = is->next ?: again ? (again = 0, g->items) : NULL) {
		int i;
		struct symset done = INIT_SYMSET;
		if (is->completed)
			continue;
		is->completed = 1;
		again = 1;
#line 1306 "parsergen.mdc"
	        for (i = 0; i < is->items.cnt; i++) {
	        	int p = item_prod(is->items.syms[i]);
	        	int bs = item_index(is->items.syms[i]);
	        	struct production *pr = g->productions[p];
	        	int p2;
	        	struct symbol *s;
	        	struct symset LA = INIT_SYMSET;
	        	unsigned short sn = 0;
	        	struct symset LAnl = INIT_SYMSET;
	        	unsigned short snnl = 0;
	        
	        	if (is->min_prefix == 0 ||
	        	    (bs > 0 && bs < is->min_prefix))
	        		is->min_prefix = bs;
	        	if (bs == pr->body_size)
	        		continue;
	        	s = pr->body[bs];
	        	if (s->precedence && is->precedence &&
	        	    is->precedence > s->precedence)
	        		/* This terminal has a low precedence and
	        		 * shouldn't be shifted
	        		 */
	        		continue;
	        	if (s->precedence && is->precedence &&
	        	    is->precedence == s->precedence && s->assoc != Right)
	        		/* This terminal has a matching precedence and is
	        		 * not Right-associative, so we mustn't shift it.
	        		 */
	        		continue;
	        	if (symset_find(&done, s->num) < 0) {
	        		symset_add(&done, s->num, 0);
	        	}
	        	if (s->type != Nonterminal)
	        		continue;
	        	if (s->line_like)
	        		is->starts_line = 1;
	        	again = 1;
	        	if (type >= LALR) {
	        		// Need the LA set.
	        		int to_end;
	        		add_first(pr, bs+1, &LA, g, &to_end);
	        		if (to_end) {
	        			struct symset ss = set_find(g, is->items.data[i]);
	        			symset_union(&LA, &ss);
	        		}
	        		sn = save_set(g, LA);
	        		LA = set_find(g, sn);
	        		if (symset_find(&LA, TK_newline))
	        			symset_add(&LAnl, TK_newline, 0);
	        		snnl = save_set(g, LAnl);
	        		LAnl = set_find(g, snnl);
	        	}
	        
	        	/* Add productions for this symbol */
	        	for (p2 = s->first_production;
	        	     p2 < g->production_count &&
	        	      g->productions[p2]->head == s;
	        	     p2++) {
	        		int itm = item_num(p2, 0);
	        		int pos = symset_find(&is->items, itm);
	        		if (pos < 0) {
	        			if (g->productions[p2]->line_like)
	        				symset_add(&is->items, itm, snnl);
	        			else
	        				symset_add(&is->items, itm, sn);
	        			/* Will have re-ordered, so start
	        			 * from beginning again */
	        			i = -1;
	        		} else if (type >= LALR) {
	        			struct symset ss = set_find(g, is->items.data[pos]);
	        			struct symset tmp = INIT_SYMSET;
	        			struct symset *la = &LA;
	        
	        			if (g->productions[p2]->line_like)
	        				la = &LAnl;
	        			symset_union(&tmp, &ss);
	        			if (symset_union(&tmp, la)) {
	        				is->items.data[pos] = save_set(g, tmp);
	        				i = -1;
	        			} else
	        				symset_free(tmp);
	        		}
	        	}
	        }
#line 1397 "parsergen.mdc"
	        // Now we have a completed itemset, so we need to
	        // compute all the 'next' itemsets and create them
	        // if they don't exist.
	        for (i = 0; i < done.cnt; i++) {
	        	int j;
	        	unsigned short state;
	        	struct symbol *sym = g->symtab[done.syms[i]];
	        	enum assoc assoc = Non;
	        	unsigned short precedence = 0;
	        	struct symset newitemset = INIT_SYMSET;
	        	if (type >= LALR)
	        		newitemset = INIT_DATASET;
	        
	        	for (j = 0; j < is->items.cnt; j++) {
	        		int itm = is->items.syms[j];
	        		int p = item_prod(itm);
	        		int bp = item_index(itm);
	        		struct production *pr = g->productions[p];
	        		unsigned short la = 0;
	        		int pos;
	        
	        		if (bp == pr->body_size)
	        			continue;
	        		if (pr->body[bp] != sym)
	        			continue;
	        		if (type >= LALR)
	        			la = is->items.data[j];
	        		pos = symset_find(&newitemset, pr->head->num);
	        		if (bp + 1 == pr->body_size &&
	        		    pr->precedence > 0 &&
	        		    pr->precedence > precedence) {
	        			// new itemset is reducible and has a precedence.
	        			precedence = pr->precedence;
	        			assoc = pr->assoc;
	        		}
	        		if (pos < 0)
	        			symset_add(&newitemset, item_num(p, bp+1), la);
	        		else if (type >= LALR) {
	        			// Need to merge la set.
	        			int la2 = newitemset.data[pos];
	        			if (la != la2) {
	        				struct symset ss = set_find(g, la2);
	        				struct symset LA = INIT_SYMSET;
	        				symset_union(&LA, &ss);
	        				ss = set_find(g, la);
	        				if (symset_union(&LA, &ss))
	        					newitemset.data[pos] = save_set(g, LA);
	        				else
	        					symset_free(LA);
	        			}
	        		}
	        	}
	        	state = add_itemset(g, newitemset, assoc, precedence, type);
	        	if (symset_find(&is->go_to, done.syms[i]) < 0)
	        		symset_add(&is->go_to, done.syms[i], state);
	        }
#line 1485 "parsergen.mdc"
		symset_free(done);
	}
}
#line 1570 "parsergen.mdc"
static void report_symbols(struct grammar *g)
{
	int n;
	if (g->first)
		printf("SYMBOLS + FIRST:\n");
	else
		printf("SYMBOLS:\n");

	for (n = 0; n < g->num_syms; n++) {
		struct symbol *s = g->symtab[n];
		if (!s)
			continue;

		printf(" %c%c%3d%c: ",
		       s->nullable ? '.':' ',
		       s->line_like ? '<':' ',
		       s->num, symtypes[s->type]);
		prtxt(s->name);
		if (s->precedence)
			printf(" (%d%s)", s->precedence,
			       assoc_names[s->assoc]);

		if (g->first && s->type == Nonterminal) {
			int j;
			char c = ':';
			for (j = 0; j < g->first[n].cnt; j++) {
				printf("%c ", c);
				c = ',';
				prtxt(g->symtab[g->first[n].syms[j]]->name);
			}
		}
		printf("\n");
	}
}
#line 1607 "parsergen.mdc"
static void report_follow(struct grammar *g)
{
	int n;
	printf("FOLLOW:\n");
	for (n = 0; n < g->num_syms; n++)
		if (g->follow[n].cnt) {
			int j;
			char c = ':';
			printf("  ");
			prtxt(g->symtab[n]->name);
			for (j = 0; j < g->follow[n].cnt; j++) {
				printf("%c ", c);
				c = ',';
				prtxt(g->symtab[g->follow[n].syms[j]]->name);
			}
			printf("\n");
		}
}
#line 1630 "parsergen.mdc"
static void report_item(struct grammar *g, int itm)
{
	int p = item_prod(itm);
	int dot = item_index(itm);
	struct production *pr = g->productions[p];
	int i;

	printf("    ");
	prtxt(pr->head->name);
	printf(" ->");
	for (i = 0; i < pr->body_size; i++) {
		printf(" %s", (dot == i ? ". ": ""));
		prtxt(pr->body[i]->name);
	}
	if (dot == pr->body_size)
		printf(" .");
	printf(" [%d]", p);
	if (pr->precedence && dot == pr->body_size)
		printf(" (%d%s)", pr->precedence,
		       assoc_names[pr->assoc]);
	if (dot < pr->body_size &&
	    pr->body[dot]->precedence) {
		struct symbol *s = pr->body[dot];
		printf(" [%d%s]", s->precedence,
		       assoc_names[s->assoc]);
	}
	if (pr->line_like == 1)
		printf(" $$NEWLINE");
	else if (pr->line_like)
		printf(" $$OUT");
	printf("\n");
}
#line 1665 "parsergen.mdc"
static void report_la(struct grammar *g, int lanum)
{
	struct symset la = set_find(g, lanum);
	int i;
	char c = ':';

	printf("        LOOK AHEAD(%d)", lanum);
	for (i = 0; i < la.cnt; i++) {
		printf("%c ", c);
		c = ',';
		prtxt(g->symtab[la.syms[i]]->name);
	}
	printf("\n");
}
#line 1682 "parsergen.mdc"
static void report_goto(struct grammar *g, struct symset gt)
{
	int i;
	printf("    GOTO:\n");

	for (i = 0; i < gt.cnt; i++) {
		printf("      ");
		prtxt(g->symtab[gt.syms[i]]->name);
		printf(" -> %d\n", gt.data[i]);
	}
}
#line 1696 "parsergen.mdc"
static void report_itemsets(struct grammar *g)
{
	int s;
	printf("ITEM SETS(%d)\n", g->states);
	for (s = 0; s < g->states; s++) {
		int j;
		struct itemset *is = g->statetab[s];
		printf("  Itemset %d:%s min prefix=%d",
		       s, is->starts_line?" (startsline)":"", is->min_prefix);
		if (is->precedence)
			printf(" %d%s", is->precedence, assoc_names[is->assoc]);
		printf("\n");
		for (j = 0; j < is->items.cnt; j++) {
			report_item(g, is->items.syms[j]);
			if (is->items.data != NO_DATA)
				report_la(g, is->items.data[j]);
		}
		report_goto(g, is->go_to);
	}
}
#line 1747 "parsergen.mdc"
static int conflicts_lr0(struct grammar *g, enum grammar_type type)
{
	int i;
	int cnt = 0;
	for (i = 0; i < g->states; i++) {
		struct itemset *is = g->statetab[i];
		int last_reduce = -1;
		int prev_reduce = -1;
		int last_shift = -1;
		int j;
		if (!is)
			continue;
		for (j = 0; j < is->items.cnt; j++) {
			int itm = is->items.syms[j];
			int p = item_prod(itm);
			int bp = item_index(itm);
			struct production *pr = g->productions[p];

			if (bp == pr->body_size) {
				prev_reduce = last_reduce;
				last_reduce = j;
				continue;
			}
			if (pr->body[bp]->type == Terminal)
				last_shift = j;
		}
		if (type == LR0 && last_reduce >= 0 && last_shift >= 0) {
			printf("  State %d has both SHIFT and REDUCE:\n", i);
			report_item(g, is->items.syms[last_shift]);
			report_item(g, is->items.syms[last_reduce]);
			cnt++;
		}
		if (prev_reduce >= 0) {
			printf("  State %d has 2 (or more) reducible items\n", i);
			report_item(g, is->items.syms[prev_reduce]);
			report_item(g, is->items.syms[last_reduce]);
			cnt++;
		}
	}
	return cnt;
}
#line 1803 "parsergen.mdc"
static int conflicts_slr(struct grammar *g, enum grammar_type type)
{
	int i;
	int cnt = 0;

	for (i = 0; i < g->states; i++) {
		struct itemset *is = g->statetab[i];
		struct symset shifts = INIT_DATASET;
		struct symset reduce = INIT_DATASET;
		int j;
		if (!is)
			continue;
		/* First collect the shifts */
		for (j = 0; j < is->items.cnt; j++) {
			unsigned short itm = is->items.syms[j];
			int p = item_prod(itm);
			int bp = item_index(itm);
			struct production *pr = g->productions[p];
			struct symbol *s;

			if (bp >= pr->body_size ||
			    pr->body[bp]->type != Terminal)
				/* not shiftable */
				continue;

			s = pr->body[bp];
			if (s->precedence && is->precedence)
				/* Precedence resolves this, so no conflict */
				continue;

			if (symset_find(&shifts, s->num) < 0)
				symset_add(&shifts, s->num, itm);
		}
		/* Now look for reductions and conflicts */
		for (j = 0; j < is->items.cnt; j++) {
			unsigned short itm = is->items.syms[j];
			int p = item_prod(itm);
			int bp = item_index(itm);
			struct production *pr = g->productions[p];

			if (bp < pr->body_size)
				continue;
			/* reducible */
			struct symset la;
			if (type == SLR)
				la = g->follow[pr->head->num];
			else
				la = set_find(g, is->items.data[j]);
			int k;
			for (k = 0; k < la.cnt; k++) {
				int pos = symset_find(&shifts, la.syms[k]);
				if (pos >= 0 && la.syms[k] != TK_newline) {
					printf("  State %d has SHIFT/REDUCE conflict on ", i);
					cnt++;
						prtxt(g->symtab[la.syms[k]]->name);
					printf(":\n");
					report_item(g, shifts.data[pos]);
					report_item(g, itm);
				}
				pos = symset_find(&reduce, la.syms[k]);
				if (pos < 0) {
					symset_add(&reduce, la.syms[k], itm);
					continue;
				}
				printf("  State %d has REDUCE/REDUCE conflict on ", i);
				prtxt(g->symtab[la.syms[k]]->name);
				printf(":\n");
				report_item(g, itm);
				report_item(g, reduce.data[pos]);
				cnt++;
			}
		}
		symset_free(shifts);
		symset_free(reduce);
	}
	return cnt;
}
#line 1726 "parsergen.mdc"

static int report_conflicts(struct grammar *g, enum grammar_type type)
{
	int cnt = 0;
	printf("Conflicts:\n");
	if (type < SLR)
		cnt = conflicts_lr0(g, type);
	else
		cnt = conflicts_slr(g, type);
	if (cnt == 0)
		printf(" - no conflicts\n");
	return cnt;
}
#line 1931 "parsergen.mdc"
static void gen_known(FILE *f, struct grammar *g)
{
	int i;
	fprintf(f, "#line 0 \"gen_known\"\n");
	fprintf(f, "static const char *known[] = {\n");
	for (i = TK_reserved;
	     i < g->num_syms && g->symtab[i]->type == Terminal;
	     i++)
		fprintf(f, "\t\"%.*s\",\n", g->symtab[i]->name.len,
			g->symtab[i]->name.txt);
	fprintf(f, "};\n\n");
}

static void gen_non_term(FILE *f, struct grammar *g)
{
	int i;
	fprintf(f, "#line 0 \"gen_non_term\"\n");
	fprintf(f, "static const char *non_term[] = {\n");
	for (i = TK_reserved;
	     i < g->num_syms;
	     i++)
		if (g->symtab[i]->type != Terminal)
			fprintf(f, "\t\"%.*s\",\n", g->symtab[i]->name.len,
				g->symtab[i]->name.txt);
	fprintf(f, "};\n\n");
}
#line 1989 "parsergen.mdc"
static void gen_goto(FILE *f, struct grammar *g)
{
	int i;
	fprintf(f, "#line 0 \"gen_goto\"\n");
	for (i = 0; i < g->states; i++) {
		int j;
		fprintf(f, "static const struct lookup goto_%d[] = {\n",
			i);
		struct symset gt = g->statetab[i]->go_to;
		for (j = 0; j < gt.cnt; j++)
			fprintf(f, "\t{ %d, %d },\n",
				gt.syms[j], gt.data[j]);
		fprintf(f, "};\n");
	}
}
#line 2007 "parsergen.mdc"
static void gen_states(FILE *f, struct grammar *g)
{
	int i;
	fprintf(f, "#line 0 \"gen_states\"\n");
	fprintf(f, "static const struct state states[] = {\n");
	for (i = 0; i < g->states; i++) {
		struct itemset *is = g->statetab[i];
		int j, prod = -1, prod_len;

		for (j = 0; j < is->items.cnt; j++) {
			int itm = is->items.syms[j];
			int p = item_prod(itm);
			int bp = item_index(itm);
			struct production *pr = g->productions[p];

			if (bp < pr->body_size)
				continue;
			/* This is what we reduce */
			if (prod < 0 || prod_len < pr->body_size) {
				prod = p;
				prod_len = pr->body_size;
			}
		}

		if (prod >= 0)
			fprintf(f, "\t[%d] = { %d, goto_%d, %d, %d, %d, %d, %d, %d },\n",
				i, is->go_to.cnt, i, prod,
				g->productions[prod]->body_size,
				g->productions[prod]->head->num,
				is->starts_line,
				g->productions[prod]->line_like,
				is->min_prefix);
		else
			fprintf(f, "\t[%d] = { %d, goto_%d, -1, -1, -1, %d, 0, %d },\n",
				i, is->go_to.cnt, i,
				is->starts_line, is->min_prefix);
	}
	fprintf(f, "};\n\n");
}
#line 2078 "parsergen.mdc"
static void gen_code(struct production *p, FILE *f, struct grammar *g)
{
	char *c;
	char *used = calloc(1, p->body_size);
	int i;

	fprintf(f, "\t\t\t");
	for (c = p->code.txt; c < p->code.txt + p->code.len; c++) {
		int n;
		int use = 0;
		if (*c != '$') {
			fputc(*c, f);
			if (*c == '\n')
				fputs("\t\t\t", f);
			continue;
		}
		c++;
		if (*c == '<') {
			use = 1;
			c++;
		}
		if (*c < '0' || *c > '9') {
			if (use)
				fputc('<', f);
			fputc(*c, f);
			continue;
		}
		n = *c - '0';
		while (c[1] >= '0' && c[1] <= '9') {
			c += 1;
			n = n * 10 + *c - '0';
		}
		if (n == 0)
			fprintf(f, "(*(struct %.*s*%s)ret)",
				p->head->struct_name.len,
				p->head->struct_name.txt,
				p->head->isref ? "*":"");
		else if (n > p->body_size)
			fprintf(f, "$%d", n);
		else if (p->body[n-1]->type == Terminal)
			fprintf(f, "(*(struct token *)body[%d])",
				n-1);
		else if (p->body[n-1]->struct_name.txt == NULL)
			fprintf(f, "$%d", n);
		else {
			fprintf(f, "(*(struct %.*s*%s)body[%d])",
				p->body[n-1]->struct_name.len,
				p->body[n-1]->struct_name.txt,
				p->body[n-1]->isref ? "*":"", n-1);
			used[n-1] = use;
		}
	}
	fputs("\n", f);
	for (i = 0; i < p->body_size; i++) {
		if (p->body[i]->struct_name.txt &&
		    used[i]) {
			// assume this has been copied out
			if (p->body[i]->isref)
				fprintf(f, "\t\t*(void**)body[%d] = NULL;\n", i);
			else
				fprintf(f, "\t\tmemset(body[%d], 0, sizeof(struct %.*s));\n", i, p->body[i]->struct_name.len, p->body[i]->struct_name.txt);
		}
	}
	free(used);
}
#line 2146 "parsergen.mdc"
static void gen_reduce(FILE *f, struct grammar *g, char *file,
                       struct code_node *code)
{
	int i;
	fprintf(f, "#line 1 \"gen_reduce\"\n");
	fprintf(f, "static int do_reduce(int prod, void **body, struct token_config *config, void *ret)\n");
	fprintf(f, "{\n");
	fprintf(f, "\tint ret_size = 0;\n");
	if (code)
		code_node_print(f, code, file);

	fprintf(f, "#line 4 \"gen_reduce\"\n");
	fprintf(f, "\tswitch(prod) {\n");
	for (i = 0; i < g->production_count; i++) {
		struct production *p = g->productions[i];
		fprintf(f, "\tcase %d:\n", i);

		if (p->code.txt) {
			fprintf(f, "#line %d \"%s\"\n", p->code_line, file);
			gen_code(p, f, g);
		}

		if (p->head->struct_name.txt)
			fprintf(f, "\t\tret_size = sizeof(struct %.*s%s);\n",
				p->head->struct_name.len,
				p->head->struct_name.txt,
				p->head->isref ? "*":"");

		fprintf(f, "\t\tbreak;\n");
	}
	fprintf(f, "\t}\n\treturn ret_size;\n}\n\n");
}
#line 2195 "parsergen.mdc"
static void gen_free(FILE *f, struct grammar *g)
{
	int i;

	fprintf(f, "#line 0 \"gen_free\"\n");
	fprintf(f, "static void do_free(short sym, void *asn)\n");
	fprintf(f, "{\n");
	fprintf(f, "\tif (!asn) return;\n");
	fprintf(f, "\tif (sym < %d) {\n", g->first_nonterm);
	fprintf(f, "\t\tfree(asn);\n\t\treturn;\n\t}\n");
	fprintf(f, "\tswitch(sym) {\n");

	for (i = 0; i < g->num_syms; i++) {
		struct symbol *s = g->symtab[i];
		if (!s ||
		    s->type != Nonterminal ||
		    s->struct_name.txt == NULL)
			continue;

		fprintf(f, "\tcase %d:\n", s->num);
		if (s->isref) {
			fprintf(f, "\t\tfree_%.*s(*(void**)asn);\n",
			        s->struct_name.len,
			        s->struct_name.txt);
			fprintf(f, "\t\tfree(asn);\n");
		} else
			fprintf(f, "\t\tfree_%.*s(asn);\n",
			        s->struct_name.len,
			        s->struct_name.txt);
		fprintf(f, "\t\tbreak;\n");
	}
	fprintf(f, "\t}\n}\n\n");
}
#line 2329 "parsergen.mdc"
static int errs;
static void pr_err(char *msg)
{
	errs++;
	fprintf(stderr, "%s\n", msg);
}
#line 2381 "parsergen.mdc"
static FILE *open_ext(char *base, char *ext)
{
	char *fn = malloc(strlen(base) + strlen(ext) + 1);
	FILE *f;
	strcat(strcpy(fn, base), ext);
	f = fopen(fn, "w");
	free(fn);
	return f;
}
#line 574 "parsergen.mdc"
static struct grammar *grammar_read(struct code_node *code)
{
	struct token_config conf = {
		.word_start = "",
		.word_cont = "",
		.words_marks = known,
		.known_count = sizeof(known)/sizeof(known[0]),
		.number_chars = "",
		.ignored = (1 << TK_line_comment)
		         | (1 << TK_block_comment)
			 | (0 << TK_number)
			 | (1 << TK_string)
			 | (1 << TK_multi_string)
			 | (1 << TK_in)
			 | (1 << TK_out),
	};

	struct token_state *state = token_open(code, &conf);
	struct token tk;
	struct symbol *head = NULL;
	struct grammar *g;
	char *err = NULL;

	g = calloc(1, sizeof(*g));
	symbols_init(g);

	for (tk = token_next(state); tk.num != TK_eof;
	     tk = token_next(state)) {
		if (tk.num == TK_newline)
			continue;
		if (tk.num == TK_ident) {
			// new non-terminal
			head = sym_find(g, tk.txt);
			if (head->type == Nonterminal)
				err = "This non-terminal has already be used.";
			else if (head->type == Virtual)
				err = "Virtual symbol not permitted in head of production";
			else {
				head->type = Nonterminal;
				head->struct_name = g->current_type;
				head->isref = g->type_isref;
				if (g->production_count == 0) {
#line 552 "parsergen.mdc"
	                                struct production *p = calloc(1,sizeof(*p));
	                                struct text start = {"$start",6};
	                                struct text eof = {"$eof",4};
	                                struct text code = {"$0 = $<1;", 9};
	                                p->head = sym_find(g, start);
	                                p->head->type = Nonterminal;
	                                p->head->struct_name = g->current_type;
	                                p->head->isref = g->type_isref;
	                                if (g->current_type.txt)
	                                	p->code = code;
	                                array_add(&p->body, &p->body_size, head);
	                                array_add(&p->body, &p->body_size, sym_find(g, eof));
	                                p->head->first_production = g->production_count;
	                                array_add(&g->productions, &g->production_count, p);
#line 617 "parsergen.mdc"
				}
				head->first_production = g->production_count;
				tk = token_next(state);
				if (tk.num == TK_mark &&
				    text_is(tk.txt, "->"))
					err = parse_production(g, head, state);
				else
					err = "'->' missing in production";
			}
		} else if (tk.num == TK_mark
		           && text_is(tk.txt, "|")) {
			// another production for same non-term
			if (head)
				err = parse_production(g, head, state);
			else
				err = "First production must have a head";
		} else if (tk.num == TK_mark
		           && text_is(tk.txt, "$")) {
			err = dollar_line(state, g, 0);
		} else if (tk.num == TK_mark
		           && text_is(tk.txt, "$*")) {
			err = dollar_line(state, g, 1);
		} else {
			err = "Unrecognised token at start of line.";
		}
		if (err)
			goto abort;
	}
	token_close(state);
	return g;
abort:
	fprintf(stderr, "Error at line %d: %s\n",
	        tk.line, err);
	token_close(state);
	free(g);
	return NULL;
}
#line 1511 "parsergen.mdc"
static void grammar_analyse(struct grammar *g, enum grammar_type type)
{
	struct symbol *s;
	struct itemset *is;
	int snum = TK_reserved;
	for (s = g->syms; s; s = s->next)
		if (s->num < 0 && s->type == Terminal) {
			s->num = snum;
			snum++;
		}
	g->first_nonterm = snum;
	for (s = g->syms; s; s = s->next)
		if (s->num < 0) {
			s->num = snum;
			snum++;
		}
	g->num_syms = snum;
	g->symtab = calloc(g->num_syms, sizeof(g->symtab[0]));
	for (s = g->syms; s; s = s->next)
		g->symtab[s->num] = s;

	set_nullable(g);
	set_line_like(g);
	if (type >= SLR)
		build_first(g);

	if (type == SLR)
		build_follow(g);

	build_itemsets(g, type);

	g->statetab = calloc(g->states, sizeof(g->statetab[0]));
	for (is = g->items; is ; is = is->next)
		g->statetab[is->state] = is;
}
#line 1554 "parsergen.mdc"
static int grammar_report(struct grammar *g, enum grammar_type type)
{
	report_symbols(g);
	if (g->follow)
		report_follow(g);
	report_itemsets(g);
	return report_conflicts(g, type);
}
#line 1897 "parsergen.mdc"
static void gen_parser(FILE *f, struct grammar *g, char *file, char *name,
	               struct code_node *pre_reduce)
{
	gen_known(f, g);
	gen_non_term(f, g);
	gen_goto(f, g);
	gen_states(f, g);
	gen_reduce(f, g, file, pre_reduce);
	gen_free(f, g);

	fprintf(f, "#line 0 \"gen_parser\"\n");
	fprintf(f, "void *parse_%s(struct code_node *code, struct token_config *config, FILE *trace)\n",
	        name);
	fprintf(f, "{\n");
	fprintf(f, "\tstruct token_state *tokens;\n");
	fprintf(f, "\tconfig->words_marks = known;\n");
	fprintf(f, "\tconfig->known_count = sizeof(known)/sizeof(known[0]);\n");
	fprintf(f, "\tconfig->ignored |= (1 << TK_line_comment) | (1 << TK_block_comment);\n");
	fprintf(f, "\ttokens = token_open(code, config);\n");
	fprintf(f, "\tvoid *rv = parser_run(tokens, states, do_reduce, do_free, trace, non_term, config);\n");
	fprintf(f, "\ttoken_close(tokens);\n");
	fprintf(f, "\treturn rv;\n");
	fprintf(f, "}\n\n");
}
#line 2459 "parsergen.mdc"
int main(int argc, char *argv[])
{
	struct section *s;
	int rv = 0;

	setlocale(LC_ALL,"");

#line 2263 "parsergen.mdc"
	int opt;
	char *outfile = NULL;
	char *infile;
	char *name;
	char *tag = NULL;
	int report = 1;
	enum grammar_type type = LR05;
	while ((opt = getopt_long(argc, argv, options,
	                          long_options, NULL)) != -1) {
		switch(opt) {
		case '0':
			type = LR0; break;
		case '5':
			type = LR05; break;
		case 'S':
			type = SLR; break;
		case 'L':
			type = LALR; break;
		case '1':
			type = LR1; break;
		case 'R':
			report = 2; break;
		case 'o':
			outfile = optarg; break;
		case 't':
			tag = optarg; break;
		default:
			fprintf(stderr, "Usage: parsergen ...\n");
			exit(1);
		}
	}
	if (optind < argc)
		infile = argv[optind++];
	else {
		fprintf(stderr, "No input file given\n");
		exit(1);
	}
	if (outfile && report == 1)
		report = 0;
	name = outfile;
	if (name && strchr(name, '/'))
		name = strrchr(name, '/')+1;
	
	if (optind < argc) {
		fprintf(stderr, "Excess command line arguments\n");
		exit(1);
	}
#line 2337 "parsergen.mdc"
	struct section *table;
	int fd;
	int len;
	char *file;
	fd = open(infile, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "parsergen: cannot open %s: %s\n",
			infile, strerror(errno));
		exit(1);
	}
	len = lseek(fd, 0, 2);
	file = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
	table = code_extract(file, file+len, pr_err);
	
	struct code_node *hdr = NULL;
	struct code_node *code = NULL;
	struct code_node *gram = NULL;
	struct code_node *pre_reduce = NULL;
	for (s = table; s; s = s->next) {
		struct text sec = s->section;
		if (tag && !strip_tag(&sec, tag))
			continue;
		if (text_is(sec, "header"))
			hdr = s->code;
		else if (text_is(sec, "code"))
			code = s->code;
		else if (text_is(sec, "grammar"))
			gram = s->code;
		else if (text_is(sec, "reduce"))
			pre_reduce = s->code;
		else {
			fprintf(stderr, "Unknown content section: %.*s\n",
			        s->section.len, s->section.txt);
			rv |= 2;
		}
	}
#line 2395 "parsergen.mdc"
	struct grammar *g = NULL;
	if (gram == NULL) {
		fprintf(stderr, "No grammar section provided\n");
		rv |= 2;
	} else {
		g = grammar_read(gram);
		if (!g) {
			fprintf(stderr, "Failure to parse grammar\n");
			rv |= 2;
		}
	}
	if (g) {
		grammar_analyse(g, type);
		if (report)
			if (grammar_report(g, type))
				rv |= 1;
	}
#line 2416 "parsergen.mdc"
	if (rv == 0 && hdr && outfile) {
		FILE *f = open_ext(outfile, ".h");
		if (f) {
			code_node_print(f, hdr, infile);
			fprintf(f, "void *parse_%s(struct code_node *code, struct token_config *config, FILE *trace);\n",
			        name);
			fclose(f);
		} else {
			fprintf(stderr, "Cannot create %s.h\n",
			        outfile);
			rv |= 4;
		}
	}
#line 2433 "parsergen.mdc"
	if (rv == 0 && outfile) {
		FILE *f = open_ext(outfile, ".c");
		if (f) {
			if (code)
				code_node_print(f, code, infile);
			gen_parser(f, g, infile, name, pre_reduce);
			fclose(f);
		} else {
			fprintf(stderr, "Cannot create %s.c\n",
			        outfile);
			rv |= 4;
		}
	}
#line 2469 "parsergen.mdc"

	return rv;
}
