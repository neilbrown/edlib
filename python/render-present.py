# -*- coding: utf-8 -*-
# Copyright Neil Brown ©2015-2021 <neil@brown.name>
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
# - an empty line is blank (BL)
# - other lines are text (P)
#
# A level-1 heading (H1) starts a new page. attribute lines immediately preceding
# it are part of the page and apply only to the page
# Attribute lines before the first page apply to whole document but can be over-ridden
# in a page.
#
# Within text *word* becomes bold (b), _word_ is italic (i) `word` is monospaced (m)
#
# attributes can be defined for paragraph types (H1,h2,I,L1,L1c,L2,L2c,C,P,BL) and
# text types ('bold','italic','mono','bullet').  Possible attributes for para or
# text are:
#  normal oblique italic bold small-caps large NN family:fontfamily
#  fg:colour bg:colour inverse bullet:char
# Possible attributes for para only:
#   left:size right:size centre space-after:size space-before:size
#
# L1c and L2c are continuation paragraphs of a list item.  They typically
# don't specify a bullet.  The text type "bullet" adds attrs to the bullet
# character chosen for the paragraph type.
#
# THIS IS NOT TRUE
# All the above can also be given for "default" and apply before other defaults and
# global/local setting.  "default" can also be given:
#   xscale:size yscale:size
#
# All sizes are in "points".  A "point" is either the height of the window divided by
# the yscale size, or the width divided by xscale size, whichever is smaller.
#
# global defaults are NOT (see "defaults =" below):


default_attrs = "normal 10 family:sans fg:black bg:white left:5 space-after:1 space-before:1 scale:400x200"

# We keep two sets of marks in document for guiding the presentation view.
# self.pageview provides a mark at the start of each page.  Each mark has two attributes
#    'valid' and 'next-valid' which can be 'yes' or 'no'.
# self.attrview provides a mark at the start of each line in the currently displayed page.
#    Mark attributes are:
#       'type' 'attr:foo' or 'text' or 'unknown' ('unknown' set when Replace modification happens).
#       'value'  attribute contents
#       'mode'  para-mode for the text
# When change happens, type changed to 'unknown' which triggers self.mark_lines() to
# reparse some of the page.

import re
import os

