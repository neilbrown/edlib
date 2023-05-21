# -*- coding: utf-8 -*-
# Copyright Neil Brown (c)2018-2022 <neil@brown.name>
# May be distributed under terms of GPLv2 - see file:COPYING

def textwidth(line, w=0):
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

        self.open='^'		# open bracket char, or None
        self.d = 0		# current depth
        self.comma_ends = False # in enum{} or a={}, comma ends a 'statement'
        self.seen = []		# interesting things we have seen
        self.ss = True		# at the start of a statement
        self.comment = None	# // or /* or None
        self.comment_col = 0	# column of the comment start
        self.tab_col = 0	# column of tab after code
        self.quote = None	# ' or " or None
        self.have_prefix = False# Have seen, or are seeing "word("

    def push(self):
        self.s.append((self.open, self.d, self.comma_ends,
                       self.seen, self.ss, self.comment,
                       self.comment_col, self.quote, self.have_prefix,
                       self.tab_col))
        self.seen = []
        self.have_prefix = False
    def pop(self):
        if not self.s:
            return
        (self.open, self.d, self.comma_ends,
         self.seen, self.ss, self.comment,
         self.comment_col, self.quote, self.have_prefix,
         self.tab_col)= self.s.pop()

    def parse(self, c, p, m):

        if self.quote:
            self.parse_quote(c, p, m)
        elif self.comment:
            self.parse_comment(c, p, m)
        else:
            if c == '/':
                c2 = p.following(m)
                if c2 in '/*':
                    if self.tab_col == self.column:
                        # tabs for comments ignored
                        self.tab_col = 0
                    self.comment = '/' + c2
                    self.comment_col = self.column
                    p.next(m)
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
            c = p.following(m)
            if c == self.quote or c == '\\':
                # step over this second char as well. We could do this
                # for any char except newline.
                p.next(m)
                self.column += 1

    def parse_comment(self, c, p, m):
        if self.comment == '//':
            # closed by end-of-line
            if c == '\n':
                self.comment = None
                self.parse_newline()
        elif self.comment == '/*':
            # closed by */
            if c == '*' and p.following(m) == '/':
                p.next(m)
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
        if self.tab_col == self.column:
            # tab at end-of-line ignored
            self.tab_col = 0

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

            if p.call("text-match","[ \\t]*define ", m.dup(), 1) > 1:
                self.seen.append('define')
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
                    c == '\\' and p.following(m) == '\n')
        # FIXME c can be None here - maybe timeout
        if c not in ' \t':
            self.sol = c == '\n'

        # treat '\' like white-space as probably at eol of macro
        if c in ' \t\n\\':
            # ignore white space
            if c == '\n':
                self.parse_newline()
            if not self.ss and c == '\t' and (self.tab_col == 0 or
                                              self.tab_col == self.column):
                self.tab_col = (self.column|7)+1
            if not self.ss and c == ' ' and self.tab_col == self.column:
                # spaces after a tab extend that tab.
                self.tab_col += 1

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
            if c == '{':
                self.ss = True
                if ('=' in seen or 'enum' in seen or
                    'define' in seen or 'define-body' in seen):
                    self.comma_ends = True
            if 'case' not in seen:
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
                is_define = 'define' in self.seen
                if is_define:
                    self.seen.remove('define')
                self.push()
                self.open = None
                self.ss = True
                # define foo(bar) looks like a prefix, but doesn't indent like one.
                if is_define:
                    self.seen.append('define-body')
                else:
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

        if c.isalnum() or c == '_' or c.isspace():
            # In a word - 'return' never indicates a prefix.
            if ss:
                self.have_prefix = False
                if (not self.comma_ends and
                    not (c == 'r' and p.call("text-match", m.dup(), "eturn\\b") > 0)):
                        self.have_prefix = True
        else:
            self.have_prefix = False

        if ss and c == 'i' and p.call("text-match", m.dup(), "f\\b") > 0:
            self.seen.append("if")
        if ss and c == 'e' and p.call("text-match", m.dup(), "num\\b") > 0:
            self.seen.append("enum")
        if ss and c == 'c' and p.call("text-match", m.dup(), "ase\\b") > 0:
            self.seen.append("case")
        if ss and c == 'd' and p.call("text-match", m.dup(), "efault\\b") > 0:
            self.seen.append("case")
        if ss and ((c == 'd' and p.call("text-match", m.dup(), "o\\b") > 0) or
                   (c == 'e' and p.call("text-match", m.dup(), "lse\\b") > 0)):
            # do or else start a new statement, like if() does
            while p.following(m) in 'elsedo':
                p.next(m)
                self.column += 1
            self.push()
            self.open = None
            self.ss = True
            if c == 'e' and p.call("text-match", m.dup(), "[ \t]+if\\b") > 0:
                # "else if" doesn't increase depth
                pass
            else:
                # otherwise increase depth one tabstop
                self.d += self.tab

    def end_statement(self, p, m):
        see_else = p.call("text-match", m.dup(), " else\\b", 1) > 0
        self.else_indent = -1
        while self.s and self.open == None and (not see_else or
                                                not 'if' in self.seen):
            if 'if' in self.seen and self.else_indent < 0:
                self.else_indent = self.d
            self.pop()
        self.ss = True; self.have_prefix = False
        self.tab_col = 0
        self.seen = []
        if see_else:
            self.seen.append('else')

    def preparse(self, c):
        # This character is at (or near) start of line and so might affect
        # the indent we want here.
        if c in ')]}' and self.to_open[c] == self.open:
            self.pop()
            self.ss = True
        if c == '{' and self.open == None:
            self.pop()
            self.ss = True

