# -*- coding: utf-8 -*-
# Copyright Neil Brown (c)2018-2019 <neil@brown.name>
# May be distributed under terms of GPLv2 - see file:COPYING

def textwidth(line):
    w = 0
    for c in line:
        if c == '\t':
            w = w | 7
        w += 1
    return w

class CModePane(edlib.Pane):
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus)
        # for paren-highlight
        self.pre_paren = None
        self.post_paren = None

        # for indent
        self.spaces = None   # is set to a number, use spaces, else TABs
        self.indent_type = None

    def handle_clone(self, key, focus, **a):
        "handle:Clone"
        p = CModePane(focus)
        p.spaces = self.spaces
        p.indent_type = self.indent_type
        self.clone_children(p)
        return 1

    def mkwhite(self, align):
        rv = ''
        if not self.spaces:
            while align >= 8:
                align -= 8
                rv += '\t'
        return rv + ' ' * align

    def calc_indent_c(self, p, mark):
        # C indenting can be determined precisely, but it
        # requires a lot of work.
        # We need to go back to the start of the function
        # (or structure?) and work forwards
        # We need to:
        #  recognize labels (goto and case and default)
        #  Skip comments, but recognize when in one
        #  Skip {} sections
        #  If a line ends with ';' or '}' then it ends a statement,
        #  else next line is indented
        # The total indent is the nest-depth (of '{') plus one if not
        #  on the first line of a statement.
        # When inside a comment, set get an alignment of the '*' and
        # a prefix of '* '
        # When inside brackets '{[(' that start at end-of-line, indent
        # increments. If they start not at eol, they set an alignment.
        # 'eol' must be determined ignoring spaces and comments.
        #
        # So we find a zero-point, which is a non-label, non-comment, non-#
        # at start of line.
        # Then we move forward tracking the state - possibly recording at
        # each newline.
        # elements of state are:
        #  an array of depths which are either prev+self.spaces, or an alignment
        #  flag "start of statement"
        #  flag "label line"
        #  comment type none,line,block
        #  quote type none,single,double
        #  column
        #

        # 'zero' point is a start of line that is not empty, not
        #  white space, not #, not /, not }, and not "alphanum:"
        #  But don't accept a match on this line
        m = mark.dup()
        p.call("Move-eol", m, -1)
        p.call("doc:step", m, 0, 1)
        try:
            n = p.call("text-search", 1, 1, m,
                       "^([^\s\a\A\d#/}]|[\A\a\d_]+[\s]*[^:\s\A\a\d_]|[\A\a\d_]+[\s]+[^:\s])")
        except edlib.commandfailed:
            p.call("Move-file", m, -1)

        depth = [0]
        brackets = ""
        start_stat = True
        comment = None
        quote = None
        column = 0
        nextcol = 0
        open_col = 0
        comment_col = 0

        tab = self.spaces
        if not tab:
            tab = 8

        c = p.call("doc:step", 1, 0, m, ret='char')
        while m < mark:
            column = nextcol
            c = p.call("doc:step", 1, 1, m, ret='char')
            if c is None:
                break
            if c == '\n':
                nextcol = 0
            elif c == '\t':
                nextcol = (column|7)+1
            else:
                nextcol = column+1

            if quote:
                # check for close
                if c == quote:
                    quote = None
                elif c == '\\':
                    c = p.call("doc:step", 1, 0, m, ret='char')
                    if c == quote or c == '\\':
                        # skip this
                        c = p.call("doc:step", 1, 1, m, ret='char')
                        nextcol += 1

                continue
            elif comment == '//':
                # check for close
                if c == '\n':
                    comment = None
                    # fallthrough to newline handling
                else:
                    continue
            elif comment == '/*':
                # check for close
                if c != '*':
                    continue
                c = p.call("doc:step", 1, 0, m, ret='char')
                if c != '/':
                    continue
                p.call("doc:step", 1, 1, m)
                nextcol += 1
                comment = None
                continue

            # must be in code
            if c not in ' \t\n/':
                if start_stat:
                    start_stat = False
                    maybe_label = True
                if open_col:
                    depth.append(open_col)
                    open_col = 0
            if c == '\n':
                # end of line
                # if we haven't seen code since a group opened, then
                # that group indents at an extra next level, else it indents
                # at the opening column
                if open_col:
                    depth.append(depth[-1]+tab)
                    open_col = 0
                maybe_label = False
            elif c in '{([':
                brackets += c
                if c == '{':
                    start_stat = True
                open_col = column+1
            elif c in '})]':
                if c == '}':
                    start_stat = True
                if depth:
                    brackets = brackets[:-1]
                    depth.pop()
            elif c == ';':
                start_stat = True
            elif c == ':':
                # If this is a label, then starts-statement
                if maybe_label:
                    start_stat = True
                maybe_label = False
            elif c == '?':
                # could be ?: - probably not a label
                maybe_label = False
            elif c == '"' or c == "'":
                quote = c
            elif c == '/':
                c = p.call("doc:step", 1, 0, m, ret='char')
                if c == '/':
                    comment = '//'
                elif c == '*':
                    comment = '/*'
                    comment_col = column
                    p.call("doc:step", 1, 1, m)
                    nextcol += 1

        if open_col:
            depth.append(depth[-1]+tab)

        br = m.dup()
        c = p.call("doc:step", 1, 1, br, ret='char')
        while brackets and c and c in '\t }])':
            if c in '}])':
                brackets = brackets[:-1]
                depth.pop()
                if c == '}':
                    start_stat = True
            c = p.call("doc:step", 1, 1, br, ret='char')

        if not start_stat and brackets and brackets[-1] == '{':
            depth.append(depth[-1]+tab)

        #check for label
        st = m.dup()
        c = p.call("doc:step", 0, 1, st, ret='char')
        while c and c != '\n':
            c = p.call("doc:step", 0, 1, st, ret='char')
        p.call("doc:step", 1, 1, st)

        # assume exactly this depth
        depth.insert(0,0)
        ret = [ depth[-1], depth[-1] ]

        try:
            l = p.call("text-match", st,
                       '^[ \t]*(case\s[^:\n]*|default[^\A\a\d:\n]*|[_\A\a\d]+):')
        except edlib.commandfailed:
            l = 0
        if l > 0:
            if p.call("doc:step", 1, st, ret='char') in ' \t':
                label_line = "indented-label"
            else:
                try:
                    l = p.call("text-match", st, '^[ \t]*[_\A\a\d]+:')
                    label_line = "margin-label"
                except edlib.commandfailed:
                    label_line = "indented-label"
            depth.insert(0,0)
            if label_line == "margin-label":
                ret = [0, depth[-1]]
            else:
                ret = [depth[-1],depth[-1]]

        if comment == "/*":
            prefix = "* "
            ret = [comment_col+1,comment_col+1]
        else:
            prefix = ""
        return (ret, prefix)

    def calc_indent_python(self, p, m):
        # like calc-indent, but check for ':' and end of previous line
        indent = self.calc_indent(p, m, 'default')
        m = m.dup()
        c = p.call("doc:step", 0, 1, m, ret='char')
        while c in ' \t\n':
            c = p.call("doc:step", 0, 1, m, ret='char')
        if c == ':':
            t=self.spaces
            if not t:
                t=8
            indent[0].append(indent[0][-1]+t)
        return indent

    def calc_indent(self, p, m, type=None):
        # m is at the end of a line or start of next line in p - Don't move it
        # Find indent for 'next' line as a list of depths
        # and an alignment...

        if not type:
            if self.indent_type == 'C':
                return self.calc_indent_c(p, m)
            if self.indent_type == 'python':
                return self.calc_indent_python(p, m)

        m = m.dup()
        # Find previous line which is not empty and make up
        # a depth list from the leading tabs/spaces.
        while p.call("doc:step", 0, m, ret="char") in ' \t':
            p.call("doc:step", 0, 1, m)
        line_start = m.dup()
        while p.call("doc:step", 0, m, ret="char") == '\n':
            p.call("doc:step", 0, 1, m)
        if p.call("doc:step", 0, m) == edlib.WEOF:
            # No previous non-empty line.
            return ([0], None)
        # line before m is not empty
        m2 = m.dup()
        # walk to start of line, leaving m2 at leading non-space
        c = p.call("doc:step", 0, m2, ret="char")
        while c != None and c != '\n':
            p.call("doc:step", 0, 1, m2)
            if c not in " \t":
                m.to_mark(m2)
            c = p.call("doc:step", 0, m2, ret="char")
        # m2 .. m is the prefix
        pfx = p.call("doc:get-str", m2, m, ret = 'str')
        w = textwidth(pfx)
        if self.spaces:
            t = self.spaces
        else:
            t = 8
        r = [0]
        while w >= t:
            w -= t
            r.append(r[-1] + t)
        if w:
            r.append(r[-1] + w)

        # Now see if there are any parentheses.
        # i.e see if a () expression started since the indent.
        # If so, either line-up with open, or add a tab if open
        # is at end-of-line
        indent_end = m
        expr = line_start.dup()
        p.call("Move-Expr", -1, 1, expr)
        if expr >= indent_end:
            p.call("doc:step", expr, 1, 1)
            if p.call("doc:step", expr, 1, 0, ret="char") == '\n':
                # open-bracket at end-of-line, so add a standard indent
                extra = t
            else:
                extra = p.call("doc:get-str", indent_end, expr, ret='str')
                extra = len(extra)
            extra += r[-1]
            # A further indent than this really isn't appropriate, so
            # add it twice
            r.append(extra)
            r.append(extra)
        else:
            # allow one extra indent
            r.append(r[-1]+t)
        return (r, '')

    def handle_enter(self, key, focus, mark, **a):
        "handle:Enter"
        # If there is white space before or after the cursor,
        # remove it.  Then work out how indented this line
        # should be, and insert that much space, plus any
        # requested prefix
        m = mark.dup()
        # First, move point forward over any white space
        c = focus.call("doc:step", 1, 0, m, ret="char")
        while c and c in " \t":
            focus.call("doc:step", 1, 1, m)
            c = focus.call("doc:step", 1, 0, m, ret="char")
        focus.call("Move-to", m)
        # Second, move m back over any white space.
        c = focus.call("doc:step", 0, 1, m, ret="char")
        while c != None:
            if c not in " \t":
                focus.call("doc:step", 1, 1, m)
                break
            c = focus.call("doc:step", 0, 1, m, ret="char")
        # Now calculate the indent
        (depths,prefix) = self.calc_indent(focus, m)

        # and replace all that white space with a newline and the indent
        return focus.call("Replace", 1, m,
                          "\n" + self.mkwhite(depths[-2]) + prefix)

    def handle_tab(self, key, focus, mark, **a):
        "handle:Tab"
        # if there is only white-space before cursor (up to newline) then:
        # move to end of white-space
        # - choose an indent as for Return
        # - If we don't have exactly that, replace with that
        # - if we do have that, use the 'extra' indent level
        m = mark.dup()
        c = focus.call("doc:step", 0, 0, m, ret="char")
        prevc = c
        while c and c in " \t":
            focus.call("doc:step", 0, 1, m)
            c = focus.call("doc:step", 0, 0, m, ret="char")
        if not (c is None or c == "\n"):
            # not at start of line, maybe convert preceding spaces to tabs
            if prevc != ' ':
                return 0
            m = mark.dup()
            len = 0
            while focus.call("doc:step", 0, 0, m, ret='char') == ' ':
                len += 1
                focus.call("doc:step", 0, 1, m)
            new = "\t" * (len / 8)
            focus.call("Replace", 1, m, mark, new)
            # fall through it insert a new tab
            return 0

        # m at start-of-line, move mark (point) to first non-white-space
        c = focus.call("doc:step", 1, 0, mark, ret="char")
        while c and c in " \t":
            focus.call("doc:step", 1, 1, mark)
            c = focus.call("doc:step", 1, 0, mark, ret="char")

        (depths,prefix) = self.calc_indent(focus, m)

        new = self.mkwhite(depths[-2])

        current = focus.call("doc:get-str", m, mark, ret="str")

        if new != current:
            return focus.call("Replace", 1, m, mark, new)
        if depths[-1] == depths[-2]:
            # There is extra indent allowed, so stay where we are
            return 1

        new = self.mkwhite(depths[-1])
        return focus.call("Replace", 1, m, mark, new)

    def handle_bs(self, key, focus, mark, **a):
        "handle:Backspace"
        # If in the indent, remove one level of indent
        m = mark.dup()
        c = focus.call("doc:step", 1, m, ret="char")
        if c and c in " \t":
            # Not at end of indent, fall through
            return 0
        c = focus.call("doc:step", 0, 0, m, ret="char")
        while c and c in " \t":
            focus.call("doc:step", 0, 1, m)
            c = focus.call("doc:step", 0, 0, m, ret="char")
        if not (c is None or c == "\n"):
            # not at start of line, just fall through
            return 0
        if m == mark:
            # at start-of-line, fall-through
            return 0

        (depths,prefix) = self.calc_indent(focus, m)
        new = self.mkwhite(depths[-2])
        current = focus.call("doc:get-str", m, mark, ret="str")
        # if current is more than expected, return to expected
        if current.startswith(new) and current != new:
            return focus.call("Replace", 1, m, mark, new)
        # if current is a prefix of expectation, reduce expection until not
        if new.startswith(current):
            while len(depths) > 2:
                depths.pop()
                new = self.mkwhite(depths[-2])
                if current.startswith(new) and current != new:
                    return focus.call("Replace", 1, m, mark, new)
            return focus.call("Replace", 1, m, mark)
        # No clear relationship - replace wih new
        return focus.call("Replace", 1, m, mark, new)

    def handle_replace(self, key, focus, **a):
        "handle:Replace"
        if self.pre_paren:
            focus.call("doc:step", self.pre_paren[1], 1, 1)
            focus.call("Notify:change", self.pre_paren[0], self.pre_paren[1])
        self.pre_paren = None
        if self.post_paren:
            focus.call("doc:step", self.post_paren[1], 1, 1)
            focus.call("Notify:change", self.post_paren[0], self.post_paren[1])
        self.post_paren = None
        return 0

    def handle_refresh(self, key, focus, **a):
        "handle:Refresh"
        point = focus.call("doc:point", ret = 'mark')
        skip_pre = False
        skip_post = False
        if self.pre_paren:
            # maybe marks are still OK
            m = point.dup()
            if focus.call("doc:step", m, 0, 1, ret='char') and m == self.pre_paren[1]:
                skip_pre = True
            else:
                focus.call("doc:step", self.pre_paren[1], 1, 1)
                focus.call("Notify:change", self.pre_paren[0], self.pre_paren[1])
                self.pre_paren = None

        if self.post_paren:
            # maybe marks are still OK
            if point == self.post_paren[0]:
                skip_post = True
            else:
                focus.call("doc:step", self.post_paren[1], 1, 1)
                focus.call("Notify:change", self.post_paren[0], self.post_paren[1])
                self.post_paren = None

        if not skip_pre:
            c = focus.call("doc:step", point, 0, 0, ret = 'char')
            if c and c in ')}]':
                m2 = point.dup()
                focus.call("doc:step", m2, 0, 1)
                m1 = point.dup()
                focus.call("Move-Expr", m1, -1)
                c2 = focus.call("doc:step", m1, 1, 0, ret = 'char')
                if c2+c in "(){}[]":
                    m1['render:paren'] = "open"
                    m2['render:paren'] = "close"
                else:
                    m1['render:paren-mismatch'] = "open"
                    m2['render:paren-mismatch'] = "close"
                self.pre_paren = (m1,m2)
                focus.call("Notify:change", m1, point)

        if not skip_post:
            c = focus.call("doc:step", point, 1, 0, ret = 'char')
            if c and c in '({[':
                m1 = point.dup()
                m2 = point.dup()
                focus.call("Move-Expr", m2, 1)
                focus.call("Notify:change", point, m2)
                focus.call("doc:step", m2, 0, 1)
                c2 = focus.call("doc:step", m2, 1, 0, ret = 'char')
                if c+c2 in "(){}[]":
                    m1['render:paren'] = "open"
                    m2['render:paren'] = "close"
                else:
                    m1['render:paren-mismatch'] = "open"
                    m2['render:paren-mismatch'] = "close"
                self.post_paren = (m1,m2)

        return 0

    def handle_map_attr(self, key, focus, mark, str, comm2, **a):
        "handle:map-attr"
        if str == "render:paren" and self.pre_paren and (mark in self.pre_paren):
            comm2("cb", focus, "bg:blue+50,bold", 1)
        if str == "render:paren" and self.post_paren and (mark in self.post_paren):
            comm2("cb", focus, "bg:blue+50,bold", 1)
        if str == "render:paren-mismatch" and self.pre_paren and (mark in self.pre_paren):
            comm2("cb", focus, "bg:red+50,bold", 1)
        if str == "render:paren-mismatch" and self.post_paren and (mark in self.post_paren):
            comm2("cb", focus, "bg:red+50,bold", 1)

    def handle_para(self, key, focus, mark, num, **a):
        "handle:Move-Paragraph"
        # A "Paragraph" is a function, which starts with a line that has no
        # indent, but does have a '('
        backward = 1
        if num > 0:
             backward = 0

        while num:
            try:
                focus.call("doc:step", mark, 1-backward, 1)
                l = focus.call("text-search", mark, "^([_a-zA-Z0-9].*\(|\()",
                               0, backward)
                if not backward and l > 0:
                    while l > 1:
                        focus.call("doc:step", mark, 0, 1)
                        l -= 1
            except:
                break

            if num > 0:
                num -= 1
            else:
                num += 1
        focus.call("Move-to", mark)

        return 1

