# -*- coding: utf-8 -*-
# Copyright Neil Brown ©2015,2016 <neil@brown.name>
# May be distributed under terms of GPLv2 - see file:COPYING

# edlib rendering module for slide presentation.
# This reads lines from document, interprets as something vaguely like
# markdown or restructuredtext, and displays a page at a time with formatting.
#
# Parsing is very line-oriented.
# - a line starting ":ptype:" defines attributes for given paragraph type
# - a line starting "#" is a heading, "##" is a level-2 heading  (H1 H2 ...)
# - a line starting ">" is indented   (I)
# - a line starting "-" is a list item.  (L1)
# - a line starting "    -" after a list item is a second-level list item (L2)
# - a line starting "    " after a list item is more text in that list item
# - a line starting "    " elsewhere is a code fragment (C)
# - a line starting "!" treats rest of line a name of file containing an image
# - other lines are text (P)
#
# A level-1 heading (H1) starts a new page. attribute lines immediately preceding
# it are part of the page and apply only to the page
# Attribute lines before the first page apply to whole document but can be over-ridden
# in a page.
#
# Within text *word* becomes bold (b), _word_ is italic (i) `word` is monospaced (m)
#
# attributes can be defined for paragraph types (H1,h2,I,L1,L2,C,P) and text types
# (b,i,m).  Possible attributes for para or text are:
#  normal oblique italic bold small-caps large NN family:fontfamily fg:colour bg:colour inverse
# Possible attributes for para only:
#   left:size right:size centre space-after:size space-before:size
#
# All the above can also be given for "default" and apply before other defaults and
# global/local setting.  "default" can also be given:
#   xscale:size yscale:size
#
# All sizes are in "points".  A "point" is either the height of the window divided by
# the yscale size, or the width divided by xscale size, whichever is smaller.
#
# global defaults are:

default_attrs = "normal 10 family:sans fg:black bg:white left:5 space-after:1 space-before:1 xscale:10 yscale:40"

import re
import os

def take(name, place, args, default=None):
    if name in args:
        place.append(args[name])
    else:
        place.append(default)
    return 1

