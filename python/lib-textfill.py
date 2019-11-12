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
# This version only has fill-paragragh.
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


class FillMode(edlib.Pane):
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus)

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
            mark2 = m
            mark2.to_mark(mark)
            m = mark2.dup()
            focus.call("Move-EOL", 100, m)
            try:
                leng = focus.call("text-search", "^[^a-zA-Z0-9\n]*$",
                                  mark2, m, 1)
                focus.call("Move-Char", -leng, mark2)
            except edlib.commandfailed:
                if focus.call("doc:step", 1, m, ret='char') != None:
                    return edlib.Efail
                mark2.to_mark(m)

            # Now choose a prefix, which is non-alphanum or quotes.
            # Possibly open brackets should be included too?
            l = focus.call("text-match", "^[^a-zA-Z0-9'\"\n]*", mark.dup())
            while l > 1:
                focus.call("doc:step", 1, 1, mark)
                l -= 1

        if num != edlib.NO_NUMERIC and num > 8:
            width = num
        else:
            width = 72

        if mark2 < mark:
            mark, mark2 = mark2, mark

        m = mark.dup()
        focus.call("Move-EOL", -1, m)
        prefix0 = focus.call("doc:get-str", m, mark, ret='str')
        para = focus.call("doc:get-str", mark, mark2, ret='str')
        lines = para.splitlines()
        if len(lines) == 0:
            return 1

        tostrip = prefix0 + ' \t'
        if len(lines) == 1:
            # only one line, so prefix is all spaces but based on first line
            prefix = ""
            for c in prefix0:
                if c == '\t':
                    prefix += c
                else:
                    prefix += ' '
        else:
            prefix = span(lines[1], tostrip)

        plen = textwidth(prefix)
        plen0 = textwidth(prefix0)

        words = []
        for l in lines:
            p = span(l, tostrip)
            l = l[len(p):]
            words.extend(re.split(r'\s+', l.strip()))

        if len(words) < 2:
            return

        # we have the words, time to assemble the lines
        # first line never gets prefix
        newpara = ""
        ln = ""
        lln = plen0

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
        newpara += pfx + ln
        if para[-1] == '\n':
            newpara += '\n'
        if newpara != para:
            try:
                focus.call("doc:replace", 1, mark, newpara, mark2)
            except edlib.commandfail:
                pass
        return 1

def fill_mode_attach(key, focus, comm2, **a):
    p = FillMode(focus)
    if comm2:
        comm2("callback", p)
    return 1

editor.call("global-set-command", "attach-textfill", fill_mode_attach)
