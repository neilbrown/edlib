#
# Copyright Neil Brown <neil@brown.name> 2015
# May be distrubuted under terms of GPLv2 - see file:COPYING
#

LDLIBS= -lncursesw -levent -ldl
CPPFLAGS= -I/usr/include/ncursesw
CFLAGS=-g -Wall -Werror -Wstrict-prototypes -Wextra -Wno-unused-parameter

all:edlib checksym lib shared

OBJ = mainloop.o
LIBOBJ = core-mark.o core-doc.o core-editor.o core-attr.o core-keymap.o core-pane.o
SHOBJ = doc-text.o doc-dir.o \
	render-text.o render-hex.o render-dir.o \
	lib-view.o lib-tile.o lib-popup.o lib-line-count.o lib-keymap.o \
	mode-emacs.o \
	display-ncurses.o

SO = $(patsubst %.o,lib/edlib-%.so,$(SHOBJ))
H = list.h core.h
edlib: $(OBJ) lib
	$(CC) $(CPPFLAGS) $(CFLAGS) -rdynamic -o edlib $(OBJ) -Llib -Wl,-rpath=`pwd`/lib -ledlib $(LDLIBS)

$(OBJ) $(SHOBJ) : $(H)

$(SHOBJ) $(LIBOBJ) : %.o : %.c
	gcc -fPIC $(CPPFLAGS) $(CFLAGS) -c $<

lib: lib/libedlib.so
lib/libedlib.so: $(LIBOBJ)
	@mkdir -p lib
	gcc -shared -Wl,-soname,libedlib.so -o $@ $(LIBOBJ)

shared: $(SO)
$(SO) : lib/edlib-%.so : %.o
	@mkdir -p lib
	gcc -shared -Wl,-soname,edlib-$*.so -o $@ $<


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
	rm -f edlib $(OBJ) $(SHOBJ) $(LIBOBJ)
	rm -rf lib
