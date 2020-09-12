#line 1972 "parsergen.mdc"
struct lookup {
	short sym;
	short state;
};
struct state {
	short go_to_cnt;
	const struct lookup * go_to;
	short reduce_prod;
	short reduce_size;
	short reduce_sym;
	char starts_line;
	char newline_only;
	short min_prefix;
};
#line 2955 "parsergen.mdc"
void *parser_run(struct token_state *tokens,
                 const struct state states[],
                 int (*do_reduce)(int, void**, struct token_config*, void*),
                 void (*do_free)(short, void*),
                 FILE *trace, const char *non_term[],
                 struct token_config *config);