class PresenterPane(edlib.Pane):
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus, self.handle)
        self.globals = {}
        self.pageview = focus.call("doc:add-view") - 1
        self.attrview = focus.call("doc:add-view") - 1
        self.borderless = False

    def marks_same(self, m1, m2):
        if isinstance(m1, edlib.Mark) and isinstance(m1, edlib.Mark):
            return 1 == self.call("doc:mark-same", m1, m2);
        return None

    def first_page(self):
        m = []
        self.call("doc:vmark-get", self.pageview, lambda key, **a: take('mark', m, a))
        return m[0]

    def first_line(self):
        m = []
        self.call("doc:vmark-get", self.attrview, lambda key, **a: take('mark', m, a))
        return m[0]

    def prev_page(self, m):
        m2 = []
        self.call("doc:vmark-get", self.pageview, 3, m, lambda key, **a: take('mark2', m2, a))
        return m2[0]

    def prev_line(self, m):
        m2 = []
        self.call("doc:vmark-get", self.attrview, 3, m, lambda key, **a: take('mark2', m2, a))
        return m2[0]

    def get_line_at(self, m):
        # call render-line at m
        line = []
        self.parent.call("render-line", m, -1, lambda key, **a: take('str', line, a, ''))
        return line[0]

    def get_line_before(self, m):
        m2 = m.dup()
        ret = self.parent.call("render-line-prev", m2, 1)
        if ret <= 0:
            return None
        l = self.get_line_at(m2)
        return l

    def print_line_at(self, m):
        x = m.dup()
        l = self.get_line_at(x)
        return l[0:-1]

    def find_page(self, start, sof = False):
        # Assumming 'm' is either start-of-file (if 'sof') or the start of
        # a page, find the start of the next page.  Return an untyped mark
        # or None if there is no next page.
        # A page starts with some ":attr:" lines and then a "# " line.
        # If not 'sof' we first need to skip over the start of 'this' page.
        skipping = not sof
        m = start.dup()
        maybe = None
        globals = None
        if sof:
            globals = {}
            extra = {}
        while True:
            if not maybe and not skipping:
                maybe = m.dup()
            l = self.get_line_at(m)
            if not l:
                # End of document
                return None
            if l[0] == ':':
                # attribute - doesn't advance possible start
                if globals is not None:
                    mt = re.match("^:([^: ]+):(.*)", l.strip('\n'))
                    if mt:
                        extra[mt.group(1)] = mt.group(2)
            elif l[0:2] == "# ":
                # level-1 heading. This is it if not skipping
                if skipping:
                    skipping = False
                else:
                    if globals is not None:
                        self.globals = globals
                        if 'background' in globals:
                            self['background'] = 'call:Present-BG:'+globals['background']
                        if 'scale' in globals:
                            self['scale'] = globals['scale']
                        self.call("render-lines:redraw")
                    return maybe
            else:
                # not part of start-of-page
                skipping = False
                maybe = None
                if globals is not None:
                    globals.update(extra)
                    extra = {}

    def check_start(self, start):
        # This was the start of a page, but might not be any more.  Must check.
        # Following lines must be ":attr:" or "# ".
        # Preceeding line, if any must not be ":attr:"
        m = start.dup()
        l = self.get_line_at(m)
        while l and l[0] == ':':
            l = self.get_line_at(m)
        if not l or l[0:2] != '# ':
            # No appropriate heading
            return False

        m = start.dup()
        l = self.get_line_before(m)
        if l and l[0] == ':':
            return False
        return True

    def find_pages(self, m):
        # I need to find the start of the page that contains 'm' - though if m is before
        # the first page, then I want the first page.
        # I need to know that the 'next' page after that really is the start of the following
        # page, or is None iff this is the last page.
        # Each page mark has a 'valid' attribute which confirms it is at start of page,
        # and a 'next-valid' attribute which confirms next page is valid.  These are cleared
        # when nearby text changes.
        # The PresenterPane has a 'first_valid' flag if the first page mark is valid.
        # it is like 'next-valid'.
        # If these check out, answer is easy.  If they don't we need to revalidate anything
        # that doesn't match and either set a flag or delete a mark or add a mark.  Then
        # try again.
        pm = None
        while not pm:
            pm = self.prev_page(m)
            if not pm:
                # could be in pre-amble
                pm = self.first_page()
                if not pm:
                    # no marks at all.  Create mark-at-start and find page
                    pm = edlib.Mark(self, self.pageview)
                    first = self.find_page(pm, True)
                    if first:
                        # Good, there is a page.
                        pm.to_mark(first)
                        self.first_valid = True
                        pm['valid'] = 'yes'
                        pm = None
                        continue
                    # no pages at all
                    break
                if not self.first_valid:
                    # We have a first page, but it might not be valid
                    t = edlib.Mark(self)
                    first = self.find_page(t, True)
                    if not first:
                        # no pages any more
                        pass
                    elif self.marks_same(pm, first):
                        # Oh good!
                        self.first_valid = True
                        pm['valid'] = 'yes'
                    elif first < pm:
                        pm.to_mark(first)
                        self.first_valid = True
                        pm['valid'] = 'yes'
                        # but having moved,..
                        pm['next-valid'] = 'no'
                    else:
                        # .first_page() is before real first, just delete it
                        pm.release()
                    pm = None
                    continue
                # So this first page is valid, but is after the target.
                # Fall through to checking next-page
            elif pm['valid'] != 'yes':
                # We have a previous page but need to validate it
                if self.check_start(pm):
                    pm['valid'] = 'yes'
                else:
                    if pm.prev():
                        pm.prev()['next-valid'] = 'no'
                    else:
                        self.first_valid = False
                    pm.release()
                    pm = None
                    continue

            # pm is the start of a page, just need to check next is vald
            if pm['next-valid'] != 'yes':
                next = self.find_page(pm)
                if not next:
                    # this is last page
                    while pm.next():
                        pm.next().release()
                    pm['next-valid'] = 'yes'
                    # all good now
                elif pm.next() is None:
                    n = edlib.Mark(orig=pm)
                    n.to_mark(next)
                    pm['next-valid'] = 'yes'
                    pm = None
                else:
                    if self.marks_same(next, pm.next()):
                        pm['next-valid'] = 'yes'
                    elif next <= pm.next():
                        pm.next().to_mark(next)
                    else:
                        pm.next().release()
                    pm = None

        if not self.first_valid:
            # need to update globals
            t = edlib.Mark(self)
            first = self.find_page(t, True)
            if first:
                self.first_valid = True

        #x = self.first_page()
        #while x:
        #    print "PAGE:", x.seq, self.print_line_at(x)
        #    x = x.next()
        #print "RETURN", pm.seq
        return pm

    # We maintain marks at the start of every line in the current page
    # These have tags storing attributes extracted from the line.
    # - If there are marks not in the current page, discard them.
    # - If there are no marks, create them
    # - get an attribute by searching back for marks of the right type
    #   If one has no type, validate it
    #
    def clean_lines(self, page):
        next = page.next()
        first = self.first_line()
        while first and ((first < page and not self.marks_same(first,page)) or
                         (next and first > next and not self.marks_same(first, next))):
            # first is outside this page
            first.release(); first = None
            first = self.first_line()

    def annotate(self, mark, line):
        m = re.match("^:([^: ]+):(.*)", line.strip('\n'))
        if m:
            mark['type'] = 'attr:'+m.group(1)
            mark['value'] = m.group(2)
        else:
            mark['type'] = 'text'
            mark['value'] = '-None-'

    def mark_lines(self, page):
        first = self.first_line()
        if first:
            return
        # there are currently no lines
        next = page.next()
        line = edlib.Mark(self, self.attrview)
        line.to_mark(page)

        while not next or (line < next and not self.marks_same(line, next)):
            # There is a line there that we care about - unless EOF
            this = edlib.Mark(orig=line)
            l = self.get_line_at(line)
            if not l:
                break
            self.annotate(this, l)
        line.release()

    def line_reval(self, l, page):
        while l.prev() is not None and l.prev()['type'] == 'unknown':
            l2 = l.prev()
            l.release()
            l = l2

        if l is None:
            l = edlib.Mark(self, self.attrview)
            l.to_mark(page)
        # l is a good starting point.  parse until l.next or page.next
        end = l.next()
        if end is None:
            end = page.next()

        while not end or (l < end and not self.marks_same(l, end)):
            # There is a line there that we care about - unless EOF
            this = edlib.Mark(orig=l)
            txt = self.get_line_at(l)
            if not txt:
                break
            self.annotate(this, txt)
        l.release()

    def get_local_attr(self, m, attr, page):
        t = 'attr:' + attr
        l = self.prev_line(m)
        while l:
            if l['type'] == 'unknown':
                self.line_reval(l, page)
                l = self.prev_line(m)
                continue

            if l['type'] == t:
                return l['value']
            l = l.prev()
        return None

    paras = {
        '# ' : 'H1',
        '## ': 'H2',
        '>'  : 'I',
        '- ' : 'L1',
        '!'  : 'IM',
        ''   : 'P',
        None : 'BL',
        '    - ': 'L2',
        '     ': {'L2':'L2c', 'L2c':'L2c', 'L1':'L1c', 'L1c': 'L1c', None: 'C'},
        '    ' : {'L2':'L1c', 'L2c':'L1c', 'L1':'L1c', 'L1c': 'L1c', None: 'C'},
        ' '    : {'L2':'L1c', 'L2c':'L1c', 'L1':'L1c', 'L1c': 'L1c', None: 'P'},
        }
    defaults = {
        'H1': 'center,30,family:serif',
        'H2': 'center,20,fg:red,bold',
        'I': 'left:50,family:Mufferaw',
        'C': 'family:mono',
        'L1': 'left:20,family:sans,bullet:#,tab:20',
        'L1c': 'left:20,family:sans,tab:20',
        'L2': 'left:40,family:sans,bullet:#,tab:20',
        'L2c': 'left:40,family:sans,tab:20',
        'P': 'left:30,family:sans,space-below:20',
        'BL': '3',
        'bullet': 'fg:red',
        'bold': 'bold',
        'italic':'italic'
        }

    def get_attr(self, here, mode, page):
        v = self.get_local_attr(here, mode, page)
        if not v and mode in self.globals:
            v = self.globals[mode]
        if not v and mode in self.defaults:
            v = self.defaults[mode]
        if not v:
            v = ""
        return v

    def pathto(self, f):
        if f[0] == '/':
            return f
        path = self['filename']
        if not path:
            return f
        return os.path.dirname(path)+'/'+f

    def handle(self, key, **a):
        if key[:10] == "Present-BG":
            cmds = key[11:].split(',')
            f = a['focus']
            ret = 0
            for c in cmds:
                rv = None
                if c[:6] == 'color:':
                    rv = f.call('pane-clear', c[6:])
                if c[:14] == "image-stretch:":
                    rv = f.call('image-display', 1, self.pathto(c[14:]))
                if c[:6] == "image:":
                    rv = f.call('image-display', 0, 5, self.pathto(c[6:]))
                if c[:8] == "overlay:":
                    rv = f.call('image-display', 0, 2, self.pathto(c[8:]))
                if c[:9] == "overlayC:":
                    rv = f.call('image-display', self.w/6, self.h*3/4, self.pathto(c[9:]), (self.w*5/12, self.h/8))
                if c == "page-local":
                    page = self.find_pages(a['mark'])
                    self.clean_lines(page)
                    self.mark_lines(page)
                    cm = self.get_local_attr(a['mark'], "background", page)
                    if cm:
                        cmds.extend(cm.split(','))
                if rv != None:
                    ret |= rv
            return ret
        if key == "render-line-prev":
            # Go to start of page
            here = a['mark']
            if a['numeric'] == 0:
                # just make sure at start of line
                return self.parent.call("render-line-prev", here, 0)

            start = self.find_pages(here)
            if not start:
                return -2
            if start > here:
                start = here

            if self.marks_same(start, here):
                return -2
            here.to_mark(start)
            return 1

        if key == "render-line":
            here = a['mark']
            cb = a['comm2']
            page = self.find_pages(here)
            if not page:
                # No pages at all
                cb("callback", self)
                return 1

            if here < page:
                here.to_mark(page)

            self.clean_lines(page)
            self.mark_lines(page)

            end = page.next()

            line = None
            while end is None or here < end:
                lines = []
                self.parent.call("render-line", here,a['numeric'],
                                 lambda key2, **aa: take('str', lines, aa))
                if len(lines) == 0 or lines[0] is None:
                    line = None
                    break
                line = lines[0]
                if line[0] == ':':
                    #skip attributes
                    continue
                line = line.strip("\n")
                break

            if line is None:
                cb("callback", self)
            else:
                mode = 'P'
                prefix = None
                for pf in self.paras:
                    if pf == None and line == "":
                        mode = self.paras[pf]
                        break
                    if (pf and (prefix is None or len(pf) > len(prefix)) and
                        pf == line[0:len(pf)]):
                        mode = self.paras[pf]
                        prefix = pf

                if prefix:
                    line = line[len(prefix):]
                if type(mode) == dict:
                    # look up type of previous line.
                    pmode = None
                    if here.prev() is not None:
                        pmode = here.prev()['mode']
                    if pmode in mode:
                        mode = mode[pmode]
                    else:
                        mode = mode[None]
                here['mode'] = mode
                v = self.get_attr(here, mode, page)

                if mode == 'IM':
                    width=200; height=100
                    if len(line) > 1 and line[0].isdigit():
                        try:
                            c = line.index(':')
                            width = int(line[:c])
                        except:
                            c = -1

                        line = line[c+1:]
                        try:
                            c = line.index(':')
                            height = int(line[:c])
                        except:
                            c = -1
                        line = line[c+1:]

                    cb("callback", self, "<image:"+self.pathto(line)+",width:%d,height:%d>"%(width,height))
                    return 1

                line = re.sub("\*([A-Za-z0-9][^*<]*)\*", "<italic>\\1</>", line)
                line = re.sub("`([/A-Za-z0-9][^*<]*)`", "<family:mono>\\1</>", line)
                b = re.match(".*,bullet:([^:,]*)", v)
                if b:
                    vb = self.get_attr(here, 'bullet', page)
                    if vb:
                        bl = "<%s>%s</>" % (vb, b.group(1))
                    else:
                        bl = b.group(1)
                    line = bl +"<"+v+">"+ line + "</>"
                else:
                    line = "<"+v+">"+ line + "</>"
                line += '\n'
                if end and (here > end or self.marks_same(here,end)):
                    line += '\f'
                cb("callback", self, line)
            return 1

        if key == "Notify:Replace":
            m = a['mark']
            # A change has happened at 'm'.  The following page might not
            # be valid, and the previous may not be valid or have next-valid.
            # If no previous, self.first_valid may not be.
            page = self.prev_page(m)
            if not page:
                # m is before first page
                page = self.first_page()
                if page:
                    self.first_valid = False
                    page['valid'] = 'no'
                # attributes probably changed so...
                self.call("render-lines:redraw")
            else:
                page['valid'] = 'no'
                page['next-valid'] = 'no'
                page = page.next()
                if page:
                    page['valid'] = 'no'

            l = self.prev_line(m)
            if l:
                if l['type'] and l['type'][0:5] == "attr:":
                    self.call("render-lines:redraw")
                l['type'] = 'unknown'
                l = l.next()
                if l:
                    l['type'] = 'unknown'
            return 1

        if key == "Notify:doc:Recentre":
            mark = a['mark']
            if a['numeric'] == 2:
                # Move 'mark' to start of next page
                m = self.find_page(mark)
                if m:
                    mark.to_mark(m)
            self.final.call("Move-View-Pos", mark)
            return 1

        if key == "Close":
            # destroy all marks
            self.release()

        if key == "Clone":
            # Need to create a new PresenterPane I guess, then recurse on children
            pass

        if key == "M-Chr-f":
            if self.borderless:
                self.call("Window:border", 1)
                self.call("Display:border", 1)
                self.call("Display:fullscreen", -1)
                self.borderless = False
            else:
                self.call("Window:border", -1)
                self.call("Display:border", -1)
                self.call("Display:fullscreen", 1)
                self.borderless = True
            return 1

        if key == "Move-View-Large":
            p = a['mark']
            page = self.find_pages(p)
            if a['numeric'] < 0:
                page = page.prev()
            else:
                page = page.next()
            if page is not None:
                p.to_mark(page)
                a['focus'].call("Move-View-Pos", page)
                a['focus'].damage(edlib.DAMAGED_CURSOR)
            return 1

        return None

