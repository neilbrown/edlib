#
# Copyright Neil Brown <neil@brown.name> 2015
# May be distrubuted under terms of GPLv2 - see file:COPYING
#

LDLIBS= -lncursesw -levent -ldl
CPPFLAGS= -I. -I/usr/include/ncursesw
CFLAGS=-g -Wall -Werror -Wstrict-prototypes -Wextra -Wno-unused-parameter

all: dirs edlib checksym lib shared

OBJ = O/mainloop.o
LIBOBJ = O/core-mark.o O/core-doc.o O/core-editor.o O/core-attr.o \
	O/core-keymap.o O/core-pane.o O/core-misc.o
SHOBJ = O/doc-text.o O/doc-dir.o \
	O/render-text.o O/render-hex.o O/render-dir.o O/render-lines.o \
	O/render-format.o O/render-complete.o \
	O/lib-view.o O/lib-tile.o O/lib-popup.o O/lib-line-count.o O/lib-keymap.o \
	O/lib-search.o \
	O/mode-emacs.o \
	O/display-ncurses.o
XOBJ = O/rexel.o O/emacs-search.o

SO = $(patsubst O/%.o,lib/edlib-%.so,$(SHOBJ))
H = list.h core.h misc.h
edlib: $(OBJ) lib/libedlib.so
	$(CC) $(CPPFLAGS) $(CFLAGS) -rdynamic -Wl,--disable-new-dtags -o edlib $(OBJ) -Llib -Wl,-rpath=`pwd`/lib -ledlib $(LDLIBS)

$(OBJ) $(SHOBJ) $(LIBOBJ) : $(H)

$(OBJ) : O/%.o : %.c
	gcc $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(SHOBJ) $(LIBOBJ) $(XOBJ) : O/%.o : %.c
	gcc -fPIC $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

dirs :
	@mkdir -p lib O

lib: lib/libedlib.so
lib/libedlib.so: $(LIBOBJ)
	@mkdir -p lib
	gcc -shared -Wl,-soname,libedlib.so -o $@ $(LIBOBJ)

shared: $(SO)
lib/edlib-lib-search.so : O/lib-search.o O/rexel.o
lib/edlib-mode-emacs.so : O/mode-emacs.o O/emacs-search.o

$(SO) : lib/edlib-%.so : O/%.o
	@mkdir -p lib
	gcc -shared -Wl,-soname,edlib-$*.so -o $@ $^


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
	rm -f edlib
	rm -rf lib O