class PresenterPane(edlib.Pane):
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus)
        self.globals = {}
        self.pageview = focus.call("doc:add-view", self) - 1
        self.attrview = focus.call("doc:add-view", self) - 1
        self.borderless = False
        self.first_valid = False
        self.lines_damaged = True
        self.call("doc:request:doc:replaced")
        self.call("doc:request:doc:Recentre")

        self['render-wrap'] = 'no'
        self['background'] = 'color:yellow'
        self['hide-cursor'] = 'yes'

    def first_page(self):
        return self.vmarks(self.pageview)[0]

    def first_line(self):
        return self.vmarks(self.attrview)[0]

    def prev_page(self, m):
        return self.vmark_at_or_before(self.pageview, m)

    def prev_line(self, m):
        return self.vmark_at_or_before(self.attrview, m)

    def get_line_at(self, m):
        # call render-line at m
        try:
            s = self.parent.call("doc:render-line", m, -1, ret = 'str')
        except:
            s = ''
        return s if s else ''

    def get_line_before(self, m):
        m2 = m.dup()
        ret = self.parent.call("doc:render-line-prev", m2, 1)
        if ret <= 0:
            return None
        l = self.get_line_at(m2)
        return l

    def print_line_at(self, m):
        x = m.dup()
        l = self.get_line_at(x)
        return l[0:-1]

    def find_page(self, start, sof = False):
        # Assuming 'm' is either start-of-file (if 'sof') or the start of
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
                        self.leaf.call("view:changed")
                    return maybe
            else:
                # not part of start-of-page block
                skipping = False
                maybe = None
                if globals is not None:
                    globals.update(extra)
                    extra = {}

    def check_start(self, start):
        # This was the start of a page, but might not be any more.  Must check.
        # Following lines must be ":attr:" or "# ".
        # Preceding line, if any must not be ":attr:"
        # and must end at 'start'
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
        if m != start:
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
                        pm.release()
                    elif pm == first:
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
                    if next == pm.next():
                        pm['next-valid'] = 'yes'
                    elif next < pm.next():
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
        while first and (first < page or (next and first >= next)):
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
            if type(mode) == dict:
                # look up type of previous line.
                pmode = "-None-"
                if mark.prev() is not None:
                    pmode = mark.prev()['mode']
                if pmode in mode:
                    mode = mode[pmode]
                else:
                    mode = mode["-None-"]
                mark['prev'] = pmode
            else:
                mark['prev'] = None
            mark['mode'] = mode
            if prefix:
                line = line[len(prefix):]
            mark['value'] = line.strip('\n')

    def mark_lines(self, page):
        first = self.first_line()
        if first and not self.lines_damaged:
            return
        self.lines_damaged = False
        # There are no lines marked, or some are 'unknown'
        next = page.next()
        if not first or first != page:
            # no first mark, or it has been moved off page start
            first = edlib.Mark(self, self.attrview)
            first.to_mark(page)
        else:
            while first and first['type'] != 'unknown':
                first = first.next()
        # first is now the start of a line we need to parse.
        if not first:
            return

        # set extra_change if some line that isn't 'unknown' gets changed
        extra_change = False
        while not next or first < next:
            # There is a line there that we care about - unless EOF
            this = first.dup()
            l = self.get_line_at(this)
            if not l:
                break
            if first['type'] != 'unknown':
                extra_change = True
            self.annotate(first, l)
            while first.next() and first.next() < this:
                # first.next() is within the line just rendered
                first.next().release()
                extra_change = True
            if first.next() and first.next() == this:
                first = first.next()
                while first and first['type'] != 'unknown' and (
                        first['prev'] == None or first['prev'] == first.prev()['mode']):
                    first = first.next()
                if not first:
                    break
            else:
                first = edlib.Mark(orig=first)
                first.to_mark(this)

        if first:
            while first.next():
                extra_change = True
                first.next().release()
            first.release()
        if extra_change:
            # force full refresh
            self.leaf.call("view:changed")

    def get_local_attr(self, m, attr, page):
        t = 'attr:' + attr
        l = self.prev_line(m)
        while l:
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
        '     ': {'L2':'L2c', 'L2c':'L2c', 'L1':'L1c', 'L1c': 'L1c', '-None-': 'C'},
        '    ' : {'L2':'L1c', 'L2c':'L1c', 'L1':'L1c', 'L1c': 'L1c', '-None-': 'C'},
        ' '    : {'L2':'L1c', 'L2c':'L1c', 'L1':'L1c', 'L1c': 'L1c', '-None-': 'P'},
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

    def handle_present_bg(self, key, focus, mark, **a):
        "handle-prefix:Present-BG:"
        cmds = key[11:].split(',')
        ret = 0
        for c in cmds:
            rv = None
            if c[:6] == 'color:':
                rv = focus.call('Draw:clear', 'bg:' + c[6:])
            if c[:14] == "image-stretch:":
                rv = focus.call('Draw:image', 1, "file:" + self.pathto(c[14:]))
            if c[:6] == "image:":
                rv = focus.call('Draw:image', 0, 4+1, "file:" + self.pathto(c[6:])) # centre
            if c[:8] == "overlay:":
                rv = focus.call('Draw:image', 0, 0+2, "file:" + self.pathto(c[8:])) # top right
            if c == "page-local":
                page = self.find_pages(mark)
                self.clean_lines(page)
                self.mark_lines(page)
                cm = self.get_local_attr(mark, "background", page)
                if cm:
                    cmds.extend(cm.split(','))
            if rv:
                ret |= rv
        return ret

    def handle_clip(self, key, mark, mark2, num, **a):
        "handle:Notify:clip"
        self.clip(self.attrview, mark, mark2, num)
        self.clip(self.pageview, mark, mark2, num)
        return edlib.Efallthrough

    def handle_render_prev(self, key, mark, num, **a):
        "handle:doc:render-line-prev"
        # Go to start of page
        if num == 0:
            # just make sure at start of line
            return self.parent.call("doc:render-line-prev", mark)

        start = self.find_pages(mark)
        if not start:
            return edlib.Efalse
        if start.seq > mark.seq:
            start = mark

        if start == mark:
            return edlib.Efail
        mark.to_mark(start)
        return 1

    def handle_render_line(self, key, focus, mark, comm2, **a):
        "handle:doc:render-line"
        page = self.find_pages(mark)
        if not page:
            # No pages at all
            return edlib.Efail

        if mark.seq < page.seq:
            mark.to_mark(page)

        self.clean_lines(page)
        self.mark_lines(page)

        end = page.next()

        line = None
        linemark = None
        while end is None or mark < end:
            if not end and focus.prior(mark) is None:
                break
            linemark = self.prev_line(mark)
            if not linemark:
                break
            if linemark.next():
                mark.to_mark(linemark.next())
            elif end:
                mark.to_mark(end)
            else:
                self.call("doc:set-ref", 0, mark)

            if linemark['type'] != 'text':
                #skip attributes
                continue
            line = linemark['value']
            break

        if line is None:
            return edlib.Efail
        else:
            mode = linemark['mode']
            line = linemark['value']
            prefix = linemark['prefix']
            if prefix:
                line = line[len(prefix):]
            v = self.get_attr(mark, mode, page)

            # leading spaces will confuse 'centre', and using spaces for formatting
            # is to be discouraged.  Hard spaces can still be used when needed.
            line = line.strip(' ')
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

                comm2("callback", self, "<image:file:"+self.pathto(line)+",width:%d,height:%d>\n"%(width,height))
                return 1

            vb = self.get_attr(mark, 'mono', page)
            if not vb:
                vb = 'family:mono'
            line = re.sub("`(\\S[^`<]*)`", "<"+vb+">\\1</>", line)

            vb = self.get_attr(mark, 'italic', page)
            if not vb:
                vb = 'italic'
            line = re.sub("\\b_(\\B[^_<]*)_", "<"+vb+">\\1</>", line)

            vb = self.get_attr(mark, 'bold', page)
            if not vb:
                vb = 'bold'
            line = re.sub("\\*(\\S[^*<]*)\\*", "<"+vb+">\\1</>", line)
            b = re.match(".*,bullet:([^:,]*)", v)
            if b:
                vb = self.get_attr(mark, 'bullet', page)
                if vb:
                    bl = "<%s>%s</>" % (vb, b.group(1))
                else:
                    bl = b.group(1)
                line = bl +"<"+v+">"+ line + "</>"
            else:
                line = "<"+v+">"+ line + "</>"
            if end and mark >= end:
                line += '\f'
            else:
                line += '\n'
            comm2("callback", self, line)
        return 1

    def handle_notify_replace(self, key, mark, **a):
        "handle:doc:replaced"
        # A change has happened at 'mark'.  The following page might not
        # be valid, and the previous may not be valid or have next-valid.
        # If no previous, self.first_valid may not be.
        if mark:
            page = self.prev_page(mark)
        else:
            page = None
        if not page:
            # mark is before first page
            page = self.first_page()
            if page:
                self.first_valid = False
                page['valid'] = 'no'
            # attributes probably changed so...
            self.leaf.call("view:changed")
        else:
            page['valid'] = 'no'
            page['next-valid'] = 'no'
            page = page.next()
            if page:
                page['valid'] = 'no'
        if mark:
            l = self.prev_line(mark)
        else:
            l = None

        if l:
            self.lines_damaged = True
            if l == mark and l.prev():
                l['type'] = 'unknown'
                l = l.prev()
            if l['type'] and l['type'][0:5] == "attr:":
                self.leaf.call("view:changed")
            l['type'] = 'unknown'
            l = l.next()
            if l:
                l['type'] = 'unknown'
        return 1

    def handle_recentre(self, key, focus, mark, num, comm2, **a):
        "handle:doc:Recentre"
        m2 = edlib.Mark(self)
        m2.to_mark(mark)
        if num == 2:
            # Move 'm2' to start of next page
            m = self.find_page(m2)
            if m:
                m2.to_mark(m)
                if comm2 is not None:
                    comm2("callback", focus, m)
        if num == 3:
            # Move 'm2' to start of previous page
            m = self.find_pages(m2)
            if m:
                m = m.prev()
            if m:
                m2.to_mark(m)
                if comm2 is not None:
                    comm2("callback", focus, m)
        self.leaf.call("Move-View-Pos", m2)
        return 1

    def handle_clone(self, key, **a):
        "handle:Clone"
        # Need to create a new PresenterPane I guess, then recurse on children
        return 1

    def handle_M_f(self, key, **a):
        "handle:K:A-f"
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

    def handle_mvl(self, key, focus, mark, num, **a):
        "handle:Move-View-Large"
        # If mark isn't set, the movement might come
        # from scroll-bar or similar, ignore that.
        if mark:
            page = self.find_pages(mark)
            if num < 0:
                page = page.prev()
            else:
                page = page.next()
            if page is not None:
                mark.to_mark(page)
                focus.call("Move-View-Pos", page)
                return 1
        return 2

