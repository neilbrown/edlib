#line 41 "parsergen.mdc"
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#line 76 "parsergen.mdc"
#include "mdcode.h"
#include "scanner.h"
#line 2684 "parsergen.mdc"
#include <memory.h>
#line 2770 "parsergen.mdc"
#include "parser.h"
#line 2486 "parsergen.mdc"
static int search(const struct state *l, int sym)
{
	int lo = 0;
	int hi = l->go_to_cnt;

	if (hi == 0)
		return -1;
	while (lo + 1 < hi) {
		int mid = (lo + hi) / 2;
		if (l->go_to[mid].sym <= sym)
			lo = mid;
		else
			hi = mid;
	}
	if (l->go_to[lo].sym == sym)
		return l->go_to[lo].state;
	else
		return -1;
}
#line 2549 "parsergen.mdc"
struct parser {
	struct frame {
		short state;
		short newline_permitted;

		short sym;
		short indents;
		short since_newline;
		short since_indent;
	} *stack;
	void **asn_stack;
	int stack_size;
	int tos;
};
#line 2596 "parsergen.mdc"
static int shift(struct parser *p,
                 short sym, short indents, short start_of_line,
                 void *asn,
                 const struct state states[])
{
	// Push an entry onto the stack
	struct frame next = {0};
	int newstate = p->tos
		? search(&states[p->stack[p->tos-1].state],
		         sym)
		: 0;
	if (newstate < 0)
		return 0;
	if (p->tos >= p->stack_size) {
		p->stack_size += 10;
		p->stack = realloc(p->stack, p->stack_size
	                           * sizeof(p->stack[0]));
		p->asn_stack = realloc(p->asn_stack, p->stack_size
	                           * sizeof(p->asn_stack[0]));
	}
	next.sym = sym;
	next.indents = indents;
	next.state = newstate;
	if (states[newstate].starts_line)
		next.newline_permitted = 1;
	else if (indents)
		next.newline_permitted = 0;
	else if (p->tos)
		next.newline_permitted =
			p->stack[p->tos-1].newline_permitted;
	else
		next.newline_permitted = 0;

	if (!start_of_line) {
		if (p->tos)
			next.since_newline = p->stack[p->tos-1].since_newline + 1;
		else
			next.since_newline = 1;
	}
	if (indents)
		next.since_indent = 0;
	else if (p->tos)
		next.since_indent = p->stack[p->tos-1].since_indent + 1;
	else
		next.since_indent = 1;

	p->stack[p->tos] = next;
	p->asn_stack[p->tos] = asn;
	p->tos++;
	return 1;
}
#line 2656 "parsergen.mdc"
static int pop(struct parser *p, int num,
               short *start_of_line,
               void(*do_free)(short sym, void *asn))
{
	int i;
	short indents = 0;
	int sol = 0;

	p->tos -= num;
	for (i = 0; i < num; i++) {
		sol |= !p->stack[p->tos+i].since_newline;
		indents += p->stack[p->tos+i].indents;
		do_free(p->stack[p->tos+i].sym,
		        p->asn_stack[p->tos+i]);
	}
	if (start_of_line)
		*start_of_line = sol;
	return indents;
}
#line 2688 "parsergen.mdc"
void *memdup(void *m, int len)
{
	void *ret;
	if (len == 0)
		return NULL;
	ret = malloc(len);
	memcpy(ret, m, len);
	return ret;
}

static struct token *tok_copy(struct token tk)
{
	struct token *new = malloc(sizeof(*new));
	*new = tk;
	return new;
}
#line 2981 "parsergen.mdc"
static char *reserved_words[] = {
	[TK_error]        = "ERROR",
	[TK_in]           = "IN",
	[TK_out]          = "OUT",
	[TK_newline]      = "NEWLINE",
	[TK_eof]          = "$eof",
};
static void parser_trace_state(FILE *trace, struct frame *f, const struct state states[])
{
	fprintf(trace, "(%d", f->state);
	if (states[f->state].starts_line)
		fprintf(trace, "s");
	if (f->newline_permitted)
		fprintf(trace, "n%d", f->since_newline);
	fprintf(trace, ") ");
}

void parser_trace(FILE *trace, struct parser *p,
		  struct token *tk, const struct state states[],
		  const char *non_term[], int knowns)
{
	int i;
	if (!trace)
		return;
	for (i = 0; i < p->tos; i++) {
		struct frame *f = &p->stack[i];
		if (i) {
			int sym = f->sym;
			if (sym < TK_reserved &&
			    reserved_words[sym] != NULL)
				fputs(reserved_words[sym], trace);
			else if (sym < TK_reserved + knowns) {
				struct token *t = p->asn_stack[i];
				text_dump(trace, t->txt, 20);
			} else
				fputs(non_term[sym - TK_reserved - knowns],
				      trace);
			if (f->indents)
				fprintf(trace, ".%d", f->indents);
			if (f->since_newline == 0)
				fputs("/", trace);
			fputs(" ", trace);
		}
		parser_trace_state(trace, f, states);
	}
	fprintf(trace, "[");
	if (tk->num < TK_reserved &&
	    reserved_words[tk->num] != NULL)
		fputs(reserved_words[tk->num], trace);
	else
		text_dump(trace, tk->txt, 20);
	fprintf(trace, ":%d:%d]", tk->line, tk->col);
}

