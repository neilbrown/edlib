#line 23 "scanner.mdc"
#include <wchar.h>
#include <wctype.h>
#include <unicode/uchar.h>
#line 35 "scanner.mdc"
struct token {
	int               num;
	struct code_node *node;
	struct text       txt;
	int               line, col;
};
struct token_state;
#line 77 "scanner.mdc"
struct token_config {
	int ignored;	// bit set of ignored tokens.
#line 154 "scanner.mdc"
	char *number_chars;
#line 257 "scanner.mdc"
	char *word_start;
	char *word_cont;
#line 292 "scanner.mdc"
	const char **words_marks;
	int known_count;
#line 80 "scanner.mdc"
};
#line 99 "scanner.mdc"
enum token_num {
	TK_error,
#line 161 "scanner.mdc"
	TK_number,
#line 280 "scanner.mdc"
	TK_ident,
#line 365 "scanner.mdc"
	TK_mark,
#line 453 "scanner.mdc"
	TK_string,
	TK_multi_string,
#line 572 "scanner.mdc"
	TK_line_comment,
	TK_block_comment,
#line 697 "scanner.mdc"
	TK_in,
	TK_out,
#line 743 "scanner.mdc"
	TK_newline,
#line 881 "scanner.mdc"
	TK_eof,
#line 102 "scanner.mdc"
	TK_reserved
};
#line 49 "scanner.mdc"
struct token token_next(struct token_state *state);
#line 671 "scanner.mdc"
static inline int is_newline(wchar_t ch)
{
	return ch == '\n' || ch == '\f' || ch == '\v';
}
#line 704 "scanner.mdc"
static inline int indent_tab(int indent)
{
	return (indent|7)+1;
}
#line 1241 "scanner.mdc"
struct token_state *token_open(struct code_node *code, struct
                               token_config *conf);
void token_close(struct token_state *state);
#line 1260 "scanner.mdc"
void token_trace(FILE *f, struct token tok, int max);
void text_dump(FILE *f, struct text t, int max);
#line 1336 "scanner.mdc"

