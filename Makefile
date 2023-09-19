#
# Copyright Neil Brown ©2015-2023 <neil@brown.name>
# May be distrubuted under terms of GPLv2 - see file:COPYING
#

SMATCH_CHECK_SAFE=1
export SMATCH_CHECK_SAFE

# if C=0 is given on command line, disable all checking.
# if C=1 is given, enable it
C=auto
ifeq "$(wildcard .CHECK)" ".CHECK"
 CHECK=$(C:auto=1)
else
 CHECK=$(C:auto=0)
endif
# if D=0 is given, don't add debug flags, fully optimize
# if D=1, add debugging, no optimization
# otherwise, .DEBUG file means compile with debugging
D=auto
ifeq "$(wildcard .DEBUG)" ".DEBUG"
 DEBUG=$(D:auto=1)
else
 DEBUG=$(D:auto=0)
endif
VERSION = $(shell [ -e .git ] && git describe --always HEAD | sed 's/edlib-//')
VERS_DATE = $(shell [ -e .git ] && date --iso-8601 --date="`git log -n1 --format=format:%cd --date=iso --date=short`")
DVERS = $(if $(VERSION),-DVERSION=\"$(VERSION)\",)
DDATE = $(if $(VERS_DATE),-DVERS_DATE="\"$(VERS_DATE)\"",)
VCFLAGS += $(DVERS) $(DDATE)

LDLIBS= -ldl
CC = gcc
floats= 16 32 64 128 16X 32X 64X 128X
have_float=$(foreach n,$(floats),-D__HAVE_FLOAT$(n)=0 -D__HAVE_DISTINCT_FLOAT$(n)=0)
SMATCH_FLAGS= -D_BITS_FLOATN_H $(have_float) -D__FLT_EVAL_METHOD__=1 $(VCFLAGS)

SPARSEFLAGS= -Wsparse-all -Wno-transparent-union -Wsparse-error $(SMATCH_FLAGS) $(VCFLAGS)
# Create files .DEBUG and .LEAK for extra checking
ifeq "$(DEBUG)" "1"
 DBG := -Werror -fno-omit-frame-pointer
 ifeq "$(wildcard .SANITIZE)" ".SANITIZE"
  DBG += -fsanitize=undefined
 endif
 ifeq "$(wildcard .LEAK)" ".LEAK"
  DBG += -fsanitize=leak
 endif
else
 DBG=-O3
endif
ifeq "$(CHECK)" "1"
 QUIET_CHECK   = $(Q:@=@echo    '     CHECK    '$<;)
else
 QUIET_CHECK   = @: skip
endif

ifeq "$(wildcard .SMATCH) $(CHECK)" ".SMATCH 1"
 # SYMLINK .SMATCH to the smatch binary for testing.
 SMATCH_CMD=$(shell readlink .SMATCH) $(SMATCH_FLAGS)
 QUIET_SMATCH  = $(Q:@=@echo    '     SMATCH   '$<;)$(SMATCH_CMD)
else
 QUIET_SMATCH  = @: skip
endif
CFLAGS= -g -Wall -Wstrict-prototypes -Wextra -Wno-unused-parameter $(DBG) $(VCFLAGS)
#CFLAGS= -pg -fno-pie -fno-PIC -g -Wall -Wstrict-prototypes -Wextra -Wno-unused-parameter $(DBG) $(VCFLAGS)
#Doesn't work :-( -fsanitize=address

all: edlib checksym lib shared NamesList.txt bin
test: edlib lib shared test-rexel
	./tests run

OBJ = O/edlib.o
LIBOBJ = O/core-mark.o O/core-doc.o O/core-editor.o O/core-attr.o \
	O/core-keymap.o O/core-pane.o O/core-misc.o O/core-log.o \
	O/core-version.o O/core-window.o
SHOBJ = O/doc-text.o O/doc-dir.o O/doc-docs.o \
	O/doc-email.o O/doc-multipart.o O/doc-list.o \
	O/render-hex.o O/render-lines.o \
	O/render-format.o O/render-complete.o \
	O/lib-view.o O/lib-tile.o O/lib-popup.o O/lib-linecount.o O/lib-keymap.o \
	O/lib-search.o O/lib-messageline.o O/lib-input.o O/lib-libevent.o \
	O/lib-history.o O/lib-crop.o O/lib-markup.o O/lib-rfc822header.o \
	O/lib-viewer.o O/lib-base64.o O/lib-qprint.o O/lib-utf8.o \
	O/lib-charset.o O/lib-config.o \
	O/lib-copybuf.o O/lib-whitespace.o O/lib-colourmap.o \
	O/lib-renderline.o O/lib-x11selection-gtk.o O/lib-autosave.o \
	O/lib-x11selection-xcb.o O/display-x11-xcb.o \
	O/lib-linefilter.o O/lib-wiggle.o O/lib-aspell.o O/lib-calc.o \
	O/lib-menu.o O/lib-unicode-names.o O/lib-askpass.o \
	O/lib-test-markup.o O/lib-menubar.o O/lib-rangetrack.o \
	O/lang-python.o \
	O/mode-emacs.o O/emacs-search.o \
	O/display-ncurses.o
XOBJ = O/rexel.o
WOBJ = O/libwiggle.a

BIN = edlib elma elc el-askpass

bin/edlib : edlib
bin/elma : edlib
bin/elc : python/lib-server.py
bin/el-askpass : python/lib-server.py

# From python 3.8 on we need python3-embed to get the right libraries
pypkg=$(shell pkg-config --atleast-version=3.8 python3 && echo python3-embed || echo python3)
LIBS-lang-python = $(shell pkg-config --libs $(pypkg))
INC-lang-python = $(shell pkg-config --cflags $(pypkg))

LIBS-display-ncurses = $(shell pkg-config --libs panelw ncursesw MagickWand)
INC-display-ncurses = $(shell pkg-config --cflags panelw ncursesw MagickWand)  -Wno-strict-prototypes
O/display-ncurses.o : md5.h

LIBS-lib-aspell = -laspell

LIBS-lib-libevent = $(shell pkg-config --libs libevent)

LIBS-lib-x11selection-gtk = $(shell pkg-config --libs gtk+-3.0)
INC-lib-x11selection-gtk = $(shell pkg-config --cflags gtk+-3.0)

LIBS-lib-x11selection-xcb = $(shell pkg-config --libs xcb)

LIBS-display-x11-xcb = $(shell pkg-config --libs xcb pango cairo pangocairo MagickWand xkbcommon xkbcommon-x11)
# magickwand include file contains a non-strict-prototype!
INC-display-x11-xcb = $(shell pkg-config --cflags xcb pango cairo pangocairo MagickWand xkbcommon xkbcommon-x11) -Wno-strict-prototypes

LIBS-lib-calc = -licuuc -lgmp
O/libcalc.a : calc-dir

O/core-editor-static.o : O/mod-list-decl.h O/mod-list.h

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
H = list.h core.h core-pane.h misc.h safe.h vfunc.h
edlib: $(OBJ) lib/libedlib.so
	$(QUIET_LINK)$(CC) $(CPPFLAGS) $(CFLAGS) -rdynamic -Wl,--disable-new-dtags -o $@ $(OBJ) -Llib -Wl,-rpath=`pwd`/lib -ledlib $(LDLIBS)

edlib-static: $(OBJ) $(STATICOBJ)  $(XOBJ) O/core-version.o
	$(QUIET_LINK)$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ $(LIBS-lang-python) $(LIBS-display-ncurses) $(LIBS-lib-libevent)

$(OBJ) $(SHOBJ) $(LIBOBJ) $(XOBJ) $(STATICOBJ) : $(H) O/.exists
$(LIBOBJ) : internal.h

O/lib-config.o : parse-ini.h

$(OBJ) : O/%.o : %.c
	$(QUIET_CC)$(CC) $(CPPFLAGS) $(CFLAGS) $(INC-$*) -c -o $@ $<
	$(QUIET_CHECK)sparse $(CPPFLAGS) $(INC-$*) $(SPARSEFLAGS) $<
	$(QUIET_SMATCH) $(CPPFLAGS) $(INC-$*) $<

$(SHOBJ) $(LIBOBJ) $(XOBJ) : O/%.o : %.c
	$(QUIET_CC)$(CC) -fPIC $(CPPFLAGS) $(CFLAGS) $(INC-$*) -c -o $@ $<
	$(QUIET_CHECK)sparse $(CPPFLAGS) $(INC-$*) $(SPARSEFLAGS) $<
	$(QUIET_SMATCH) $(CPPFLAGS) $(INC-$*) $<


O/libwiggle.a wiggle/wiggle.h : wiggle-dir

.PHONY: wiggle-dir calc-dir
wiggle-dir:
	@[ -f wiggle/wiggle.h ] || { git submodule init && git submodule update; }
	@$(MAKE) -q -C wiggle O=`pwd`/O OptDbg="-O3 -fPIC" lib || \
		{ echo '     MAKE     wiggle'; \
		$(MAKE) -C wiggle O=`pwd`/O OptDbg="-O3 -fPIC" lib ; }

calc-dir:
	@mkdir -p O
	@$(MAKE) -q -C calc O=`pwd`/O || \
		{ echo '     MAKE     calc'; \
		$(MAKE) -C calc O=`pwd`/O  ; }



$(STATICOBJ) : O/%-static.o : %.c
	$(QUIET_CCSTATIC)$(CC) -Dedlib_init=$(subst -,_,$*)_edlib_init $(CPPFLAGS) $(CFLAGS) $(INC-$*) -c -o $@ $<

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
	@ln -s ../python lib/python
	@ln -s ../data lib/edlib
	@touch $@

.PHONY: lib bin
lib: lib/libedlib.so lib/.exists
lib/libedlib.so: $(LIBOBJ)
	@mkdir -p lib
	$(QUIET_CC)$(CC) -shared -Wl,-soname,libedlib.so -o $@ $(LIBOBJ)
bin: $(patsubst %,bin/%,$(BIN))
bin/% :
	@mkdir -p bin
	cp $< $@
	@chmod +x $@

shared: $(SO)
lib/edlib-lib-search.so : O/lib-search.o $(XOBJ)
lib/edlib-lib-wiggle.so : O/lib-wiggle.o $(WOBJ)
lib/edlib-lib-calc.so : O/lib-calc.o O/libcalc.a

O/lib-search.o : rexel.h
O/lib-wiggle.o : wiggle/wiggle.h

$(SO) : lib/edlib-%.so : O/%.o O/core-version.o lib/.exists
	@mkdir -p lib
	$(QUIET_LIB)$(CC) -shared -Wl,-soname,edlib-$*.so -o $@ $(filter %.o,$^) $(filter %.a,$^) $(LIBS-$*)

O/mod-list.h : Makefile
	$(QUIET_SCRIPT)for file in $(patsubst O/%.o,%,$(subst -,_,$(SHOBJ))); do echo "{ \"$$file\", $${file}_edlib_init}," ; done | sort > $@
O/mod-list-decl.h : Makefile
	$(QUIET_SCRIPT)for file in $(patsubst O/%.o,%_edlib_init,$(subst -,_,$(SHOBJ))); do echo void $$file"(struct pane *ed);" ; done > $@

CSRC= attr.c

test2:
	@for f in $(CSRC); do sed -n -e 's/^#ifdef TEST_\(.*\)$$/\1/p' $$f | \
		while read test; do cc -g -o test_$$test -DTEST_$$test $(CSRC) && \
			./test_$$test || exit 2; \
	        done || exit 2; \
	done; echo SUCCESS

rexel: rexel.c rexel.h core-misc.c
	$(CC) -DDEBUG -g -o rexel rexel.c core-misc.c

test-rexel: rexel
	./rexel -T

NamesList.txt:
	wget https://unicode.org/Public/UNIDATA/NamesList.txt

checksym: edlib
	@nm edlib  | awk '$$2 == "T" {print $$3}' | while read a; do grep $$a *.h > /dev/null || echo  $$a; done | grep -vE '^(_.*|main)$$' ||:

.PHONY: clean
clean:
	rm -f edlib edlib-static rexel
	rm -rf lib O bin