void parser_trace_action(FILE *trace, char *action)
{
	if (trace)
		fprintf(trace, " - %s\n", action);
}
#line 2774 "parsergen.mdc"
static int in_lookahead(struct token *tk, const struct state *states, int state)
{
	while (state >= 0) {
		if (search(&states[state], tk->num) >= 0)
			return 1;
		if (states[state].reduce_prod < 0)
			return 0;
		state = search(&states[state], states[state].reduce_sym);
	}
	return 0;
}

void *parser_run(struct token_state *tokens,
                 const struct state states[],
                 int (*do_reduce)(int, void**, struct token_config*, void*),
                 void (*do_free)(short, void*),
                 FILE *trace, const char *non_term[],
                 struct token_config *config)
{
	struct parser p = { 0 };
	struct token *tk = NULL;
	int accepted = 0;
	void *ret = NULL;

	shift(&p, TK_eof, 0, 1, NULL, states);
	while (!accepted) {
		struct token *err_tk;
		struct frame *tos = &p.stack[p.tos-1];
		if (!tk)
			tk = tok_copy(token_next(tokens));
		parser_trace(trace, &p,
		             tk, states, non_term, config->known_count);

		if (tk->num == TK_in) {
			tos->indents += 1;
			tos->since_newline = 0;
			tos->since_indent = 0;
			if (!states[tos->state].starts_line)
				tos->newline_permitted = 0;
			free(tk);
			tk = NULL;
			parser_trace_action(trace, "Record");
			continue;
		}
		if (tk->num == TK_out) {
			if (states[tos->state].reduce_size >= 0 &&
			    states[tos->state].reduce_size <= tos->since_indent)
				goto force_reduce;
			if (states[tos->state].min_prefix >= tos->since_indent) {
				// OK to cancel
				struct frame *in = tos - tos->since_indent;
				in->indents -= 1;
				if (in->indents == 0) {
					/* Reassess since_indent and newline_permitted */
					if (in > p.stack) {
						in->since_indent = in[-1].since_indent + 1;
						in->newline_permitted = in[-1].newline_permitted;
					} else {
						in->since_indent = 0;
						in->newline_permitted = 0;
					}
					if (states[in->state].starts_line)
						in->newline_permitted = 1;
					while (in < tos) {
						in += 1;
						in->since_indent = in[-1].since_indent + 1;
						if (states[in->state].starts_line)
							in->newline_permitted = 1;
						else
							in->newline_permitted = in[-1].newline_permitted;
					}
				}
				free(tk);
				tk = NULL;
				parser_trace_action(trace, "Cancel");
				continue;
			}
			// fall through to error handling as both SHIFT and REDUCE
			// will fail.
		}
		if (tk->num == TK_newline) {
			if (!tos->newline_permitted) {
				free(tk);
				tk = NULL;
				parser_trace_action(trace, "Discard");
				continue;
			}
			if (tos->since_newline > 1 &&
			    states[tos->state].reduce_size >= 0 &&
			    states[tos->state].reduce_size <= tos->since_newline)
				goto force_reduce;
		}
		if (shift(&p, tk->num, 0, tk->num == TK_newline, tk, states)) {
			tk = NULL;
			parser_trace_action(trace, "Shift");
			continue;
		}
	force_reduce:
		if (states[tos->state].reduce_prod >= 0 &&
		    states[tos->state].newline_only &&
		    !(tk->num == TK_newline ||
		      tk->num == TK_eof ||
		      tk->num == TK_out ||
		      (tos->indents == 0 && tos->since_newline == 0))) {
			/* Anything other than newline or out or eof
			 * in an error unless we are already at start
			 * of line, as this production must end at EOL.
			 */
		} else if (states[tos->state].reduce_prod >= 0) {
			void **body;
			void *res;
			const struct state *nextstate = &states[tos->state];
			int prod = nextstate->reduce_prod;
			int size = nextstate->reduce_size;
			int bufsize;
			static char buf[16*1024];
			short indents, start_of_line;

			body = p.asn_stack + (p.tos - size);

			bufsize = do_reduce(prod, body, config, buf);

			indents = pop(&p, size, &start_of_line,
			              do_free);
			res = memdup(buf, bufsize);
			memset(buf, 0, bufsize);
			if (!shift(&p, nextstate->reduce_sym,
			           indents, start_of_line,
			           res, states)) {
				if (prod != 0) abort();
				accepted = 1;
				ret = res;
			}
			parser_trace_action(trace, "Reduce");
			continue;
		}
		/* Error. We walk up the stack until we
		 * find a state which will accept TK_error.
		 * We then shift in TK_error and see what state
		 * that takes us too.
		 * Then we discard input tokens until
		 * we find one that is acceptable.
		 */
		parser_trace_action(trace, "ERROR");
		short indents = 0, start_of_line;

		err_tk = tok_copy(*tk);
		while (p.tos > 0 &&
		       shift(&p, TK_error, 0, 0,
		             err_tk, states) == 0)
			// discard this state
			indents += pop(&p, 1, &start_of_line, do_free);
		if (p.tos == 0) {
			free(err_tk);
			// no state accepted TK_error
			break;
		}
		tos = &p.stack[p.tos-1];
		while (!in_lookahead(tk, states, tos->state) &&
		       tk->num != TK_eof) {
			free(tk);
			tk = tok_copy(token_next(tokens));
			if (tk->num == TK_in)
				indents += 1;
			if (tk->num == TK_out) {
				if (indents == 0)
					break;
				indents -= 1;
				// FIXME update since_indent here
			}
		}
		tos->indents += indents;
	}
	free(tk);
	pop(&p, p.tos, NULL, do_free);
	free(p.asn_stack);
	free(p.stack);
	return ret;
}