def c_mode_attach(key, focus, comm2, **a):
    p = CModePane(focus)
    p.indent_type = 'C'
    p2 = p.call("attach-whitespace", ret='focus')
    if p2:
        p = p2
    comm2("callback", p)
    return 1

def py_mode_attach(key, focus, comm2, **a):
    p = CModePane(focus)
    p.spaces = 4
    p.indent_type = 'python'
    p2 = p.call("attach-whitespace", ret='focus')
    if p2:
        p = p2
    comm2("callback", p)
    return 1

def c_mode_appeared(key, focus, **a):
    n = focus["filename"]
    if n and n[-2:] in [".c", ".h"]:
        focus["view-default"] = "c-mode"
    return 0

def py_mode_appeared(key, focus, **a):
    n = focus["filename"]
    if n and n[-3:] in [".py"]:
        focus["view-default"] = "py-mode"
    return 0

def attach_indent(key, focus, **a):
    CModePane(focus)
    return 1

editor.call("global-set-command", "doc:appeared-c-mode", c_mode_appeared)
editor.call("global-set-command", "doc:appeared-py-mode", py_mode_appeared)
editor.call("global-set-command", "attach-c-mode", c_mode_attach)
editor.call("global-set-command", "attach-py-mode", py_mode_attach)
editor.call("global-set-command", "interactive-cmd-indent", attach_indent)
