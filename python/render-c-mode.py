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

class parse_state:
    to_open = { '}':'{', ']':'[', ')':'(' }
    def __init__(self, tab):
        self.column = 0
        self.tab = tab

        self.last_was_open = False # Last code we saw was an 'open'
        self.else_indent = -1	# set when EOL parsed if we pop beyond a possible else indent

        self.sol = True		# at start-of-line, with possible white space
        self.preproc = False	# in pre-processor directive: saw # at sol
        self.preproc_continue=False# saw '\\' on preproc line

        self.s=[]		# stack
        self.save_stack= None	# where to save stack while processesing preproc

        self.open=None		# open bracket char, or None
        self.d = 0		# current depth
        self.comma_ends = False # in enum{} or a={}, comma ends a 'statement'
        self.seen = []		# interesting things we have seen
        self.ss = True		# at the start of a statement
        self.comment = None	# // or /* or None
        self.comment_col = 0	# column of the comment start
        self.quote = None	# ' or " or None
        self.have_prefix = False# Have seen, or are seeing "word("

    def push(self):
        self.s.append((self.open, self.d, self.comma_ends,
                       self.seen, self.ss, self.comment,
                       self.comment_col, self.quote, self.have_prefix))
        self.seen = []
        self.have_prefix = False
    def pop(self):
        if not self.s:
            return
        (self.open, self.d, self.comma_ends,
         self.seen, self.ss, self.comment,
         self.comment_col, self.quote, self.have_prefix)= self.s.pop()

    def parse(self, c, p, m):

        if self.quote:
            self.parse_quote(c, p, m)
        elif self.comment:
            self.parse_comment(c, p, m)
        else:
            if c == '/':
                c2 = p.call("doc:step", 1, 0, m, ret='char')
                if c2 in '/*':
                    self.comment = '/' + c2
                    self.comment_col = self.column
                    p.call("doc:step", 1, 1, m)
                    self.column += 1
                else:
                    self.parse_code(c, p, m)
            else:
                self.parse_code(c, p, m)

        if c == '\n':
            self.column = 0
        elif c == '\t':
            self.column = (self.column|7)+1
        else:
            self.column += 1

    def parse_quote(self, c, p, m):
        if c == self.quote:
            self.quote = None
        elif c == '\\':
            c = p.call("doc:step", 1, 0, m, ret='char')
            if c == self.quote or c == '\\':
                # step over this second char as well. We could do this
                # for any char except newline.
                p.call("doc:step", 1, 1, m)
                self.column += 1

    def parse_comment(self, c, p, m):
        if self.comment == '//':
            # closed by end-of-line
            if c == '\n':
                self.comment = None
                self.parse_newline()
        elif self.comment == '/*':
            # closed by */
            if c == '*' and p.call("doc:step", 1, m, ret='char') == '/':
                p.call("doc:step", 1, 1, m)
                self.column += 1
                self.comment = None

    def parse_newline(self):
        # we've just seen a newline in code (not a multi-line comment)
        if self.last_was_open and self.s:
            # The open bracket was at end of line, so indent depth
            # should be previous plus one tab.
            self.d = self.s[-1][1] + self.tab
            self.last_was_open = False
        if ':' in self.seen:
            # multiple colons are only interesting on the one line
            self.seen.remove(':')

    def parse_code(self, c, p, m):
        if self.sol and c == '#':
            # switch to handling preprocessor directives
            self.push()
            self.open = '^'
            self.save_stack = self.s
            self.s = []
            self.ss = True
            self.preproc = True
            self.sol = False
            self.d = 8
            # Skip over 'define name(names,)' as it looks too much like a prefix
            m2 = m.dup()
            if p.call("text-match","[ \\t]*define [a-z_][a-z0-9_]*\\(([a-z0-9_]+| |,|\\.\\.\\.)*\\)",
                      m2, 1) > 1:
                m.to_mark(m2)
            return
        if self.preproc:
            # we leave preproc mode at eol, unless there was a '\\'
            if c == '\n' and not self.preproc_continue:
                self.preproc = False
                self.s = self.save_stack
                self.pop()
                self.save_stack = None
            else:
                self.preproc_continue = (
                    c == '\\' and p.call("doc:step", 1, m, ret='char') == '\n')
        self.sol = c == '\n'

        # treat '\' like white-space as probably at eol of macro
        if c in ' \t\n\\':
            # ignore white space
            if c == '\n':
                self.parse_newline()
            return

        # probably not at start of statement after this
        ss = self.ss
        self.ss = False

        if c == '"' or c == "'":
            self.quote = c
            return

        if c in '{([':
            seen = self.seen
            if (c == '{' and self.open == None and self.d > 0 and
                not 'else' in self.seen):
                # '{' subsumes a 'prefix' nesting except after 'else'
                pass
            else:
                self.push()
            self.open = c
            self.comma_ends = False
            if c == '{':
                self.ss = True
                if '=' in seen or 'enum' in seen:
                    self.comma_ends = True
            self.d = self.column+1
            self.last_was_open = True
            return
        self.last_was_open = False
        if c in ')]}':
            while self.s and self.open is None:
                self.pop()
            if self.to_open[c] != self.open:
                return
            self.pop()
            if c == '}':
                self.end_statement(p, m)
            if c == ')' and self.have_prefix:
                # starting a new statement
                self.push()
                self.open = None
                self.ss = True
                self.d += self.tab
            return

        if c == ';' or (c ==',' and self.comma_ends):
            self.end_statement(p, m)
        if c == '?':
            # could be ?:, in any case, probably not a label
            self.seen.append(c)
        if c == ':' and '?' not in self.seen and not c in self.seen:
            # probably a label - so now at start of statement
            # We might have seen case (foo) which looked like a prefix
            # If so, pop it.
            if self.open is None:
                self.pop()
            self.ss = True
            self.seen.append(c)
        if c == '=':
            # if we see a '{' now, then it is a structured value
            # and comma act line end-of-statement
            self.seen.append(c)

        if c.isalnum or c == '_' or c.isspace():
            # In a word
            if ss:
                self.have_prefix = True
        else:
            self.have_prefix = False

        if ss and c == 'i' and p.call("text-match", m.dup(), "f\\b") > 0:
            self.seen.append("if")
        if ss and c == 'e' and p.call("text-match", m.dup(), "num\\b") > 0:
            self.seen.append("enum")
        if ss and ((c == 'd' and p.call("text-match", m.dup(), "o[ \t]*[/\n]") > 0) or
                   (c == 'e' and p.call("text-match", m.dup(), "lse[ \t]*[/\n]") > 0)):
            # do or else start a new statement, like if() does
            while p.call("doc:step", 1, m,ret='char') in 'elsedo':
                p.call("doc:step", 1, 1, m)
                self.column += 1
            self.push()
            self.open = None
            self.ss = True
            self.d += self.tab


    def end_statement(self, p, m):
        self.have_prefix = False
        see_else = p.call("text-match", m.dup(), " else\\b", 1) > 0
        self.else_indent = -1
        while self.s and self.open == None and (not see_else or
                                                not 'if' in self.seen):
            if 'if' in self.seen and self.else_indent < 0:
                self.else_indent = self.d
            self.pop()
        self.ss = True
        self.seen = []
        if see_else:
            self.seen.append('else')

    def preparse(self, c):
        # This character is at (or near) start of line and so might affect
        # the indent we want here.
        if c in ')]}' and self.to_open[c] == self.open:
            self.pop()
            self.ss = True
        if c == '{' and self.ss and self.open == None:
            self.pop()
            self.ss = True


