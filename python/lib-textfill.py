# -*- coding: utf-8 -*-
# Copyright Neil Brown (c)2019-2022 <neil@brown.name>
# May be distributed under terms of GPLv2 - see file:COPYING
#
#
# This pane supports two related text filling functions.
# - it captures text entry and fills the current line when it gets
#   too long
# - it support a fill-paragraph function to reformat a paragraph to
#   fit a given width.
#
# If two marks are provided the text between them is re-filled.  Any
# text between the first mark and the start of that line is used to supply
# characters for a prefix.  Any characters in that prefix, and any white
# space, are striped from the start of every other line.  If there is a
# second line, the text stripped from there is prefixed to all lines of
# the reformatted paragraph.  If there is no second line, then prefix
# provided is the prefix of the first line with all non-tabs replaced with
# spaces.
#
# If only one mark is provided, a paragraph is found bounded by
# lines that contain no alpha-numerics.  A prefix for the first line
# is then chosen as the maximal set of non-alphanumerics non-quote
# characters.

import re

def span(line, chars):
    s = ''
    for c in line:
        if c in chars:
            s += c
        else:
            break
    return s

def textwidth(line):
    w = 0
    for c in line:
        if c == '\t':
            w = w | 7
        w += 1
    return w

def do_replace(focus, start, end, new, strip):
    # text between 'start' and 'end' matches 'new' except for
    # space/tab/newline and chars in 'strip'
    # Edit between 'start' and 'end' to make it match 'new'.
    # We do edits rather than wholesale replace so that marks
    # can remain unmoved.
    # If we get confused, just do a wholesale replacement of the
    # remainder.

    second = 0
    while start < end and new:
        c = focus.following(start)
        if c == new[0]:
            # a match, just skip it
            new = new[1:]
            focus.next(start)
            continue
        if c in ' \t\n' or c in strip:
            # maybe this got removed
            s = start.dup()
            focus.next(start)
            focus.call("doc:replace", 0, second, s, start, "")
            second=1
            continue
        repl = ''
        while new and (new[0] in ' \t\n' or new[0] in strip):
            # probably this was inserted
            repl += new[0]
            new = new[1:]
        if repl:
            s = start.dup()
            focus.call("doc:replace", 0, second, s, start, repl)
            second = 1
            continue
        # Nothing obvious to do, just bale out
        break
    if start < end or new:
        focus.call("doc:replace", 0, second, start, new, end)

def reformat(lines, lln,  width, tostrip, prefix):
    # 'lines' is an array of lines
    # 'lln' is the length of the first line which *isn't* included
    #   in the lines array.  It is a prefix not to be touched.
    # 'tostrip' is characters that can be removed from start of lines
    #    including space and tab
    # 'prefix' is to be added to the start of each line but the first
    # 'width' is the max final width of each line
    #

    plen = textwidth(prefix)

    words = []
    for l in lines:
        p = span(l, tostrip)
        l = l[len(p):]
        words.extend(re.split(r'\s+', l.strip()))

    # we have the words, time to assemble the lines
    # first line never gets prefix
    newpara = ""
    ln = ""

    pfx = ''
    for w in words:
        spaces = 1
        if ln and ln[-1] in '.?!':
            # 2 spaces after a sentence
            spaces = 2
        if ln and lln + spaces + len(w) > width:
            # time for a line break
            newpara += pfx + ln + '\n'
            ln = ""
            lln = plen
            pfx = prefix
        if ln:
            ln += ' ' * spaces
            lln += spaces
        ln += w
        lln += len(w)

    return newpara + pfx + ln

def find_start(focus, mark):
    mark = mark.dup()
    m = mark.dup()
    re = focus.call("doc:get-attr", mark, "fill:start-re", ret='str')
    if not re:
        re = focus['fill:start-re']
    if not re:
        re = "^[^a-zA-Z0-9\n]*$"
    focus.call("doc:EOL", -100, m)
    try:
        leng = focus.call("text-search", re, mark, m, 1, 1)
        # leng is length + 1, we want +1 to kill '\n'
        focus.call("Move-Char", leng, mark)
    except edlib.commandfailed:
        if focus.prior(m) != None:
            # Went back 100 lines and found no suitable para-separator line
            return None
        mark.to_mark(m)
    # mark is at start of para - not indented yet.

    # Now choose a prefix, which is non-alphanum or quotes.
    # Possibly open brackets should be included too?
    l = focus.call("text-match", "^[^a-zA-Z0-9'\"\n]*", mark.dup())
    while l > 1:
        focus.next(mark)
        l -= 1
    return mark

def find_end(focus, mark):
    m = mark.dup()
    focus.call("doc:EOL", 100, m)
    re = focus.call("doc:get-attr", mark, "fill:end-re", ret='str')
    if not re:
        re = focus['fill:end-re']
    if not re:
        re = "^[^a-zA-Z0-9\n]*$"
    try:
        leng = focus.call("text-search", re, mark, m, 1)
        focus.call("Move-Char", -leng, mark)
    except edlib.commandfailed:
        if focus.following(m) != None:
            return edlib.Efail
        mark.to_mark(m)
    return mark

def get_prefixes(focus, mark, lines):
    # Get the text on the line before 'mark' - the prefix of the line
    # Then based on that, get a prefix of the second line, to be used
    # on other lines. And also of the last line, to be used when
    # adding a single line to a para.
    # If there is no second line, use first prefix, but with non-space
    # converted to space.

    m = mark.dup()
    focus.call("doc:EOL", -1, m)
    p0 = focus.call("doc:get-str", m, mark, ret='str')

    if len(lines) == 1:
        # only one line, so prefix is all spaces but based on first line
        prefix = focus.call("doc:get-attr", "fill:default-prefix",
                            m, ret='str')
        if not prefix:
            prefix = focus['fill:default-prefix']
        if not prefix:
            prefix = ""
            # When a single line is being wrapped, all of these
            # characters in the prefix are preserved for the prefix of
            # other lines.
            repeating_prefix = focus.call("doc:get-attr", "fill:repeating-prefix",
                                          m, ret='str')
            if not repeating_prefix:
                repeating_prefix = focus['fill:repeating-prefix']
            if not repeating_prefix:
                repeating_prefix=''
            repeating_prefix += ' \t'

            for c in p0:
                if c in repeating_prefix:
                    prefix += c
                else:
                    prefix += ' '
        prefix_last = prefix
    else:
        prefix = span(lines[1], p0 + ' \t')
        prefix_last = span(lines[-1], p0 + ' \t')
    return (p0, prefix, prefix_last)

class FillMode(edlib.Pane):
    def __init__(self, focus, colsarg=None):
        edlib.Pane.__init__(self, focus)
        cols = focus['fill-width']
        if cols:
            # auto-fill requested
            cols = int(cols)
        elif colsarg:
            # auto-fill explicitly requested
            cols = colsarg
        else:
            # don't auto-fill
            cols = None
        if cols:
            self.call("doc:set:fill-width", "%d" % cols)
        self.cols = cols

    def handle_clone(self, key, focus, **a):
        "handle:Clone"
        p = FillMode(focus, self.cols)
        self.clone_children(p)
        return 1

    def do_fill(self, key, focus, num, mark, mark2, **a):
        "handle:fill-paragraph"
        if not mark or not mark2:
            # find the paragraph: between two non-alphanum lines.
            if not mark:
                mark = mark2
            if not mark:
                mark = focus.call("doc:point", ret='mark')
            if not mark:
                return edlib.Enoarg
            mark = find_start(focus, mark)
            if not mark:
                focus.call("Message", "Cannot find start of paragraph to fill")
                return edlib.Efalse
            mark2 = find_end(focus, mark.dup())

        if num != edlib.NO_NUMERIC and num > 8:
            width = num
            if self.cols:
                # if auto-filling, save new col width
                self.cols = num
                self.call("doc:set:fill-width", "%d" % num)
        else:
            width = 72
            if self.cols:
                width = self.cols

        if mark2 < mark:
            mark, mark2 = mark2, mark

        para = focus.call("doc:get-str", mark, mark2, ret='str')
        lines = para.splitlines()
        if len(lines) == 0:
            return 1

        (prefix0, prefix, prefix_last) = get_prefixes(focus, mark, lines)
        tostrip = prefix0 + ' \t'

        newpara = reformat(lines, textwidth(prefix0), width, tostrip, prefix)

        if para[-1] == '\n':
            newpara += '\n'
        if newpara != para:
            try:
                do_replace(focus, mark, mark2, newpara, tostrip)
            except edlib.commandfailed:
                pass
        return 1

    def enable_fill(self, key, focus, num, **a):
        "handle:interactive-cmd-fill-mode"
        v = focus['view-default']
        if not self.cols:
            self.cols = 72
            self.call("doc:set:fill-width", "72")
        if v and 'textfill'in v:
            return 1
        elif v:
            v = v + ',textfill'
        else:
            v = 'textfill'
        focus.call("doc:set:view-default", v)
        return 1

    def handle_space(self, key, focus, mark, **a):
        "handle-list/K- /K:Tab/K:Enter"
        if not self.cols:
            # auto-fill not enabled
            return edlib.Efallthrough
        if not mark:
            return edlib.Efallthrough
        next = focus.following(mark)
        if next and next not in ['\n','\t', ' ', '\f']:
            # not at end-of-word, don't auto-fill
            return edlib.Efallthrough
        m = mark.dup()
        focus.call("doc:EOL", -1, m)
        line = focus.call("doc:get-str", m, mark, ret='str')
        if textwidth(line) < self.cols:
            return edlib.Efallthrough

        # need to start a new line, so need a prefix.
        st = find_start(focus, m)
        if not st:
            # Cannot find para start, so just consider this line
            st = m
        para = focus.call("doc:get-str", st, mark, ret='str')
        lines = para.splitlines()
        if len(lines) == 0:
            return edlib.Efallthrough

        (prefix0, prefix1, prefix_last) = get_prefixes(focus, st, lines)

        if textwidth(line) == self.cols:
            # just insert a line break
            try:
                focus.call("doc:replace", 1, mark, mark, "\n"+prefix_last)
                return 1
            except edlib.commandfailed:
                return edlib.Efallthrough

        # Need to reformat the current line.  Skip over prefix chars at
        # start of line.
        p = ""
        while focus.following(m) in prefix0 + ' \t':
            p += focus.next(m)
        lines = [ focus.call("doc:get-str", m, mark, ret='str') ]
        newpara = reformat(lines, textwidth(p), self.cols, prefix0+' \t',
                           prefix_last)
        if newpara != lines[0]:
            try:
                do_replace(focus, m, mark, newpara, prefix0+' \t')
            except edlib.commandfailed:
                pass
            if key == ':Enter':
                return 1
        return edlib.Efallthrough

def fill_mode_attach(key, focus, comm2, **a):
    # enable fill-paragraph, but don't auto-fill
    p = FillMode(focus)
    if comm2:
        comm2("callback", p)
    return 1

def fill_mode_activate(key, focus, comm2, **a):
    # enable fill-paragraph and auto-fill at col 72
    FillMode(focus, 72)

    v = focus['view-default']
    if v:
        v = v + ',textfill'
    else:
        v = 'textfill'
    focus.call("doc:set:view-default", v)
    return 1

editor.call("global-set-command", "attach-textfill", fill_mode_attach)
editor.call("global-set-command", "interactive-cmd-fill-mode",
            fill_mode_activate)