def at_sol(p, m):
    c = p.prior(m)
    while c and c in ' \t':
        p.prev(m)
        c = p.prior(m)
    return c == None or c == '\n'

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

        self.call("doc:request:mark:moving")
        self.call("doc:request:doc:replaced")
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
        #  Starts either:
        #   - punctuation, not / or #.  Expect '{' at start of line I guess, or
        #   - identifier that is followed by puctuation, but not as a label.  Maybe "foo("
        #   - identifier that is followed by space, but again not a label.
        m = mark.dup()

        p.call("doc:EOL", m, -1, 1)
        try:
            p.call("text-search", 1, 1, m,
                   "^([^\\s\\a\\A\\d#/}]|[\\A\\a\\d_]+[\\s]*[^:\\s\\A\\a\\d_]|[\\A\\a\\d_]+[\\s]+[^:\\s])")
        except edlib.commandfailed:
            p.call("doc:file", m, -1)

        tab = self.spaces
        if not tab:
            tab = 8

        ps = parse_state(tab)
        c = None
        while m < mark:
            c = p.next(m)
            ps.parse(c, p, m)
            if c is None:
                break
        # we always want the indent at the start of a line
        if c != '\n':
            ps.parse('\n', p, m)

        br = mark.dup()
        c = p.next(br)
        non_space = False
        while c and c in '\t }]){':
            if c in '}]){':
                non_space = True
                ps.preparse(c)
            c = p.next(br)

        preproc = False
        if c == '#' and not non_space:
            # line starts '#', so want no indent
            depth = [0]
            preproc = c
        elif (c == '/' and not non_space and
            p.following(br) in '/*'):
            # Comment at start of line is indented much like preproc
            depth = [0]
            preproc = c
        elif not ps.ss and c == '.' and not non_space and ps.comma_ends:
            # inside a value specifier and can see a '.' and start of line,
            # so probably is the start of a 'statement' despite ps.ss being false
            depth = [ps.d]
        elif not ps.ss and (ps.open in [ '{', None] or (ps.open == '^' and
                                                        '=' in ps.seen)):
            # statement continuation
            if ps.tab_col and ps.tab_col > ps.d + ps.tab:
                depth = [ps.tab_col]
            else:
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
        if preproc == '/':
            # Really a comment - put the indent *before* the zero, so it is
            # preferred if neither is present.
            depth.insert(-1, ps.d)
        elif preproc:
            # allow an indent, but don't prefer it
            depth.append(ps.d)
        elif ps.else_indent > depth[-1]:
            depth.append(ps.else_indent)
        else:
            depth.append(depth[-1])

        # Check for label.  Need to be at start of line, but
        # may only step over white space.  When trying :Enter, we
        # mustn't think we can see the label at the start of this line.
        st = m.dup()
        c = p.prev(st)
        while c and c in ' \t':
            c = p.prev(st)
        if c == '\n':
            p.next(st)

            l = p.call("text-match", st.dup(),
                       '^[ \t]*(case\\s[^:\n]*|default[^\\A\\a\\d:\n]*|[_\\A\\a\\d]+):')
        else:
            l = 0
        if l > 0:
            if p.following(st) in ' \t':
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
            if p.call("text-match", m.dup(), "[ \\t]*\\*") > 1:
                prefix = ""
            depth = [ps.comment_col+1,ps.comment_col+1]
        else:
            prefix = ""
        return (depth, prefix)

    def calc_indent_python(self, p, m):
        # like calc-indent, but check for ':' at end of previous line,
        # and # at the start.

        # first look to see if previous line is a comment.
        m1 = m.dup()
        c = p.prev(m1)
        saw_nl = False
        while c and c in ' \t\n':
            if c == '\n':
                saw_nl = True
            c = p.prev(m1)
        p.call("doc:EOL", -1, m1)
        sol = m1.dup()
        c = p.next(m1)
        while c and c in ' \t':
            c = p.next(m1)
        if c == '#':
            # comment found, use same indent
            pfx = p.call("doc:get-str", sol, m1, ret='str')
            w = textwidth(pfx) - 1
            r = ([w,w],'')
            if not saw_nl:
                # Are handling Enter, so continue the comment
                r = ([w,w],'# ')
            return r

        sol = m.dup()
        indent = self.calc_indent(p, m, 'default', sol)
        m = m.dup()
        c = p.prev(m)
        while c and c in ' \t\n':
            c = p.prev(m)
        if c == ':':
            i = indent[0][-1]
            # No other indents make sense here
            indent = ( [i,i], '')
        else:
            if p.call("text-match", sol,
                      "[ \t]*(return|pass|break|continue)\\b") > 1:
                # Probably last statement of a block, prefer no indent
                if len(indent[0]) >= 2:
                    indent[0].pop()
        return indent

    def calc_indent(self, p, m, type=None, sol=None):
        # m is at the end of a line or start of next line (in leading
        # white-space) in p - Don't move it.
        # Find indent for the following text (this line or next line)
        # and an alignment...

        if not type:
            if self.indent_type == 'C':
                return self.calc_indent_c(p, m)
            if self.indent_type == 'python':
                return self.calc_indent_python(p, m)

        m1 = m.dup()
        # Find previous line which is not empty and does not start in
        # parentheses w.r.t. here.  If we hit an unbalanced open, or a line
        # break - that is the line we want.
        # Make up a depth list from the leading tabs/spaces.
        ret = p.call("doc:expr", -1, m1)

        while ret == 1 and not at_sol(p, m1):
            ret = p.call("doc:expr", -1, m1)
        # This is the starting line.
        p.call("doc:EOL", -1, m1)

        line_start = m1.dup()
        if sol:
            sol.to_mark(line_start)
        c = p.following(m1)
        while c and c in ' \t':
            p.next(m1)
            c = p.following(m1)

        # line_start .. m1 is the prefix
        pfx = p.call("doc:get-str", line_start, m1, ret = 'str')
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
        indent_end = m1
        if indent_end >= m:
            # There is no previous line - Don't change indent
            r.append(r[-1])
            return r, ''
        expr = m.dup()
        ret = p.call("doc:expr", -1, 1, expr)
        if ret == 1 and expr >= indent_end:
            p.next(expr)
            if p.following(expr) == '\n':
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
            return edlib.Efallthrough

        # If at start of line - plus close/open, re-indent this line
        try:
            self.parent.call(key, focus, mark, **a)
        except edlib.commandfailed:
            # probably readonly
            return edlib.Efallthrough
        m = mark.dup()
        focus.call("doc:EOL", m, -1)
        if focus.call("text-match", m.dup(), "^[\\s]*[])}{]") > 0:
            self.handle_tab(key, focus, m, 1)
        return 1

    def handle_colon(self, key, focus, mark, **a):
        "handle:K-:"

        if self.indent_type != 'C':
            return edlib.Efallthrough

        # If this looks like a label line, re-indent
        try:
            self.parent.call(key, focus, mark, **a)
        except edlib.commandfailed:
            # probably readonly
            return edlib.Efallthrough
        m = mark.dup()
        focus.call("doc:EOL", m, -1)
        if focus.call("text-match", m.dup(),
                      '^[ \t]*(case\\s[^:\n]*|default[^\\A\\a\\d:\n]*|[_\\A\\a\\d]+):') > 0:
            self.handle_tab(key, focus, m, 1)
        return 1

    def handle_auto(self, key, focus, mark, **a):
        "handle-list/K-;/K-}/K-,"
        # re-indent when these chars are typed

        if self.indent_type != 'C':
            return edlib.Efallthrough

        # insert first..
        try:
            self.parent.call(key, focus, mark, **a)
        except edlib.commandfailed:
            # probably read-only
            return edlib.Efallthrough
        m = mark.dup()
        focus.call("doc:EOL", m, -1)
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
        c = focus.following(m)
        while c and c in " \t":
            focus.next(m)
            c = focus.following(m)
        focus.call("Move-to", m)
        # Second, move m back over any white space.
        c = focus.prev(m)
        while c != None:
            if c not in " \t":
                focus.next(m)
                break
            c = focus.prev(m)
        # Now calculate the indent
        (depths,prefix) = self.calc_indent(focus, m)

        # and replace all that white space with a newline and the indent
        try:
            return focus.call("Interactive:insert", 1, num2, m,
                              "\n" + self.mkwhite(depths[-2]) + prefix)
        except edlib.commandfailed:
            # probably doc is read-only.  Fall through to default to get error
            return edlib.Efallthrough

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
            focus.call("doc:EOL", -1, m)
            c = focus.following(m)
            while c and c in ' \t':
                focus.next(m)
                c = focus.following(m)
            if num < 0:
                self.handle_bs(key, focus, m)
            else:
                self.handle_tab(key, focus, m, 1)
            return 1
        c = focus.prior(m)
        prevc = c
        while c and c in " \t":
            focus.prev(m)
            c = focus.prior(m)
        if not (c is None or c == "\n"):
            # not at start of line, maybe convert preceding spaces to tabs
            if prevc != ' ':
                return edlib.Efallthrough
            m = mark.dup()
            len = 0
            while focus.prior(m) == ' ':
                len += 1
                focus.prev(m)
            new = "\t" * int(len / 8)
            try:
                focus.call("Replace", 1, m, mark, new)
            except edlib.commandfailed:
                # probably read-only
                pass
            # fall through it insert a new tab
            return edlib.Efallthrough

        # m at start-of-line, move mark (point) to first non-white-space
        c = focus.following(mark)
        moved = False
        while c and c in " \t":
            moved = True
            focus.next(mark)
            c = focus.following(mark)

        if key != "K:Tab" and c == '\n':
            # Blank line, do nothing for reindent
            if m < mark:
                try:
                    focus.call("Replace", 1, m, mark, "")
                except edlib.commandfailed:
                    pass
            return 1

        (depths,prefix) = self.calc_indent(focus, m)

        new = self.mkwhite(depths[-2])
        new2 = self.mkwhite(depths[-1])

        current = focus.call("doc:get-str", m, mark, ret="str")

        if (key != 'K:Tab' and
            focus.following(mark) in '#/' and
            (current == new or current == new2)):
            # This is a preproc directive or comment.  They can equally
            # go in one of two places - start of line or indented.
            # If they are at either, leave them for re-indent
            return 1

        if new != current:
            if prefix and focus.following(mark) == prefix[0]:
                # Don't at an extra prefix
                prefix = ""
            try:
                return focus.call("doc:replace", 1, m, mark, new+prefix)
            except edlib.commandfailed:
                pass
            return edlib.Efallthrough
        if depths[-1] == depths[-2] or key != "K:Tab" or moved:
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
        return edlib.Efallthrough

    def handle_shift_tab(self, key, focus, mark, **a):
        "handle:K:S:Tab"
        # like tab-at-start-of-line, anywhere in line
        m = mark.dup()
        focus.call("doc:EOL", -1, m)
        self.handle_tab(key, focus, m, 1)

    def handle_bs(self, key, focus, mark, **a):
        "handle:K:Backspace"
        # If in the indent, remove one level of indent
        m = mark.dup()
        c = focus.following(m)
        if c and c in " \t":
            # Not at end of indent, fall through
            return edlib.Efallthrough
        c = focus.prior(m)
        while c and c in " \t":
            focus.prev(m)
            c = focus.prior(m)
        if not (c is None or c == "\n"):
            # not at start of line, just fall through
            return edlib.Efallthrough
        if m == mark:
            # at start-of-line, fall-through
            return edlib.Efallthrough

        (depths,prefix) = self.calc_indent(focus, m)
        new = self.mkwhite(depths[-2])
        current = focus.call("doc:get-str", m, mark, ret="str")
        # if current is more than expected, return to expected
        if current.startswith(new) and current != new:
            try:
                return focus.call("doc:replace", 1, m, mark, new)
            except edlib.commandfailed:
                return edlib.Efallthrough
        # if current is a prefix of expectation, reduce expection until not
        if new.startswith(current):
            while len(depths) > 2:
                depths.pop()
                new = self.mkwhite(depths[-2])
                if current.startswith(new) and current != new:
                    try:
                        return focus.call("doc:replace", 1, m, mark, new)
                    except edlib.commandfailed:
                        return edlib.Efallthrough
            try:
                return focus.call("doc:replace", 1, m, mark)
            except edlib.commandfailed:
                return edlib.Efallthrough
        # No clear relationship - replace wih new
        try:
            return focus.call("doc:replace", 1, m, mark, new)
        except edlib.commandfailed:
            return edlib.Efallthrough

    def handle_indent_para(self, key, focus, mark, mark2, **a):
        "handle:reindent-paragraph"

        if not mark:
            return edlib.Enoarg
        m2 = mark2
        if m2:
            if m2 < mark:
                mark, m2 = m2, mark
        else:
            m2 = mark.dup()
            e = mark.dup()
            focus.call("doc:EOL", 1, e)
            while m2 < e:
                m3 = m2.dup()
                if focus.call("doc:expr", 1, m3) <= 0:
                    break
                if m3 <= m2:
                    # doc:expr sometimes doesn't move at all
                    break
                m2 = m3
        # now call handle_tab() on each line from mark to m2
        while mark < m2:
            self.handle_tab("Reindent", focus, mark, 0)
            if focus.call("doc:EOL", 1, mark, 1) <= 0:
                break

        return 1

    def update(self, focus, pair):
        if not pair:
            return
        focus.call("view:changed", 1, mark  = pair[0])
        focus.call("view:changed", 1, mark2 = pair[1])

    def handle_replace(self, key, focus, **a):
        "handle:doc:replaced"
        self.update(self.leaf, self.pre_paren)
        self.pre_paren = None
        self.update(self.leaf, self.post_paren)
        self.post_paren = None
        self.damaged(edlib.DAMAGED_VIEW)
        return 1

    def handle_moving(self, key, focus, mark, **a):
        "handle:mark:moving"
        point = self.call("doc:point", ret = 'mark')
        if mark.seq == point.seq:
            self.damaged(edlib.DAMAGED_VIEW)
        return 1

    def handle_refresh(self, key, focus, **a):
        "handle:Refresh:view"
        point = focus.call("doc:point", ret = 'mark')
        point.ack()
        skip_pre = False
        skip_post = False
        if self.pre_paren:
            # maybe marks are still OK
            m = point.dup()
            if focus.prev(m) and m == self.pre_paren[1]:
                skip_pre = True
            else:
                self.update(focus, self.pre_paren)
                self.pre_paren = None

        if self.post_paren:
            # maybe marks are still OK
            if point == self.post_paren[0]:
                skip_post = True
            else:
                self.update(focus, self.post_paren)
                self.post_paren = None

        if not skip_pre:
            c = focus.prior(point)
            if c and c in ')}]':
                m2 = point.dup()
                focus.prev(m2)
                m1 = point.dup()
                focus.call("doc:expr", m1, -1)
                c2 = focus.following(m1)
                if c2+c in "(){}[]":
                    m1['render:paren'] = "open"
                    m2['render:paren'] = "close"
                else:
                    m1['render:paren-mismatch'] = "open"
                    m2['render:paren-mismatch'] = "close"
                self.pre_paren = (m1,m2)
                self.update(focus, self.pre_paren)

        if not skip_post:
            c = focus.following(point)
            if c and c in '({[':
                m1 = point.dup()
                m2 = point.dup()
                focus.call("doc:expr", m2, 1)
                focus.prev(m2)
                c2 = focus.following(m2)
                if c+c2 in "(){}[]":
                    m1['render:paren'] = "open"
                    m2['render:paren'] = "close"
                else:
                    m1['render:paren-mismatch'] = "open"
                    m2['render:paren-mismatch'] = "close"
                self.post_paren = (m1,m2)
                self.update(focus, self.post_paren)

        return 1

    def handle_map_attr(self, key, focus, mark, str, comm2, **a):
        "handle:map-attr"
        if str == "render:paren" and self.pre_paren and (mark in self.pre_paren):
            comm2("cb", focus, "bg:blue+50,bold", 1, 201)
        if str == "render:paren" and self.post_paren and (mark in self.post_paren):
            comm2("cb", focus, "bg:blue+50,bold", 1, 201)
        if str == "render:paren-mismatch" and self.pre_paren and (mark in self.pre_paren):
            comm2("cb", focus, "bg:red+50,bold", 1, 201)
        if str == "render:paren-mismatch" and self.post_paren and (mark in self.post_paren):
            comm2("cb", focus, "bg:red+50,bold", 1, 201)

    def handle_attr(self, key, focus, mark, str, comm2, **a):
        "handle:doc:get-attr"
        if not mark or not str or not comm2:
            return edlib.Efallthrough
        if str == "fill:repeating-prefix":
            if self.indent_type == 'C':
                comm2("cb", focus, mark, "*", str)
            if self.indent_type == 'python':
                comm2("cb", focus, mark, "#", str)
            return 1
        return edlib.Efallthrough

    def handle_para(self, key, focus, mark, num, **a):
        "handle:doc:paragraph"
        # A "Paragraph" is a function, which starts with a line that has no
        # indent, but does have a '('
        backward = 1
        if num > 0:
             backward = 0

        while num:
            try:
                focus.prev(mark) if backward else focus.next(mark)
                l = focus.call("text-search", mark, "^([_a-zA-Z0-9].*\\(|\\()",
                               0, backward)
                if not backward and l > 0:
                    while l > 1:
                        focus.prev(mark)
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
        "handle:doc:expr"
        # Add '_' to list for word chars
        return self.parent.call(key, focus, mark, num, num2, "_")

