#line 16 "scanner.mdc"
#include "mdcode.h"
#line 168 "scanner.mdc"
#include <string.h>
#line 933 "scanner.mdc"
#include <memory.h>
#line 1219 "scanner.mdc"
#include <malloc.h>
#line 1257 "scanner.mdc"
#include <stdio.h>
#line 1339 "scanner.mdc"
#include "scanner.h"
#line 44 "scanner.mdc"
struct token_state {
#line 83 "scanner.mdc"
	struct token_config *conf;
#line 716 "scanner.mdc"
	int	indent_level;
	int	indent_sizes[20];
#line 788 "scanner.mdc"
	int check_indent;
	int delayed_lines;
	int out_next;
#line 936 "scanner.mdc"
	struct code_node *node;
	int    offset;
	int    line;
	int    col;
	int    strip_offset;
#line 1029 "scanner.mdc"
	int    prev_offset;
	int    prev_line;
	int    prev_col;
#line 1056 "scanner.mdc"
	int	prev_offset2;
	int	prev_line2;
	int	prev_col2;
#line 46 "scanner.mdc"
};
#line 261 "scanner.mdc"
static int is_word_start(wchar_t ch, struct token_config *conf)
{
	return iswalpha(ch) ||
	       strchr(conf->word_start, ch) != NULL ||
	       u_hasBinaryProperty(ch, UCHAR_ID_START);
}

static int is_word_continue(wchar_t ch, struct token_config *conf)
{
	return iswalnum(ch) ||
	       strchr(conf->word_cont, ch) != NULL ||
	       u_hasBinaryProperty(ch, UCHAR_ID_CONTINUE);
}
#line 327 "scanner.mdc"
static int is_mark(wchar_t ch, struct token_config *conf)
{
	return ch > ' ' &&
	       ch < 0x7f &&
	       !iswalnum(ch) &&
	       strchr(conf->word_start, ch) == NULL;
}
#line 457 "scanner.mdc"
static int is_quote(wchar_t ch)
{
	return ch == '\'' || ch == '"' || ch == '`'; // "
}
#line 576 "scanner.mdc"
static int is_line_comment(struct text txt)
{
	return (txt.len >= 1 && txt.txt[0] == '#') ||
	       (txt.len >= 2 && txt.txt[0] == '/' &&
	                        txt.txt[1] == '/');
}

static int is_block_comment(struct text txt)
{
	return txt.len >= 2 && txt.txt[0] == '/' &&
	       txt.txt[1] == '*';
}
#line 802 "scanner.mdc"
static int state_indent(struct token_state *state)
{
	if (state->node == NULL)
		return state->col;
	return state->node->indent - state->node->needs_strip + state->col;
}
#line 944 "scanner.mdc"
static void do_strip(struct token_state *state)
{
	int indent = 0;
	if (state->node->needs_strip) {
		int n = 4;
		while (n && state->node->code.txt[state->offset] == ' ') {
			indent += 1;
			state->offset += 1;
			n -= 1;
		}
		while (n == 4 && state->node->code.txt[state->offset] == '\t') {
			indent = indent_tab(indent);
			state->offset += 1;
			n -= 4;
		}
	}
}

static void state_check_node(struct token_state *state)
{
	if (!state->node)
		return;
	if (state->node->code.len > state->offset)
		return;

	do
		state->node = state->node->next;
	while (state->node && state->node->code.txt == NULL);
	state->offset = 0;
	state->prev_offset = 0;
	state->strip_offset = 0;
	state->col = 0;
	if (state->node == NULL)
		return;
	state->line = state->node->line_no;
	do_strip(state);
	state->col = state->node->needs_strip;
	state->strip_offset = state->offset;
}

static wint_t get_char(struct token_state *state)
{
	wchar_t next;
	size_t n;
	mbstate_t mbstate;

	state_check_node(state);
	if (state->node == NULL)
		return WEOF;

#line 1034 "scanner.mdc"
	state->prev_offset = state->offset;
	state->prev_line   = state->line;
	state->prev_col    = state->col;
#line 995 "scanner.mdc"

	memset(&mbstate, 0, sizeof(mbstate));

	n = mbrtowc(&next, state->node->code.txt + state->offset,
		    state->node->code.len - state->offset,
		    &mbstate);
	if (n == -2 || n == 0) {
		/* Not enough bytes - not really possible */
		next = '\n';				// NOTEST
		state->offset = state->node->code.len;	// NOTEST
	} else if (n == -1) {
		/* error */
		state->offset += 1;			// NOTEST
		next = 0x7f; // an illegal character	// NOTEST
	} else
		state->offset += n;

	if (next >= ' ') {
		state->col += 1;
	} else if (is_newline(next)) {
		state->line += 1;
		do_strip(state);
		state->col = state->node->needs_strip;
	} else if (next == '\t') {
		state->col = indent_tab(state->col);
	}
	return next;
}
#line 1040 "scanner.mdc"
static void unget_char(struct token_state *state)
{
	if (state->node) {
		state->offset = state->prev_offset;
		state->line   = state->prev_line;
		state->col    = state->prev_col;
	}
}
#line 1061 "scanner.mdc"
static void save_unget_state(struct token_state *state)
{
	state->prev_offset2 = state->prev_offset;
	state->prev_line2 = state->prev_line;
	state->prev_col2 = state->prev_col;
}