class MarkdownPane(edlib.Pane):
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus, self.handle)

    def handle(self, key, focus, **a):
        # Refresh causes presentation page to recenter
        # page-down just moves down to start of next page.
        if key == "Display:refresh":
            focus.call("Notify:doc:Recentre", a['mark'])
            return 0
        if key == "Move-View-Large" and a['numeric'] >= 0:
            if focus.call("Notify:doc:Recentre", a['mark'], 2) > 0:
                return 1
        return 0

def present_attach(key, focus, comm2, **a):
    p = focus.render_attach("lines")
    p = PresenterPane(p)
    p['render-wrap'] = 'no'
    p['background'] = 'color:yellow'
    p['hide-cursor'] = 'yes'

    p.call("Request:Notify:Replace")
    p.call("Request:Notify:doc:Recentre")
    while p.focus:
        p = p.focus
    comm2("callback", p)
    return 1

def markdown_attach(key, focus, comm2, **a):
    p = focus.render_attach("lines")
    p = MarkdownPane(p)
    comm2("callback", p)
    return 1

def markdown_appeared(key, focus, **a):
    n = focus["filename"]
    if n and n[-3:] == ".md":
        focus.call("doc:attr-set", "render-Chr-P", "present")
        focus.call("doc:attr-set", "render-default", "markdown")
    return 1

editor.call("global-set-command", pane, "attach-render-markdown", markdown_attach)
editor.call("global-set-command", pane, "attach-render-present", present_attach)
editor.call("global-set-command", pane, "doc:appeared-markdown", markdown_appeared)