class MarkdownPane(edlib.Pane):
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus)

    def handle_clone(self, key, focus, **a):
        "handle:Clone"
        p = MarkdownPane(focus)
        self.clone_children(p)
        return 1

    def handle_refresh(self, key, focus, mark, **a):
        "handle:Display:refresh"
        # Refresh causes presentation page to recenter
        # page-down just moves down to start of next page.
        focus.call("doc:notify:doc:Recentre", mark)
        return edlib.Efallthrough

    def handle_mvl(self, key, focus, mark, num, **a):
        "handle:Move-View-Large"
        if num >= 0 and mark:
            m2 = mark.dup()
            if focus.call("doc:notify:doc:Recentre", m2, 2,
                          lambda key, **a: mark.to_mark(a['mark'])) > 0:
                return 1
        if num < 0 and mark:
            m2 = mark.dup()
            if focus.call("doc:notify:doc:Recentre", m2, 3,
                          lambda key, **a: mark.to_mark(a['mark'])) > 0:
                return 1
        return edlib.Efallthrough

def present_attach(key, focus, comm2, **a):
    p = PresenterPane(focus)

    if p:
        p2 = p.call("attach-viewer")
        if p2:
            p = p2
    if p:
        comm2("callback", p)
    return 1

def markdown_attach(key, focus, comm2, **a):
    p = MarkdownPane(focus)
    comm2("callback", p)
    return 1

def markdown_appeared(key, focus, **a):
    n = focus["filename"]
    if n and n[-3:] == ".md":
        focus["view-cmd-P"] = "present"
        focus["view-default"] = "markdown"
    return edlib.Efallthrough

editor.call("global-set-command", "attach-markdown", markdown_attach)
editor.call("global-set-command", "attach-present", present_attach)
editor.call("global-set-command", "doc:appeared-markdown", markdown_appeared)
