
LDLIBS= -lncursesw -levent
CPPFLAGS= -I/usr/include/ncursesw
CFLAGS=-g -Wall -Werror -Wstrict-prototypes -Wextra -Wno-unused-parameter

all:edlib checksym

OBJ = ncurses.o view.o tile.o mainloop.o text.o mark.o attr.o keymap.o pane.o \
	render_text.o render_hex.o \
	popup.o line_count.o
H = list.h text.h pane.h mark.h attr.h tile.h view.h keymap.h extras.h
edlib: $(OBJ)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o edlib $(OBJ) $(LDLIBS)

$(OBJ) : $(H)


CSRC= attr.c text.c

test:
	@for f in $(CSRC); do sed -n -e 's/^#ifdef TEST_\(.*\)$$/\1/p' $$f | \
		while read test; do cc -g -o test_$$test -DTEST_$$test $(CSRC) && \
			./test_$$test || exit 2; \
	        done || exit 2; \
	done; echo SUCCESS

checksym:
	@nm edlib  | awk '$$2 == "T" {print $$3}' | while read a; do grep $$a *.h > /dev/null || echo  $$a; done | grep -vE '^(_.*|main)$$' ||:
