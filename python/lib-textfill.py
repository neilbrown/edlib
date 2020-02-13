# -*- coding: utf-8 -*-
# Copyright Neil Brown (c)2019 <neil@brown.name>
# May be distributed under terms of GPLv2 - see file:COPYING
#
#
# This pane supports two related text filling functions.
# - it captures text entry and fills the current line when it gets
#   too long
# - it support a fill-paragraph function to reformat a paragraph to
#   fit a given width.
#
# If two marks are provided and the text between them is re-filled.  Any
# text between the first mark and the start of that line is used to supply
# characters for a prefix.  Any characters in that prefix, and any white
# space, are striped from the start of every other line.  If there is a
# second line, the text stripped from there is prefixed to all lines of
# the reformatted paragraph.  if there is no second line, then prefix
# provides is the prefix of the first line with all non-tabs replaced with
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
        c = focus.call("doc:step", start, 1, 0, ret='char')
        if c == new[0]:
            # a match, just skip it
            new = new[1:]
            focus.call("doc:step", start, 1, 1)
            continue
        if c in ' \t\n' or c in strip:
            # maybe this got removed
            s = start.dup()
            focus.call("doc:step", start, 1, 1)
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
    # 'lln' is the lenth of the first line which *isn't* included
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
        if ln and ln[-1] == '.':
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
    focus.call("Move-EOL", -100, m)
    try:
        leng = focus.call("text-search", "^[^a-zA-Z0-9\n]*$",
                          mark, m, 1, 1)
        # leng is length + 1, we want +1 to kill '\n'
        focus.call("Move-Char", leng, mark)
    except edlib.commandfailed:
        if focus.call("doc:step", 0, m, ret='char') != None:
            return edlib.Efail
        mark.to_mark(m)
    # mark is at start of para - not indented yet.

    # Now choose a prefix, which is non-alphanum or quotes.
    # Possibly open brackets should be included too?
    l = focus.call("text-match", "^[^a-zA-Z0-9'\"\n]*", mark.dup())
    while l > 1:
        focus.call("doc:step", 1, 1, mark)
        l -= 1
    return mark

def find_end(focus, mark):
    m = mark.dup()
    focus.call("Move-EOL", 100, m)
    try:
        leng = focus.call("text-search", "^[^a-zA-Z0-9\n]*$",
                          mark, m, 1)
        focus.call("Move-Char", -leng, mark)
    except edlib.commandfailed:
        if focus.call("doc:step", 1, m, ret='char') != None:
            return edlib.Efail
        mark.to_mark(m)
    return mark

def get_prefixes(focus, mark, lines):
    # Get the text on the line before 'mark' - the prefix of the line
    # Then based on that, get a prefix of the second line, to be used
    # on other lines.
    # If there is no second line, use first prefix, but with non-space
    # converted to space.

    m = mark.dup()
    focus.call("Move-EOL", -1, m)
    p0 = focus.call("doc:get-str", m, mark, ret='str')

    if len(lines) == 1:
        # only one line, so prefix is all spaces but based on first line
        prefix = ""
        for c in p0:
            if c == '\t':
                prefix += c
            else:
                prefix += ' '
    else:
        prefix = span(lines[1], p0 + ' \t')
    return (p0, prefix)

class FillMode(edlib.Pane):
    def __init__(self, focus, cols=None):
        edlib.Pane.__init__(self, focus)
        if not cols:
            cols = focus['fill-width']
            if cols:
                cols = int(cols)
        self.cols = cols

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
            mark2 = find_end(focus, mark.dup())

        if num != edlib.NO_NUMERIC and num > 8:
            width = num
            if self.cols:
                self.cols = num
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

        (prefix0, prefix) = get_prefixes(focus, mark, lines)
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
        self.cols = 72
        return 1

    def handle_space(self, key, focus, mark, **a):
        "handle-list/KChr- /KTab/KEnter"
        if not self.cols:
            # auto-fill not enabled
            return 0
        if not mark:
            return 0
        next = focus.call("doc:step", mark, 1, 0, ret='char')
        if not next or next != '\n':
            # not at end-of-line, don't auto-fill
            return 0
        m = mark.dup()
        focus.call("Move-EOL", -1, m)
        line = focus.call("doc:get-str", m, mark, ret='str')
        if textwidth(line) < self.cols:
            return 0

        # need to start a new line, so need a prefix.
        st = find_start(focus, m)
        para = focus.call("doc:get-str", st, mark, ret='str')
        lines = para.splitlines()
        if len(lines) == 0:
            return 0

        (prefix0, prefix1) = get_prefixes(focus, st, lines)

        if textwidth(line) == self.cols:
            # just insert a line break
            try:
                focus.call("doc:replace", 1, mark, mark, "\n"+prefix1)
                return 1
            except edlib.commandfailed:
                return 0

        # Need to reformat the current line.  Skip over prefix chars at
        # start of line.
        p = ""
        while focus.call("doc:step", 1, 0, m, ret='char') in prefix0 + ' \t':
            p += focus.call("doc:step", 1, 1, m, ret='char')
        lines = [ focus.call("doc:get-str", m, mark, ret='str') ]
        newpara = reformat(lines, textwidth(p), self.cols, prefix0+' \t',
                           prefix1)
        if newpara != lines[0]:
            try:
                do_replace(focus, m, mark, newpara, prefix0+' \t')
            except edlib.commandfailed:
                pass
            if key == 'Enter':
                return 1
        return 0

def fill_mode_attach(key, focus, comm2, **a):
    p = FillMode(focus)
    if comm2:
        comm2("callback", p)
    return 1

def fill_mode_activate(key, focus, comm2, **a):
    FillMode(focus, 72)
    return 1

editor.call("global-set-command", "attach-textfill", fill_mode_attach)
editor.call("global-set-command", "interactive-cmd-fill-mode",
            fill_mode_activate)
