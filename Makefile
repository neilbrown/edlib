#
# Copyright Neil Brown ©2015 <neil@brown.name>
# May be distrubuted under terms of GPLv2 - see file:COPYING
#
MAKEFLAGS += -j

SMATCH_CHECK_SAFE=1
export SMATCH_CHECK_SAFE

VERSION = $(shell [ -d .git ] && git describe --always HEAD | sed 's/edlib-//')
VERS_DATE = $(shell [ -d .git ] && date --iso-8601 --date="`git log -n1 --format=format:%cd --date=iso --date=short`")
DVERS = $(if $(VERSION),-DVERSION=\"$(VERSION)\",)
DDATE = $(if $(VERS_DATE),-DVERS_DATE="\"$(VERS_DATE)\"",)
VCFLAGS += $(DVERS) $(DDATE)

LDLIBS= -ldl
CC = gcc
SMATCH_FLAGS= -D_BITS_FLOATN_H -D__HAVE_FLOAT128=0 -D__FLT_EVAL_METHOD__=1 -D__HAVE_DISTINCT_FLOAT128=0 $(VCFLAGS)
SPARSEFLAGS= -Wsparse-all -Wno-transparent-union -Wsparse-error $(SMATCH_FLAGS) $(VCFLAGS)
# Create files .DEBUG and .LEAK for extra checking
ifeq "$(wildcard .DEBUG)" ".DEBUG"
 ifeq "$(wildcard .LEAK)" ".LEAK"
  DBG= -Werror -fno-omit-frame-pointer -fsanitize=undefined -fsanitize=leak
 else
  DBG= -Werror -fno-omit-frame-pointer -fsanitize=undefined
 endif
 QUIET_CHECK   = $(Q:@=@echo    '     CHECK    '$<;)
else
 DBG=
 QUIET_CHECK   = @: skip
endif
ifeq "$(wildcard .SMATCH)" ".SMATCH"
 # SYMLINK .SMATCH to the smatch binary for testing.
 SMATCH_CMD=$(shell readlink .SMATCH) $(SMATCH_FLAGS)
 QUIET_SMATCH  = $(Q:@=@echo    '     SMATCH   '$<;)$(SMATCH_CMD)
else
 QUIET_SMATCH  = @: skip
endif
CFLAGS=-g -Wall -Wstrict-prototypes -Wextra -Wno-unused-parameter $(DBG) $(VCFLAGS)
#Doesn't work :-( -fsanitize=address

all: edlib checksym lib shared

OBJ = O/edlib.o
LIBOBJ = O/core-mark.o O/core-doc.o O/core-editor.o O/core-attr.o \
	O/core-keymap.o O/core-pane.o O/core-misc.o
SHOBJ = O/doc-text.o O/doc-dir.o O/doc-docs.o \
	O/doc-email.o O/doc-multipart.o \
	O/render-hex.o O/render-lines.o \
	O/render-format.o O/render-complete.o \
	O/lib-view.o O/lib-tile.o O/lib-popup.o O/lib-linecount.o O/lib-keymap.o \
	O/lib-search.o O/lib-messageline.o O/lib-input.o O/lib-libevent.o \
	O/lib-history.o O/lib-crop.o O/lib-renderline.o O/lib-rfc822header.o \
	O/lib-viewer.o O/lib-base64.o O/lib-qprint.o O/lib-utf8.o \
	O/lib-copybuf.o \
	O/lang-python.o \
	O/mode-emacs.o \
	O/display-ncurses.o
XOBJ = O/rexel.o O/emacs-search.o

LIBS-lang-python = $(shell pkg-config --libs python-2.7)
INC-lang-python = $(shell pkg-config --cflags python-2.7)

LIBS-display-ncurses = $(shell pkg-config --libs ncursesw)
INC-display-ncurses = $(shell pkg-config --cflags ncursesw)
O/display-ncurses.o : md5.h

LIBS-lib-libevent = $(shell pkg-config --libs libevent)

O/core-editor-static.o : O/mod-list-decl.h O/mod-list.h
O/core-keymap.o : jhash.h

STATICOBJ = $(SHOBJ:.o=-static.o) $(LIBOBJ:.o=-static.o)

#
# Pretty print - borrowed from 'sparse'
#
V	      = @
Q	      = $(V:1=)
QUIET_CC      = $(Q:@=@echo    '     CC       '$@;)
QUIET_CCSTATIC= $(Q:@=@echo    '     CCstatic '$@;)
QUIET_AR      = $(Q:@=@echo    '     AR       '$@;)
QUIET_GEN     = $(Q:@=@echo    '     GEN      '$@;)
QUIET_LINK    = $(Q:@=@echo    '     LINK     '$@;)
QUIET_LIB     = $(Q:@=@echo    '     LIB      '$@;)
QUIET_SCRIPT  = $(Q:@=@echo    '     SCRIPT   '$@;)

SO = $(patsubst O/%.o,lib/edlib-%.so,$(SHOBJ))
H = list.h core.h misc.h
edlib: $(OBJ) lib/libedlib.so
	$(QUIET_LINK)$(CC) $(CPPFLAGS) $(CFLAGS) -rdynamic -Wl,--disable-new-dtags -o $@ $(OBJ) -Llib -Wl,-rpath=`pwd`/lib -ledlib $(LDLIBS)

edlib-static: $(OBJ) $(STATICOBJ)  $(XOBJ)
	$(QUIET_LINK)$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ $(LIBS-lang-python) $(LIBS-display-ncurses) $(LIBS-lib-libevent)

$(OBJ) $(SHOBJ) $(LIBOBJ) $(XOBJ) $(STATICOBJ) : $(H) O/.exists

$(OBJ) : O/%.o : %.c
	$(QUIET_CHECK)sparse $(CPPFLAGS) $(INC-$*) $(SPARSEFLAGS) $<
	$(QUIET_SMATCH) $(CPPFLAGS) $(INC-$*) $<
	$(QUIET_CC)$(CC) $(CPPFLAGS) $(INC-$*) $(CFLAGS) -c -o $@ $<

$(SHOBJ) $(LIBOBJ) $(XOBJ) : O/%.o : %.c
	$(QUIET_CHECK)sparse $(CPPFLAGS) $(INC-$*) $(SPARSEFLAGS) $<
	$(QUIET_SMATCH) $(CPPFLAGS) $(INC-$*) $<
	$(QUIET_CC)$(CC) -fPIC $(CPPFLAGS) $(INC-$*) $(CFLAGS) -c -o $@ $<

$(STATICOBJ) : O/%-static.o : %.c
	$(QUIET_CCSTATIC)$(CC) -Dedlib_init=$(subst -,_,$*)_edlib_init $(CPPFLAGS) $(INC-$*) $(CFLAGS) -c -o $@ $<

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

O/.exists:
	@mkdir -p O
	@touch $@
lib/.exists:
	@mkdir -p lib
	@touch $@

.PHONY: lib
lib: lib/libedlib.so lib/.exists
lib/libedlib.so: $(LIBOBJ) core-version.c
	@$(CC) $(CFLAGS) -c -o O/core-version.o core-version.c
	@mkdir -p lib
	$(QUIET_CC)$(CC) -shared -Wl,-soname,libedlib.so -o $@ $(LIBOBJ) O/core-version.o

shared: $(SO)
lib/edlib-lib-search.so : O/lib-search.o O/rexel.o
lib/edlib-mode-emacs.so : O/mode-emacs.o O/emacs-search.o

$(SO) : lib/edlib-%.so : O/%.o lib/.exists
	@mkdir -p lib
	$(QUIET_LIB)$(CC) -shared -Wl,-soname,edlib-$*.so -o $@ $(filter %.o,$^) $(LIBS-$*)

O/mod-list.h : Makefile
	$(QUIET_SCRIPT)for file in $(patsubst O/%.o,%,$(subst -,_,$(SHOBJ))); do echo "{ \"$$file\", $${file}_edlib_init}," ; done | sort > $@
O/mod-list-decl.h : Makefile
	$(QUIET_SCRIPT)for file in $(patsubst O/%.o,%_edlib_init,$(subst -,_,$(SHOBJ))); do echo void $$file"(struct pane *ed);" ; done > $@

CSRC= attr.c

test:
	@for f in $(CSRC); do sed -n -e 's/^#ifdef TEST_\(.*\)$$/\1/p' $$f | \
		while read test; do cc -g -o test_$$test -DTEST_$$test $(CSRC) && \
			./test_$$test || exit 2; \
	        done || exit 2; \
	done; echo SUCCESS

checksym: edlib
	@nm edlib  | awk '$$2 == "T" {print $$3}' | while read a; do grep $$a *.h > /dev/null || echo  $$a; done | grep -vE '^(_.*|main)$$' ||:

.PHONY: clean
clean:
	rm -f edlib edlib-static
	rm -rf lib O
