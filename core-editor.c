#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include "core.h"

struct editor *editor_new(void)
{
	struct editor *ed = calloc(sizeof(*ed), 1);
	INIT_LIST_HEAD(&ed->doctypes);
	return ed;
}
