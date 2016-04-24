#
# Copyright Neil Brown ©2015 <neil@brown.name>
# May be distrubuted under terms of GPLv2 - see file:COPYING
#

LDLIBS= -ldl
CPPFLAGS= -I.
CC = gcc
SPARSEFLAGS= -Wsparse-all -Wno-transparent-union -Wsparse-error
# use "make DBG=" to avoid the extra checks and errors
ifdef LEAK
DBG= -Werror -fno-omit-frame-pointer -fsanitize=undefined -fsanitize=leak
else
DBG= -Werror -fno-omit-frame-pointer -fsanitize=undefined
endif
CFLAGS=-g -Wall -Wstrict-prototypes -Wextra -Wno-unused-parameter $(DBG)
#Doesn't work :-( -fsanitize=address

all: dirs edlib checksym lib shared

OBJ = O/edlib.o
LIBOBJ = O/core-mark.o O/core-doc.o O/core-editor.o O/core-attr.o \
	O/core-keymap.o O/core-pane.o O/core-misc.o
SHOBJ = O/doc-text.o O/doc-dir.o O/doc-docs.o \
	O/render-hex.o O/render-lines.o \
	O/render-format.o O/render-complete.o \
	O/lib-view.o O/lib-tile.o O/lib-popup.o O/lib-linecount.o O/lib-keymap.o \
	O/lib-search.o O/lib-messageline.o O/lib-input.o O/lib-libevent.o \
	O/lib-history.o \
	O/lang-python.o \
	O/mode-emacs.o \
	O/display-ncurses.o
XOBJ = O/rexel.o O/emacs-search.o

LIBS-lang-python = -lpython2.7
INC-lang-python = -I/usr/include/python2.7

LIBS-display-ncurses = -lncursesw
INC-display-ncurses = -I/usr/include/ncursesw

LIBS-lib-libevent = -levent

#
# Pretty print - borrowed from 'sparse'
#
V	      = @
Q	      = $(V:1=)
QUIET_CC      = $(Q:@=@echo    '     CC       '$@;)
QUIET_AR      = $(Q:@=@echo    '     AR       '$@;)
QUIET_GEN     = $(Q:@=@echo    '     GEN      '$@;)
QUIET_LINK    = $(Q:@=@echo    '     LINK     '$@;)
QUIET_LIB     = $(Q:@=@echo    '     LIB      '$@;)
QUIET_CHECK   = $(Q:@=@echo    '     CHECK    '$<;)



SO = $(patsubst O/%.o,lib/edlib-%.so,$(SHOBJ))
H = list.h core.h misc.h
edlib: $(OBJ) lib/libedlib.so
	$(QUIET_LINK)$(CC) $(CPPFLAGS) $(CFLAGS) -rdynamic -Wl,--disable-new-dtags -o edlib $(OBJ) -Llib -Wl,-rpath=`pwd`/lib -ledlib $(LDLIBS)

$(OBJ) $(SHOBJ) $(LIBOBJ) $(XOBJ) : $(H)

$(OBJ) : O/%.o : %.c
	$(QUIET_CHECK)sparse $(CPPFLAGS) $(INC-$*) $(SPARSEFLAGS) $<
	$(QUIET_CC)$(CC) $(CPPFLAGS) $(INC-$*) $(CFLAGS) -c -o $@ $<

$(SHOBJ) $(LIBOBJ) $(XOBJ) : O/%.o : %.c
	$(QUIET_CHECK)sparse  $(CPPFLAGS) $(INC-$*) $(SPARSEFLAGS) $<
	$(QUIET_CC)$(CC) -fPIC $(CPPFLAGS) $(INC-$*) $(CFLAGS) -c -o $@ $<

.PHONY: TAGS
TAGS :
	etags -o TAGS.tmp Makefile *.h *.c python/*.py
	@sed 's/[\o177,].*//' TAGS > .TAGS1
	@sed 's/[\o177,].*//' TAGS.tmp > .TAGS2
	@if cmp -s .TAGS1 .TAGS2 ; then \
	    rm -f TAGS.tmp ; \
	else \
	    mv TAGS.tmp TAGS ;\
	fi
	@rm -f .TAGS1 .TAGS2


dirs :
	@mkdir -p lib O

lib: lib/libedlib.so
lib/libedlib.so: $(LIBOBJ)
	@mkdir -p lib
	$(QUIET_CC)$(CC) -shared -Wl,-soname,libedlib.so -o $@ $(LIBOBJ)

shared: $(SO)
lib/edlib-lib-search.so : O/lib-search.o O/rexel.o
lib/edlib-mode-emacs.so : O/mode-emacs.o O/emacs-search.o

$(SO) : lib/edlib-%.so : O/%.o
	@mkdir -p lib
	$(QUIET_LIB)$(CC) -shared -Wl,-soname,edlib-$*.so -o $@ $^ $(LIBS-$*)

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