class CModePane(edlib.Pane):
        # C indenting can be determined precisely, but it
        # requires a lot of work.
        # We need to go back to the start of the function
        # (or structure?) and work forwards
        # We need to:
        #  recognize labels (goto and case and default).  These don't affect
        #   other lines, but decrease the indent of this line
        #  Skip comments, but recognize when in one - comment are expected to
        #   start at the prevailing indent, but must be consistently indented.
        #  track nested {} [] (). Open at end-of-line increases indent by 1 tab,
        #    open elsewhere sets an indent at current position.
        #  If a line ends with ';' or '}' then it ends a statement,
        #  else next line is indented
        #  Similarly after a ';' or '}' or label, a statement starts
        #  labels are only recognised at a statement start.
        #  If a statement starts "word spaces (", then another statement
        #  starts after the ')', and if that is on a new line, it is indented.
        # The total indent is the nest-depth of brackets or prefixed statements
        # (switch,if,while), plus an extra tab when not on the the first line
        # of a statement.
        # When inside a comment, set get an alignment of the '*' and
        # a prefix of '* '
        # When inside brackets '{[(' that start at end-of-line, indent
        # increments. If they start not at eol, they set an alignment.
        # 'eol' must be determined ignoring spaces and comments.
        # A line starting with some close brackets and an option open has
        # indent decreased by the number of brackets.
        #
        # So we find a zero-point, which is a non-label, non-comment, non-#
        # at start of line.
        # Then we move forward tracking the state.
        # Elements of state are:
        #  an array of depths which are either prev+self.spaces, or an alignment
        #  a string identifying type of each indent.  Types are:
        #    { ( [ - bracket nesting
        #    p     - like ( but for a statement prefix, so closing ) starts a
        #            statement and, if at eol, adds an indent
        #    space - indent case by 'p' above. statement-end closes multiple
        #            of these.
        #  flag "start of statement"
        #  comment type none,line,block
        #  quote type none,single,double
        #  flag (have_prefix) when possibly in a prefix
        #  column
        #
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

        # 'zero' point is a start of line that is not empty, not
        #  white space, not #, not /, not }, and not "alphanum:"
        #  But don't accept a match on this line
        m = mark.dup()

        p.call("Move-EOL", m, -1)
        p.call("doc:step", m, 0, 1)
        try:
            p.call("text-search", 1, 1, m,
                   "^([^\\s\\a\\A\\d#/}]|[\\A\\a\\d_]+[\\s]*[^:\\s\\A\\a\\d_]|[\\A\\a\\d_]+[\\s]+[^:\\s])")
        except edlib.commandfailed:
            p.call("Move-file", m, -1)

        tab = self.spaces
        if not tab:
            tab = 8

        ps = parse_state(tab)
        c = None
        while m < mark:
            c = p.call("doc:step", 1, 1, m, ret='char')
            ps.parse(c, p, m)
            if c is None:
                break
        # we always want the indent at the start of a line
        if c != '\n':
            ps.parse('\n', p, m)

        br = mark.dup()
        c = p.call("doc:step", 1, 1, br, ret='char')
        non_hash = False
        while c and c in '\t }]){':
            if c in '}]){':
                non_hash = True
                ps.preparse(c)
            c = p.call("doc:step", 1, 1, br, ret='char')

        preproc = False
        if c == '#' and not non_hash:
            # line starts '#', so want no indent
            depth = [0]
            preproc = True
        elif not ps.ss and ps.open in [ '^', '{', None]:
            # statement continuation
            depth = [ps.d + ps.tab]
        else:
            # assume exactly the current depth
            depth = [ps.d]

        if ps.ss:
            # Allow reverting to start of line for label if at the
            # start of a statement.
            depth.insert(0, 0)
            # if a '{' or '}' would make sense to reduce indent,
            # allow that.
            if ps.s and  ps.open in [ '{', None ]:
                depth.insert(-1, ps.s[-1][1])

        # Only allow an extra indent only if there could be a hanging else
        # or if a preproc line could optionally be indented
        if preproc:
            depth.append(ps.d)
        elif ps.else_indent > depth[-1]:
            depth.append(ps.else_indent)
        else:
            depth.append(depth[-1])

        # Check for label.  Need to be at start of line, but
        # may only step over white space.  When trying :Enter, we
        # mustn't think we can see the label at the start of this line.
        st = m.dup()
        c = p.call("doc:step", 0, 1, st, ret='char')
        while c and c in ' \t':
            c = p.call("doc:step", 0, 1, st, ret='char')
        if c == '\n':
            p.call("doc:step", 1, 1, st)

            l = p.call("text-match", st.dup(),
                       '^[ \t]*(case\\s[^:\n]*|default[^\\A\\a\\d:\n]*|[_\\A\\a\\d]+):')
        else:
            l = 0
        if l > 0:
            if p.call("doc:step", 1, st, ret='char') in ' \t':
                label_line = "indented-label"
            else:
                if (p.call("text-match", st.dup(), '^[_\\A\\a\\d]+:') > 0 and
                    p.call("text-match", st.dup(), '^default:') <= 0):
                    label_line = "margin-label"
                else:
                    label_line = "indented-label"

            if label_line == "margin-label":
                depth = [0, 0]
            elif len(depth) >= 3:
                depth = [depth[-3],depth[-3]]
            else:
                depth = [depth[0],depth[0]]

        if ps.comment == "/*":
            prefix = "* "
            if p.call("text-match", m.dup(), "[ \t]*\*") > 1:
                prefix = ""
            depth = [ps.comment_col+1,ps.comment_col+1]
        else:
            prefix = ""
        return (depth, prefix)

    def calc_indent_python(self, p, m):
        # like calc-indent, but check for ':' at end of previous line
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

    def handle_close(self, key, focus, mark, **a):
        "handle-list/K-}/K-)/K-]/K-{/"

        if self.indent_type != 'C':
            return 0

        # If at start of line - plus close/open, re-indent this line
        self.parent.call(key, focus, mark, **a)
        m = mark.dup()
        focus.call("Move-EOL", m, -1)
        if focus.call("text-match", m.dup(), "^[\\s]*[])}{]") > 0:
            self.handle_tab(key, focus, m, 1)
        return 1

    def handle_colon(self, key, focus, mark, **a):
        "handle:K-:"

        if self.indent_type != 'C':
            return 0

        # If this looks like a lable line, re-indent
        self.parent.call(key, focus, mark, **a)
        m = mark.dup()
        focus.call("Move-EOL", m, -1)
        if focus.call("text-match", m.dup(),
                      '^[ \t]*(case\\s[^:\n]*|default[^\\A\\a\\d:\n]*|[_\\A\\a\\d]+):') > 0:
            self.handle_tab(key, focus, m, 1)
        return 1


    def handle_enter(self, key, focus, mark, num2, **a):
        "handle:K:Enter"
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
        try:
            return focus.call("Interactive:insert", 1, num2, m,
                              "\n" + self.mkwhite(depths[-2]) + prefix)
        except edlib.commandfailed:
            # probably doc is read-only.  Fall through to default to get error
            return 0

    def handle_tab(self, key, focus, mark, num, **a):
        "handle:K:Tab"
        # if there is only white-space before cursor (up to newline) then:
        # move to end of white-space
        # - choose an indent as for Return
        # - If we don't have exactly that, replace with that
        # - if we do have that, use the 'extra' indent level
        # if num is <0 go to indent and 'backspace'
        # if num is 0, go to start of line and 'tab'
        m = mark.dup()
        if num <= 0:
            focus.call("Move-EOL", -1, m)
            c = focus.call("doc:step", 1, 0, m, ret='char')
            while c and c in ' \t':
                focus.call("doc:step", 1, 1, m)
                c = focus.call("doc:step", 1, 0, m, ret='char')
            if num < 0:
                self.handle_bs(key, focus, m)
            else:
                self.handle_tab(key, focus, m, 1)
            return 1
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
            new = "\t" * int(len / 8)
            try:
                focus.call("Replace", 1, m, mark, new)
            except edlib.commandfailed:
                # probably read-only
                pass
            # fall through it insert a new tab
            return 0

        # m at start-of-line, move mark (point) to first non-white-space
        c = focus.call("doc:step", 1, 0, mark, ret="char")
        moved = False
        while c and c in " \t":
            moved = True
            focus.call("doc:step", 1, 1, mark)
            c = focus.call("doc:step", 1, 0, mark, ret="char")

        if key == "Reindent" and c == '\n':
            # Blank line, do nothing for reindent
            if m < mark:
                try:
                    focus.call("Replace", 1, m, mark, "")
                except edlib.commandfailed:
                    pass
            return 1

        (depths,prefix) = self.calc_indent(focus, m)

        new = self.mkwhite(depths[-2])

        current = focus.call("doc:get-str", m, mark, ret="str")

        if new != current:
            try:
                return focus.call("doc:replace", 1, m, mark, new+prefix)
            except edlib.commandfailed:
                pass
            return 0
        if depths[-1] == depths[-2] or key == "Reindent" or moved:
            # Either we weren't at start of text and have moved there,
            # or there is no extra indent allowed, possibly because we are doing
            # re-indent.  In any of these cases, current indent is good enough,
            # stay where we are.
            return 1

        new = self.mkwhite(depths[-1])
        try:
            return focus.call("doc:replace", 1, m, mark, new)
        except edlib.commandfailed:
            pass
        return 0

    def handle_meta_tab(self, key, focus, mark, **a):
        "handle:K:M:Tab"
        # like tab-at-start-of-line, anywhere in line
        # Probably need to type esc-tab, as Alt-Tab normally
        # goes to next window in window system
        m = mark.dup()
        focus.call("Move-EOL", -1, m)
        self.handle_tab(key, focus, m, 1)

    def handle_bs(self, key, focus, mark, **a):
        "handle:K:Backspace"
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
            try:
                return focus.call("doc:replace", 1, m, mark, new)
            except edlib.commandfailed:
                return 0
        # if current is a prefix of expectation, reduce expection until not
        if new.startswith(current):
            while len(depths) > 2:
                depths.pop()
                new = self.mkwhite(depths[-2])
                if current.startswith(new) and current != new:
                    try:
                        return focus.call("doc:replace", 1, m, mark, new)
                    except edlib.commandfailed:
                        return 0
            try:
                return focus.call("doc:replace", 1, m, mark)
            except edlib.commandfailed:
                return 0
        # No clear relationship - replace wih new
        try:
            return focus.call("doc:replace", 1, m, mark, new)
        except edlib.commandfailed:
            return 0

    def handle_indent_para(self, key, focus, mark, mark2, **a):
        "handle:fill-paragraph"

        if not mark:
            return edlib.Enoarg
        m2 = mark2
        if m2:
            if m2 < mark:
                mark, m2 = m2, mark
        else:
            m2 = mark.dup()
            e = mark.dup()
            focus.call("Move-EOL", 1, e)
            while m2 < e:
                if focus.call("Move-Expr", 1, m2) <= 0:
                    break
        # now call handle_tab() on each line from mark to m2
        while mark < m2:
            self.handle_tab("Reindent", focus, mark, 0)
            if focus.call("Move-EOL", 2, mark) <= 0:
                break

        return 1

    def handle_replace(self, key, focus, **a):
        "handle:Replace"
        if self.pre_paren:
            focus.call("view:changed", 1, mark  = self.pre_paren[0])
            focus.call("view:changed", 1, mark2 = self.pre_paren[1])
        self.pre_paren = None
        if self.post_paren:
            focus.call("view:changed", 1, mark  = self.post_paren[0])
            focus.call("view:changed", 1, mark2 = self.post_paren[1])
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
                focus.call("view:changed", 1, mark  = self.pre_paren[0])
                focus.call("view:changed", 1, mark2 = self.pre_paren[1])
                self.pre_paren = None

        if self.post_paren:
            # maybe marks are still OK
            if point == self.post_paren[0]:
                skip_post = True
            else:
                focus.call("view:changed", 1, mark  = self.post_paren[0])
                focus.call("view:changed", 1, mark2 = self.post_paren[1])
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
                focus.call("view:changed", 1, mark  = m1)
                focus.call("view:changed", 1, mark2 = m2)

        if not skip_post:
            c = focus.call("doc:step", point, 1, 0, ret = 'char')
            if c and c in '({[':
                m1 = point.dup()
                m2 = point.dup()
                focus.call("Move-Expr", m2, 1)
                focus.call("doc:step", m2, 0, 1)
                c2 = focus.call("doc:step", m2, 1, 0, ret = 'char')
                if c+c2 in "(){}[]":
                    m1['render:paren'] = "open"
                    m2['render:paren'] = "close"
                else:
                    m1['render:paren-mismatch'] = "open"
                    m2['render:paren-mismatch'] = "close"
                self.post_paren = (m1,m2)
                focus.call("view:changed", 1, mark  = m1)
                focus.call("view:changed", 1, mark2 = m2)

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
                l = focus.call("text-search", mark, "^([_a-zA-Z0-9].*\\(|\\()",
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
    def handle_expr(self, key, focus, mark, num, num2, **a):
        "handle:Move-Expr"
        # Add '_' to list for word chars
        return self.parent.call(key, focus, mark, num, num2, "_")

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
