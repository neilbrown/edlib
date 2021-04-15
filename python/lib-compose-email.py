# -*- coding: utf-8 -*-
# Copyright Neil Brown ©2021 <neil@brown.name>
# May be distributed under terms of GPLv2 - see file:COPYING
#
# edlib module for composing email.
# Message is composed in a text document with some markers that
# cannot be edited that contain extra information. These include
# - end-of-headers marker
# - attachment markers
# - sign/encrypt request (maybe later)
# - signature marker
#
# Markers are a single line, are drawn by a special handler, and
# can only be editted by popping up a dialog pane.  They have a vmark
# at each end with attributes attached to the first.  The text of the line
# starts "@#!compose" so it can be re-marked when a view is opened.
#
# All text is utf-8, and is encoded if necessary when the message is
# sent.  "sending" causes the whole message to be encoded into a temporary
# file, which is then given to a configured program (e.g. /usr/sbin/sendmail)
# as standard-input.
# Text is encoded as quoted-printable if needed, and attachments are
# combined in a multipart/mixed.
#

class compose_email(edlib.Pane):
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus)
        self.view = focus.call("doc:add-view", self) - 1
        self.find_markers()
        m = self.call("doc:vmark-get", self.view, ret='mark')
        if not m:
            self.insert_header_mark()

    def find_markers(self):
        m = edlib.Mark(self)
        while True:
            try:
                r = self.parent.call("text-search", "^@#!compose:", m)
            except edlib.commandfailed:
                r = 0
            if r <= 0:
                break

            self.parent.call("Move-EOL", -1, m)
            m1 = edlib.Mark(self, self.view)
            m1.to_mark(m)
            self.parent.call("doc:set-attr", m,
                             "markup:func", "compose:markup-header")
            m2 = edlib.Mark(orig=m1)
            self.parent.call("Move-EOL", 1, m2, 1)
            s = self.parent.call("doc:get-str", m1, m2, ret='str')
            s = s[11:].strip()
            m1['compose-type'] = s.split(' ')[0]
            m.to_mark(m2)

    def insert_header_mark(self):
        # insert header at first blank line, or end of file
        m = edlib.Mark(self, self.view)
        r = self.call("text-search", "^$", m)
        if r <= 0:
            self.call("doc:set-ref", 0, m)
        m2 = edlib.Mark(orig=m)
        m2.step(0)
        self.parent.call("doc:replace", m2, m, "@#!compose:headers\n",
                         "/markup:func=compose:markup-header/")
        m2['compose-type'] = 'headers'

    def markup_header(self, key, focus, num, mark, mark2, comm2, **a):
        "handle:compose:markup-header"
        # Display cursor at start or end, anything else should be suppressed.
        if num == 0 or (mark2 and mark2 == mark):
            # appear at the start of the line
            return comm2("cb", focus, "<fg:red>")
        # at least go to end of line
        self.parent.call("Move-EOL", 1, mark)
        m = self.call("doc:vmark-get", self.view, mark, 3, ret='mark2')
        type = m['compose-type']
        if type == "headers":
            markup =  "<fg:red>Headers above, content below"
        else:
            markup = "<fg:cyan-40>[section: %s]" % type
        if (num == edlib.NO_NUMERIC or num < 0) and (not mark2 or mark2 > mark):
            # normal render - go past eol
            self.parent.next(mark)
            markup += "</>\n"
        return comm2("cb", focus, markup)

    def handle_clone(self, key, focus, **a):
        "handle:Clone"
        p = compose_email(focus)
        self.clone_children(p)
        return 1

    def handle_close(self, key, focus, **a):
        "handle:Close"
        m = self.call("doc:vmark-get", self.view, ret='mark')
        while m:
            m.release()
            m = self.call("doc:vmark-get", self.view, ret='mark')
        self.call("doc:del-view", self.view)

    def map_attr(self, key, focus, str, mark, comm2, **a):
        "handle:map-attr"
        if not str or not mark or not comm2:
            return edlib.Enoarg
        # get previous mark and see if it is here
        m = self.call("doc:vmark-get", self.view, 3, mark, ret='mark2')
        if not m and str == "start-of-line":
            # start of a header line - set colour for tag and header
            rv = self.call("text-match", "(\\s+|[!-9;-~]+\\s*:)", mark.dup())
            if rv > 0:
                # make space or tag light blue, and body dark blue
                # If tag is unknown, make it grey
                rv2 = self.call("text-match",
                                "?i:(from|to|cc|subject|in-reply-to):", mark.dup())
                if rv2 > 0:
                    comm2("cb", focus, mark, rv-1, "fg:blue+30", 2)
                else:
                    comm2("cb", focus, mark, rv-1, "fg:black+30", 2)
                comm2("cb", focus, mark, 100000, "fg:blue-70,bold", 1)
            else:
                # make whole line red
                comm2("cb", focus, mark, 100000, "bg:red+50,fg:red-50", 2)
        return edlib.Efallthrough

    def handle_replace(self, key, focus, mark, mark2, str, **a):
        "handle:doc:replace"
        if not mark:
            mark = focus.call("doc:point", ret='mark')
        if not mark2:
            mark2 = mark.dup()
            self.parent.call("doc:set-ref", mark2, 0)
        if not mark or not mark2:
            # something weird...
            return 1
        if mark2 < mark:
            mark, mark2 = mark2, mark

        m = self.call("doc:vmark-get", self.view, 3, mark, ret='mark2')
        if m and m['compose-type']:
            # must not edit a marker, but can insert a "\n" before
            if mark == mark2 and mark == m and str and str[-1] == '\n':
                # inserting newline at start
                m.step(1)
                return edlib.Efallthrough
            return 1
        if mark2 == mark:
            # not a delete, so should be safe as long as marks aren't before m
            if m:
                m.step(0)
            return edlib.Efallthrough
        m2 = self.call("doc:vmark-get", self.view, 3, mark2, ret='mark2')
        if m2 != m:
            # not completely within a part, so fail
            return 1
        # deleting/replacing something after m/m2.
        if m:
            m.step(0)
        return edlib.Efallthrough

    def handle_doc_step(self, key, focus, mark, num, num2, **a):
        "handle:doc:step"
        # if in a marker, only allow a space and newline to be seen
        if not mark:
            return edlib.Enoarg
        m = self.call("doc:vmark-get", self.view, 3, mark, ret='mark2')
        if mark == m:
            if m['compose-type']:
                # at start of marker
                if num > 0:
                    # forward
                    if num2:
                        self.parent.call("Move-EOL", 1, mark)
                    return ' '
                else:
                    # backward
                    return edlib.Efallthrough
            else:
                # at end of marker
                return edlib.Efallthrough
        if not m or not m['compose-type']:
            # not in a marker
            return edlib.Efallthrough
        # should be just before newline
        if num > 0:
            #forward, return newline
            if num2:
                self.parent.call("Move-EOL", 1, mark)
                self.parent.next(mark)
            return '\n'
        else:
            # backward, return space
            if num2:
                self.parent.call("Move-EOL", -1, mark)
            return ' '

    def handle_doc_get_attr(self, key, focus, mark, str, comm2, **a):
        "handle:doc:get-attr"
        if not mark or not str or not comm2 or not str.startswith("fill:"):
            return edlib.Efallthrough
        m = self.call("doc:vmark-get", self.view, 3, mark, ret='mark2')
        if m:
            return edlib.Efallthrough
        # in headers, need a min-prefix and start-re for fill
        if str == "fill:default-prefix":
            comm2("cb", focus, mark, "  ", str)
            return 1
        if str == "fill:start-re" or str == "fill:end-re" :
            comm2("cb", focus, mark, "^($|[^\\s])", str)
            return 1

def compose_mode_activate(key, focus, **a):
    focus['fill-width'] = '72'
    p = focus.call("attach-textfill", ret='focus')
    if not p:
        p = focus
    compose_email(p)
    return 1

editor.call("global-set-command", "interactive-cmd-compose-mode",
            compose_mode_activate)