static void restore_unget_state(struct token_state *state)
{
	state->prev_offset = state->prev_offset2;
	state->prev_line = state->prev_line2;
	state->prev_col = state->prev_col2;
}
#line 1117 "scanner.mdc"
static void close_token(struct token_state *state,
                        struct token *tk)
{
	if (state->node != tk->node)
		tk->txt.len = tk->node->code.len - (tk->txt.txt - tk->node->code.txt);
	else
		tk->txt.len = (state->node->code.txt + state->offset)
		              - tk->txt.txt;
}

static void reset_token(struct token_state *state, struct token *tok)
{
	state->prev_line = tok->line;
	state->prev_col = tok->col;
	state->prev_offset = tok->txt.txt - state->node->code.txt;
	unget_char(state);
	tok->txt.len = 0;
}
#line 1144 "scanner.mdc"
static int at_son(struct token_state *state)
{
	return state->prev_offset <= state->strip_offset;
}

static int at_eon(struct token_state *state)
{
	// at end-of-node ??
	return state->node == NULL ||
	       state->offset >= state->node->code.len;
}
#line 1164 "scanner.mdc"
static int find_known(struct token_config *conf, struct text txt)
{
	int lo = 0;
	int hi = conf->known_count;

	while (lo + 1 < hi) {
		int mid = (lo + hi) / 2;
		int cmp = strncmp(conf->words_marks[mid],
		                  txt.txt, txt.len);
		if (cmp == 0 && conf->words_marks[mid][txt.len])
			cmp = 1;
		if (cmp <= 0)
			lo = mid;
		else
			hi = mid;
	}
	if (strncmp(conf->words_marks[lo],
	           txt.txt, txt.len) == 0
	    && conf->words_marks[lo][txt.len] == 0)
		return lo;
	else
		return -1;
}
#line 52 "scanner.mdc"
struct token token_next(struct token_state *state)
{
#line 86 "scanner.mdc"
	int ignored = state->conf->ignored;
#line 55 "scanner.mdc"
	while (1) {
		wint_t ch;
		struct token tk;

#line 1108 "scanner.mdc"
	        tk.node = state->node;
	        if (state->node)
	        	tk.txt.txt = state->node->code.txt + state->offset;
	        tk.line = state->line;
	        tk.col = state->col;
	        tk.txt.len = 0;
#line 842 "scanner.mdc"
	        if (state->check_indent || state->delayed_lines) {
	        	if (state_indent(state) < state->indent_sizes[state->indent_level]) {
	        		if (!state->out_next &&
	        		    !(ignored & (1<<TK_newline))) {
	        			state->out_next = 1;
	        			tk.num = TK_newline;
	        			return tk;
	        		}
	        		state->indent_level -= 1;
	        		state->out_next = 0;
	        		tk.num = TK_out;
	        		return tk;
	        	}
	        	if (state_indent(state) > state->indent_sizes[state->indent_level] &&
	        	    state->indent_level < sizeof(state->indent_sizes)-1) {
	        		state->indent_level += 1;
	        		state->indent_sizes[state->indent_level] = state_indent(state);
	        		if (state->delayed_lines)
	        			state->delayed_lines -= 1;
	        		tk.num = TK_in;
	        		return tk;
	        	}
	        	state->check_indent = 0;
	        	if (state->delayed_lines && !(ignored & (1<<TK_newline))) {
	        		tk.num = TK_newline;
	        		state->delayed_lines -= 1;
	        		return tk;
	        	}
	        	state->delayed_lines = 0;
	        	continue;
	        }
#line 1085 "scanner.mdc"
	        if (at_eon(state)) {
	        	get_char(state);
	        	unget_char(state);
	        	tk.node = state->node;
	        	if (state->node)
	        		tk.txt.txt = state->node->code.txt + state->offset;
	        	tk.line = state->line;
	        	tk.col = state->col;
	        	tk.txt.len = 0;
	        }
#line 1205 "scanner.mdc"
	        
	        ch = get_char(state);
	        
#line 677 "scanner.mdc"
	        if (ch <= ' ' && !is_newline(ch)
	            && ! at_son(state))
	        	continue;
#line 810 "scanner.mdc"
	        if (is_newline(ch))
	        	state_check_node(state);
	        if (is_newline(ch) || (at_son(state) && ch <= ' ')) {
	        	int newlines = 0;
	        	int was_nl = is_newline(ch);
	        	if (ignored & (1<<TK_in)) {
	        		if (!is_newline(ch))
	        			continue;
	        		if (ignored & (1<<TK_newline))
	        			continue;
	        		tk.num = TK_newline;
	        		close_token(state, &tk);
	        		return tk;
	        	}
	        	// Indents are needed, so check all white space.
	        	while (ch <= ' ' && ch != WEOF) {
	        		if (is_newline(ch))
	        			newlines += 1;
	        		ch = get_char(state);
	        		if (is_newline(ch))
	        			state_check_node(state);
	        	}
	        	if (ch != WEOF)
	        		unget_char(state);
	        	state->delayed_lines = newlines;
	        	state->out_next = !was_nl;
	        	state->check_indent = 1;
	        	continue;
	        }
#line 884 "scanner.mdc"
	        if (ch == WEOF) {
	        	tk.num = TK_eof;
	        	return tk;
	        }
#line 172 "scanner.mdc"
	        if (iswdigit(ch) && !(ignored & (1<<TK_number))) {
	        	int prev = 0;
	        	int expect_p = 0;
	        	int decimal_mark = 0;
	        	if (ch == '0') {
	        		wchar_t ch2 = get_char(state);
	        		if (strchr("xobXOB", ch2) != NULL)
	        			expect_p = 1;
	        		unget_char(state);
	        	}
	        	while (1) {
	        		int sign_ok = 0;
	        		switch(expect_p) {
	        		case 0:
	        			if (ch == 'e' || ch == 'E') {
	        				sign_ok = 1;
	        				decimal_mark = 1;
	        			}
	        			break;
	        		case 1:
	        			if (ch == 'p' || ch == 'P') {
	        				sign_ok = 1;
	        				decimal_mark = 1;
	        			}
	        			break;
	        		}
	        		save_unget_state(state);
	        		prev = ch;
	        		ch = get_char(state);
	        
	        		if (!iswalnum(prev)) {
	        			/* special characters, like separators and decimal marks
	        			 * and signs, must be followed by a hexdigit, and the
	        			 * space and signs must be followed by a decimal digit.
	        			 */
	        			if (!iswxdigit(ch) ||
	        			   ((prev == '-' || prev == '+') && !iswdigit(ch)) ||
	        			   (prev == ' ' && !iswdigit(ch))) {
	        				/* don't want the new char or the special */
	        				restore_unget_state(state);
	        				break;
	        			}
	        		}
	        		if (iswalnum(ch))
	        			continue;
	        
	        		if (!strchr(state->conf->number_chars, ch)) {
	        			/* non-number char */
	        			break;
	        		}
	        		if (ch == '+' || ch == '-') {
	        			/* previous must be 'e' or 'p' in appropraite context */
	        			if (!sign_ok)
	        				break;
	        			expect_p = -1;
	        		} else if (ch == ' ') {
	        			/* previous must be a digit */
	        			if (!iswdigit(prev))
	        				break;
	        		} else {
	        			/* previous must be a hex digit */
	        			if (!iswxdigit(prev))
	        				break;
	        		}
	        		if (ch == '.' || ch == ',') {
	        			/* only one of these permitted */
	        			if (decimal_mark)
	        				break;
	        			decimal_mark = 1;
	        		}
	        	}
	        	/* We seem to have a "number" token */
	        	unget_char(state);
	        	close_token(state, &tk);
	        	tk.num = TK_number;
	        	return tk;
	        }
#line 297 "scanner.mdc"
	        if (is_word_start(ch, state->conf)) {
	        	int n;
	        	/* A word: identifier or reserved */
	        	do
	        		ch = get_char(state);
	        	while (is_word_continue(ch, state->conf));
	        	unget_char(state);
	        	close_token(state, &tk);
	        	tk.num = TK_ident;
	        	if (ignored & (1<<TK_ident))
	        		tk.num = TK_error;
	        	n = find_known(state->conf, tk.txt);
	        	if (n >= 0)
	        		tk.num = TK_reserved + n;
	        	return tk;
	        }
#line 370 "scanner.mdc"
	        tk.num = TK_error;
	        while (is_mark(ch, state->conf)) {
	        	int n;
	        	wchar_t prev;
	        	close_token(state, &tk);
	        	n = find_known(state->conf, tk.txt);
	        	if (n >= 0)
	        		tk.num = TK_reserved + n;
	        	else if (tk.num != TK_error) {
	        		/* found a longest-known-mark, still need to
	        		 * check for comments
	        		 */
	        		if (tk.txt.len == 2 && tk.txt.txt[0] == '/' &&
	        		    (ch == '/' || ch == '*')) {
	        			/* Yes, this is a comment, not a '/' */
	        			restore_unget_state(state);
	        			tk.num = TK_error;
	        			break;
	        		}
	        		unget_char(state);
	        		close_token(state, &tk);
	        		return tk;
	        	}
	        	prev = ch;
	        	save_unget_state(state);
	        	ch = get_char(state);
	        	if (!(ignored & (1<<TK_string)) && n < 0 &&is_quote(ch) && !is_quote(prev))
	        		/* If strings are allowed, a quote (Which isn't a known mark)
	        		 * mustn't be treated as part of an unknown mark.  It can be
	        		 * part of a multi-line srtings though.
	        		 */
	        		break;
	        	if (prev == '#' && n < 0)
	        		/* '#' is not a known mark, so assume it is a comment */
	        		break;
	        	if (prev == '/' && ch == '/' && tk.txt.len == 1 && n < 0) {
	        		close_token(state, &tk);
	        		restore_unget_state(state);
	        		break;
	        	}
	        	if (prev == '/' && ch == '*' && tk.txt.len == 1 && n < 0) {
	        		close_token(state, &tk);
	        		restore_unget_state(state);
	        		break;
	        	}
	        }
	        unget_char(state);
	        if (tk.num != TK_error) {
	        	close_token(state, &tk);
	        	return tk;
	        }
#line 469 "scanner.mdc"
	        if (tk.txt.len == 3 &&
	            !(ignored & (1 << TK_multi_string)) &&
	            is_quote(tk.txt.txt[0]) &&
	            memcmp(tk.txt.txt, tk.txt.txt+1, 2) == 0 &&
	            is_newline(tk.txt.txt[3])) {
	        	// triple quote
	        	wchar_t first = tk.txt.txt[0];
	        	int qseen = 0;
	        	int at_sol = 1;
	        	while (!at_eon(state) && qseen < 3) {
	        		ch = get_char(state);
	        		if (is_newline(ch)) {
	        			at_sol = 1;
	        			qseen = 0;
	        		} else if (at_sol && ch == first) {
	        			qseen += 1;
	        		} else if (ch != ' ' && ch != '\t') {
	        			at_sol = 0;
	        			qseen = 0;
	        		}
	        	}
	        	if (qseen != 3) {
	        		/* Hit end of node - error.
	        		 * unget so the newline is seen,
	        		 * but return rest of string as an error.
	        		 */
	        		if (is_newline(ch))
	        			unget_char(state);
	        		close_token(state, &tk);
	        		tk.num = TK_error;
	        		return tk;
	        	}
	        	/* 2 letters are allowed */
	        	ch = get_char(state);
	        	if (iswalpha(ch))
	        		ch = get_char(state);
	        	if (iswalpha(ch))
	        		ch = get_char(state);
	        	/* Now we must have a newline, but we don't return it
	        	 * whatever it is.*/
	        	unget_char(state);
	        	close_token(state, &tk);
	        	tk.num = TK_multi_string;
	        	if (!is_newline(ch))
	        		tk.num = TK_error;
	        	return tk;
	        }
#line 526 "scanner.mdc"
	        if (tk.txt.len && is_quote(tk.txt.txt[0]) &&
	            !(ignored & (1<<TK_string))) {
	        	wchar_t first = tk.txt.txt[0];
	        	reset_token(state, &tk);
	        	ch = get_char(state);
	        	tk.num = TK_error;
	        	while (!at_eon(state) && !is_newline(ch)) {
	        		ch = get_char(state);
	        		if (ch == first) {
	        			tk.num = TK_string;
	        			break;
	        		}
	        		if (is_newline(ch)) {
	        			unget_char(state);
	        			break;
	        		}
	        	}
	        	while (!at_eon(state) && (ch = get_char(state)) &&
	        	                          iswalpha(ch))
	        		;
	        	unget_char(state);
	        	close_token(state, &tk);
	        	return tk;
	        }
#line 596 "scanner.mdc"
	        if (is_line_comment(tk.txt)) {
	        	while (!is_newline(ch) && !at_eon(state))
	        		ch = get_char(state);
	        	if (is_newline(ch))
	        		unget_char(state);
	        	close_token(state, &tk);
	        	tk.num = TK_line_comment;
	        	if (ignored & (1 << TK_line_comment))
	        		continue;
	        	return tk;
	        }
#line 620 "scanner.mdc"
	        if (is_block_comment(tk.txt)) {
	        	wchar_t prev;
	        	int newlines = 0;
	        	reset_token(state, &tk);
	        	get_char(state);
	        	get_char(state);
	        	save_unget_state(state);
	        	ch = get_char(state);
	        	prev = 0;
	        	while (!at_eon(state) &&
	        	       (prev != '/' || ch != '*') &&
	        	       (prev != '*' || ch != '/')) {
	        		if (is_newline(ch))
	        			newlines = 1;
	        		prev = ch;
	        		save_unget_state(state);
	        		ch = get_char(state);
	        	}
	        	close_token(state, &tk);
	        	if (at_eon(state)) {
	        		tk.num = TK_error;
	        		return tk;
	        	}
	        	if (prev == '/') {
	        		/* embedded.  Need to unget twice! */
	        		restore_unget_state(state);
	        		unget_char(state);
	        		tk.num = TK_error;
	        		return tk;
	        	}
	        	tk.num = TK_block_comment;
	        	if (newlines && !(ignored & (1<<TK_newline))) {
	        		/* next char must be newline */
	        		ch = get_char(state);
	        		unget_char(state);
	        		if (!is_newline(ch))
	        			tk.num = TK_error;
	        	}
	        	if (tk.num == TK_error ||
	        	    !(ignored & (1 << TK_block_comment)))
	        		return tk;
	        	continue;
	        }
#line 897 "scanner.mdc"
	        /* one unknown mark character */
	        if (tk.txt.len) {
	        	close_token(state, &tk);
	        	if (ignored & (1<<TK_mark))
	        		tk.num = TK_error;
	        	else
	        		tk.num = TK_mark;
	        	return tk;
	        }
	        /* Completely unrecognised character is next, possibly
	         * a digit and we are ignoring numbers.
	         * What ever it is, make it an error.
	         */
	        get_char(state);
	        close_token(state, &tk);
	        tk.num = TK_error;
	        return tk;
#line 429 "scanner.mdc"
	        
#line 1212 "scanner.mdc"
	        
#line 60 "scanner.mdc"
	}
}
#line 1222 "scanner.mdc"
struct token_state *token_open(struct code_node *code, struct
                               token_config *conf)
{
	struct token_state *state = malloc(sizeof(*state));
	memset(state, 0, sizeof(*state));
	state->node = code;
	state->line = code->line_no;
	do_strip(state);
	state->col = state->node->needs_strip;
	state->strip_offset = state->offset;
	state->conf = conf;
	return state;
}
void token_close(struct token_state *state)
{
	free(state);
}
#line 1265 "scanner.mdc"
void text_dump(FILE *f, struct text txt, int max)
{
	int i;
	if (txt.len > max)
		max -= 2;
	else
		max = txt.len;
	for (i = 0; i < max; i++) {
		char c = txt.txt[i];
		if (c < ' ' || c > '~')
			fprintf(f, "\\x%02x", c & 0xff);
		else if (c == '\\')
			fprintf(f, "\\\\");
		else
			fprintf(f, "%c", c);
	}
	if (i < txt.len)
		fprintf(f, "..");
}

void token_trace(FILE *f, struct token tok, int max)
{
	static char *types[] = {
		[TK_ident] = "ident",
		[TK_mark] = "mark",
		[TK_number] = "number",
		[TK_string] = "string",
		[TK_multi_string] = "mstring",
		[TK_line_comment] = "lcomment",
		[TK_block_comment] = "bcomment",
		[TK_in] = "in",
		[TK_out] = "out",
		[TK_newline] = "newline",
		[TK_eof] = "eof",
		[TK_error] = "ERROR",
		};

	switch (tok.num) {
	default: /* known word or mark */
		fprintf(f, "%.*s", tok.txt.len, tok.txt.txt);
		break;
	case TK_in:
	case TK_out:
	case TK_newline:
	case TK_eof:
		/* No token text included */
		fprintf(f, "%s()", types[tok.num]);
		break;
	case TK_ident:
	case TK_mark:
	case TK_number:
	case TK_string:
	case TK_multi_string:
	case TK_line_comment:
	case TK_block_comment:
	case TK_error:
		fprintf(f, "%s(", types[tok.num]);
		text_dump(f, tok.txt, max);
		fprintf(f, ")");
		break;
	}
}
#line 1343 "scanner.mdc"