def c_mode_attach(key, focus, comm2, **a):
    p = CModePane(focus)
    p.indent_type = 'C'
    p['whitespace-single-blank-lines'] = 'yes'
    p['whitespace-max-spaces'] = '7'
    p2 = p.call("attach-whitespace", ret='pane')
    if p2:
        p = p2
    comm2("callback", p)
    return 1

def py_mode_attach(key, focus, comm2, **a):
    p = CModePane(focus)
    p.spaces = 4
    p.indent_type = 'python'
    p['whitespace-indent-space'] = 'yes'
    p['whitespace-single-blank-lines'] = 'yes'
    p2 = p.call("attach-whitespace", ret='pane')
    if p2:
        p = p2
    comm2("callback", p)
    return 1

def c_mode_appeared(key, focus, **a):
    n = focus["filename"]
    if n and n[-2:] in [".c", ".h"]:
        focus["view-default"] = "c-mode"
    return edlib.Efallthrough

def py_mode_appeared(key, focus, **a):
    n = focus["filename"]
    if n and n[-3:] in [".py"]:
        focus["view-default"] = "py-mode"
    return edlib.Efallthrough

def attach_indent(key, focus, **a):
    CModePane(focus)
    return 1

editor.call("global-set-command", "doc:appeared-c-mode", c_mode_appeared)
editor.call("global-set-command", "doc:appeared-py-mode", py_mode_appeared)
editor.call("global-set-command", "attach-c-mode", c_mode_attach)
editor.call("global-set-command", "attach-py-mode", py_mode_attach)
editor.call("global-set-command", "interactive-cmd-indent", attach_indent)
