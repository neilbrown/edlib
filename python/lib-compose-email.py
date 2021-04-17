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

import email.utils
from datetime import date

class compose_email(edlib.Pane):
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus)
        self.view = focus.call("doc:add-view", self) - 1
        self.find_markers()
        m = self.call("doc:vmark-get", self.view, ret='mark')
        if not m:
            self.insert_header_mark()
        st = edlib.Mark(self)
        m = focus.call("doc:point", ret='mark')
        if m == st:
            # At start of file - find a better place.
            # If there is an empty header, go there, else after headers.
            if not self.find_empty_header(m):
                self.to_body(m)

    def add_headers_new(self, key, focus, **a):
        "handle:compose-email:empty-headers"
        fr = focus['email:from']
        if fr:
            nm = focus['email:name']
            if nm:
                fr = "\"%s\" <%s>" %(nm, fr)
        if fr:
            self.check_header("From", fr)
        self.check_header("Subject")
        self.check_header("To")
        self.check_header("Cc")
        m = edlib.Mark(self)
        self.find_empty_header(m)
        self.call("Move-to", m)
        return 1

    def copy_headers(self, key, focus, str, **a):
        "handle:compose-email:copy-headers"
        self.myaddr = None
        # get date - it might be useful
        focus.call("doc:multipart-0-list-headers", "date", self.copy_date)
        # need to collect addresses even if I don't use them
        # so that I can pick the right "from" address
        self.addrlist = []
        focus.call("doc:multipart-0-list-headers", "to", self.copy_addrs)
        focus.call("doc:multipart-0-list-headers", "cc", self.copy_addrs)
        addrs = self.filter_cc(self.addrlist)
        if str != "reply-all":
            addrs = None
        me = self.myaddr
        if not me:
            me = self['email:from']
        if me:
            nm = self['email:name']
            if nm:
                me = "\"%s\" <%s>" %(nm, me)
            self.check_header("From", me)
        if str != "forward":
            focus.call("doc:multipart-0-list-headers", "from", self.copy_to)
        self.pfx = "Re"
        if str == "forward":
            self.pfx = "Fwd"
        self.check_header("To")
        if addrs:
            self.add_addr_header("Cc", addrs)
        focus.call("doc:multipart-0-list-headers", "subject", self.copy_subject)
        self.check_header("Cc")

        self.addrlist = []
        focus.call("doc:multipart-0-list-headers", "references", self.copy_addrs)
        if not self.addrlist:
            focus.call("doc:multipart-0-list-headers", "in-reply-to", self.copy_addrs)
        l = len(self.addrlist)
        focus.call("doc:multipart-0-list-headers", "message-id", self.copy_addrs)
        if l < len(self.addrlist):
            self.add_addr_header("In-reply-to", self.addrlist[l:], True)
        self.add_addr_header("References", self.addrlist, True)

        m = edlib.Mark(self)
        self.find_empty_header(m)
        self.call("Move-to", m)
        return 1

    def copy_date(self, key, focus, str, **a):
        self['date-str'] = str
        d = email.utils.parsedate_tz(str)
        if d:
            self['date-seconds'] = "%d" % email.utils.mktime_tz(d)
        return edlib.Efalse

    def copy_to(self, key, focus, str, **a):
        m2 = self.call("doc:vmark-get", self.view, ret='mark')
        if m2:
            self.call("doc:replace", m2, m2, "To: " + str.strip() + "\n")
        self['reply-author'] = str.strip()
        return edlib.Efalse

    def copy_addrs(self, key, focus, str, **a):
        addr = email.utils.getaddresses([str])
        self.addrlist.extend(addr)
        return 1

    def copy_subject(self, key, focus, str, **a):
        m2 = self.call("doc:vmark-get", self.view, ret='mark')
        if m2:
            self.call("doc:replace", m2, m2, ("Subject: "+ self.pfx +": "
                                              + str.strip() + "\n"))
        return edlib.Efalse

    def filter_cc(self, list):
        # every unique address in list that isn't my address gets added
        # to the new list
        addrs = []
        me = []
        f = self['email:from']
        if f:
            me.append(f)
        af = self['email:altfrom']
        if af:
            for a in af.strip().split("\n"):
                me.append(a.strip())
        dme = []
        af = self['email:deprecated_from']
        if af:
            for a in af.strip().split("\n"):
                dme.append(a.strip())
        ret = []
        for name,addr in list:
            if addr in me:
                if not self.myaddr:
                    self.myaddr = addr
            elif addr not in addrs and addr not in dme:
                addrs.append(addr)
                ret.append((name, addr))
        return ret

    def add_addr_header(self, hdr, addr, need_angle = False):
        prefix = hdr + ": "
        wrap = False
        m2 = self.call("doc:vmark-get", self.view, ret='mark')
        if not m2:
            return
        # Note that we must call doc:replace on parent else
        # we might be caught trying to insert a non-newline immediately
        # before a marker.  We do eventuall insert a newline, so it is safe.
        for n,a in addr:
            if n:
                na = "\"%s\" <%s>" %(n, a)
            elif need_angle:
                na = "<%s>" % a
            else:
                na = a
            if wrap:
                self.parent.call("doc:replace", m2, m2, prefix,
                                 ",render:rfc822header-wrap=%d"%len(prefix))
            else:
                self.parent.call("doc:replace", m2, m2, prefix)
            wrap = True
            prefix = ", "
            self.parent.call("doc:replace", m2, m2, na)
        if wrap:
            # we wrote a header, so write a newline
            self.parent.call("doc:replace", m2, m2, "\n")

    def find_empty_header(self, m):
        m2 = self.call("doc:vmark-get", self.view, ret='mark')
        try:
            self.call("text-search", "^[!-9;-~]+\\s*:\\s*$", m, m2)
            return True
        except:
            return False
    def find_any_header(self, m):
        m2 = self.call("doc:vmark-get", self.view, ret='mark')
        try:
            self.call("text-search", "^[!-9;-~]+\\s*:\\s*", m, m2)
            return True
        except:
            return False
    def to_body(self, m):
        m2 = self.call("doc:vmark-get", self.view, ret='mark')
        if m2:
            m2 = m2.next()
        if m2:
            m.to_mark(m2)

    def handle_quote_content(self, key, focus, str, **a):
        "handle:compose-email:quote-content"
        m = edlib.Mark(self)
        self.to_body(m)
        who = email.utils.getaddresses([self['reply-author']])
        if who and who[0][0]:
            who = who[0][0]
        elif who and who[0][1]:
            who = who[0][1]
        else:
            who = "someone"
        n = self['date-seconds']
        if n:
            d = date.fromtimestamp(int(n))
            when = d.strftime("%a, %d %b %Y")
        else:
            when = 'a recent day'
        q = "On %s, %s wrote:\n" % (when, who)
        for l in str.split("\n"):
            q += '> ' + l + '\n'
        self.call("doc:replace", m, m, q)
        return 1

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
        try:
            self.call("text-search", "^$", m)
        except:
            self.call("doc:set-ref", 0, m)
        m2 = edlib.Mark(orig=m)
        m2.step(0)
        self.parent.call("doc:replace", m2, m, "@#!compose:headers\n",
                         "/markup:func=compose:markup-header/")
        m2['compose-type'] = 'headers'

    def check_header(self, header, content = ""):
        # if header doesn't already exist, at it at end
        m1 = edlib.Mark(self)
        m2 = self.call("doc:vmark-get", self.view, ret='mark')
        try:
            self.call("text-search", "^(?i:%s)\\s*:" % header, m1, m2)
        except:
            self.call("doc:replace", m2, m2, header + ': ' + content+'\n')

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

    def map_attr(self, key, focus, str, str2, mark, comm2, **a):
        "handle:map-attr"
        if not str or not mark or not comm2:
            return edlib.Enoarg

        if str == "render:rfc822header-wrap":
            comm2("attr:callback", focus, int(str2), mark, "wrap", 20)
            return 1

        # get previous mark and see if it is here
        m = self.call("doc:vmark-get", self.view, 3, mark, ret='mark2')
        if not m and str == "start-of-line":
            # start of a header line - set colour for tag and header, and wrap info
            rv = self.call("text-match", "(\\s+|[!-9;-~]+\\s*:)", mark.dup())
            if rv > 0:
                # make space or tag light blue, and body dark blue
                # If tag is unknown, make it grey
                rv2 = self.call("text-match",
                                "?i:(from|to|cc|subject|in-reply-to|references):",
                                mark.dup())
                if rv2 > 0:
                    comm2("cb", focus, mark, rv-1, "fg:blue+30", 2)
                else:
                    comm2("cb", focus, mark, rv-1, "fg:black+30", 2)
                comm2("cb", focus, mark, 100000, "fg:blue-70,bold", 1)
            else:
                # make whole line red
                comm2("cb", focus, mark, 100000, "bg:red+50,fg:red-50", 2)
            comm2("cb", focus, mark, 10000, "wrap-tail: ,wrap-head:    ", 20)
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

    def handle_tab(self, key, focus, mark, **a):
        "handle:K:Tab"
        m2 = self.call("doc:vmark-get", self.view, ret='mark')
        if mark <= m2:
            # in headers, TAB goes to next header, or body
            if not self.find_any_header(mark):
                self.to_body(mark)
            return 1
        return edlib.Efallthrough
    def handle_s_tab(self, key, focus, mark, **a):
        "handle:K:S:Tab"
        m2 = self.call("doc:vmark-get", self.view, ret='mark')
        if mark > m2:
            # After header, S:Tab goes to last header
            m = edlib.Mark(self)
            mark.to_mark(m)
            while self.find_any_header(m):
                mark.to_mark(m)
            return 1
        # in headers, go to previous header
        m = edlib.Mark(self)
        m2 = mark.dup()
        while self.find_any_header(m) and m < m2:
            mark.to_mark(m)
        return 1


def compose_mode_attach(key, focus, comm2, **a):
    focus['fill-width'] = '72'
    p = focus.call("attach-textfill", ret='focus')
    if not p:
        p = focus
    p = compose_email(p)
    if comm2:
        comm2("cb", p)
    return 1

editor.call("global-set-command", "attach-compose-email", compose_mode_attach)
