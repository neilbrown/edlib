
LDLIBS= -lncursesw -levent -ldl
CPPFLAGS= -I/usr/include/ncursesw
CFLAGS=-g -Wall -Werror -Wstrict-prototypes -Wextra -Wno-unused-parameter

all:edlib checksym libs

OBJ = ncurses.o view.o tile.o mainloop.o attr.o keymap.o pane.o \
	popup.o line_count.o \
	core-mark.o core-doc.o core-editor.o \
	mode-emacs.o
SHOBJ = doc-text.o doc-dir.o \
	render_text.o render_hex.o render_dir.o

SO = $(patsubst %.o,lib/%.so,$(SHOBJ))
H = list.h pane.h attr.h tile.h view.h extras.h core.h
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
