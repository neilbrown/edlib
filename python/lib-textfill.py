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
# This first version only has limited fill-paragragh.
# Two marks are provided and the text between them is re-filled.
# Any text between the first mark and the start of that line is
# optionally a prefix.  If any other line starts with the same string,
# it is stripped off, to be re-added (to all lines) after filling.
# If the prefix doesn't appear, or is all spaces, then the shortest
# existing space-prefix on para-lines is used as a prefix.

import re

class FillMode(edlib.Pane):
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus)

    def do_fill(self, key, focus, num, mark, mark2, **a):
        "handle:fill-paragraph"
        if not mark or not mark2:
            return edlib.Enoarg

        if num and num > 8:
            width = num
        else:
            width = 72

        if mark2 < mark:
            mark, mark2 = mark2, mark

        m = mark.dup()
        focus.call("Move-EOL", -1, m)
        prefix = focus.call("doc:get-str", m, mark, ret='str')
        para = focus.call("doc:get-str", mark, mark2, ret='str')
        lines = para.splitlines()
        if len(lines) == 0:
            return 1

        # all spaces
        plen = len(prefix)
        use_prefix = False
        if plen:
            first = True
            for l in lines:
                if prefix == l[:len(prefix)]:
                    use_prefix = True
                p = len(l) - len(l.lstrip())
                if p < plen and not first:
                    plen = 0
                first = False
        if prefix.lstrip() == '':
            use_prefix = False
        else:
            plen = len(prefix)
        words = []
        for l in lines:
            if prefix == l[:len(prefix)]:
                l = l[len(prefix):]
            words.extend(re.split(r'\s+', l.strip()))
        if len(words) < 2:
            return

        # we have the words, time to assemble the lines
        # first line never gets prefix
        newpara = ""
        ln= ""
        lln = len(prefix)
        if not use_prefix:
            prefix = ' '*plen

        pfx = ''
        for w in words:
            spaces = 1
            if ln and ln[-1] == '.':
                # 2 spaces after a sentence
                spaces = 2
            if ln and lln + spaces + len(w) > width:
                # time for a line break
                newpara += pfx + ln + '\n'
                ln = ''
                lln = 0
                pfx = prefix
            if ln:
                ln += ' ' * spaces
                lln += 1
            ln += w
            lln += len(w)
        newpara += pfx + ln
        if para[-1] == '\n':
            newpara += '\n'
        if newpara != para:
            focus.call("doc:replace", 1, mark, newpara, 1, mark2)
        return 1

def fill_mode_attach(key, focus, comm2, **a):
    p = FillMode(focus)
    if comm2:
        comm2("callback", p)
    return 1

editor.call("global-set-command", "attach-textfill", fill_mode_attach)
