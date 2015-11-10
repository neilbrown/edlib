#
# Copyright Neil Brown <neil@brown.name> 2015
# May be distrubuted under terms of GPLv2 - see file:COPYING
#

LDLIBS= -lncursesw -levent -ldl
CPPFLAGS= -I/usr/include/ncursesw
CFLAGS=-g -Wall -Werror -Wstrict-prototypes -Wextra -Wno-unused-parameter

all:edlib checksym libs

OBJ = mainloop.o \
	core-mark.o core-doc.o core-editor.o core-attr.o core-keymap.o core-pane.o
SHOBJ = doc-text.o doc-dir.o \
	render-text.o render-hex.o render-dir.o \
	lib-view.o lib-tile.o lib-popup.o lib-line-count.o lib-keymap.o \
	mode-emacs.o \
	ncurses.o

SO = $(patsubst %.o,lib/%.so,$(SHOBJ))
H = list.h extras.h core.h
edlib: $(OBJ)
	$(CC) $(CPPFLAGS) $(CFLAGS) -rdynamic -o edlib $(OBJ) $(LDLIBS)

$(OBJ) $(SHOBJ) : $(H)

$(SHOBJ) : %.o : %.c
	gcc -fPIC $(CPPFLAGS) $(CFLAGS) -c $<

libs: $(SO)
$(SO) : lib/%.so : %.o
	@mkdir -p lib
	gcc -shared -Wl,-soname,doc-text -o $@ $<


CSRC= attr.c

test:
	@for f in $(CSRC); do sed -n -e 's/^#ifdef TEST_\(.*\)$$/\1/p' $$f | \
		while read test; do cc -g -o test_$$test -DTEST_$$test $(CSRC) && \
			./test_$$test || exit 2; \
	        done || exit 2; \
	done; echo SUCCESS

checksym:
	@nm edlib  | awk '$$2 == "T" {print $$3}' | while read a; do grep $$a *.h > /dev/null || echo  $$a; done | grep -vE '^(_.*|main)$$' ||:

clean:
	rm -f edlib $(OBJ) $(SHOBJ)
	rm -rf lib
