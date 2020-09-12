#line 101 "mdcode.mdc"
#include <stdio.h>
#line 172 "mdcode.mdc"
struct text {
	char *txt;
	int len;
};

struct section {
	struct text section;
	struct code_node *code;
	struct section *next;
};

struct code_node {
	struct text code;
	int indent;
	int line_no;
	int needs_strip;
	struct code_node *next;
	struct section *child;
};

#line 731 "mdcode.mdc"
typedef void (*code_err_fn)(char *msg);

#line 268 "mdcode.mdc"
void code_free(struct code_node *code);

#line 352 "mdcode.mdc"
int text_cmp(struct text a, struct text b);

#line 782 "mdcode.mdc"
struct section *code_extract(char *pos, char *end, code_err_fn error);

#line 850 "mdcode.mdc"
void code_node_print(FILE *out, struct code_node *node, char *fname);

#line 104 "mdcode.mdc"

