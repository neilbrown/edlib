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
import email.message
import email.policy
import email.headerregistry
import tempfile
from datetime import date

def read_status(p, key, focus, **a):
    out, err = p.communicate()
    focus.call("Message", "Email submission complete")
    edlib.LOG("Email submission reported: " + out.decode() + err.decode())
    return edlib.Efalse

class compose_email(edlib.Pane):
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus)
        self.view = focus.call("doc:add-view", self) - 1
        self.complete_start = None
        self.find_markers()
        m, l = self.vmarks(self.view)
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
        self.check_header("To")
        self.check_header("Cc")
        self.check_header("Subject")
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
        to_addrs = self.filter_cc(self.addrlist)
        self.addrlist = []
        focus.call("doc:multipart-0-list-headers", "cc", self.copy_addrs)
        cc_addrs = self.filter_cc(self.addrlist)
        self.addrlist = []
        focus.call("doc:multipart-0-list-headers", "reply-to", self.copy_addrs)
        if not self.addrlist:
            focus.call("doc:multipart-0-list-headers", "from", self.copy_addrs)
        from_addrs = self.filter_cc(self.addrlist)
        if str != "reply-all":
            to_addrs = None
            cc_addrs = None
        me = self.myaddr
        if not me:
            me = self['email:from']
        if me:
            nm = self['email:name']
            if nm:
                me = "\"%s\" <%s>" %(nm, me)
            self.check_header("From", me)
        if from_addrs:
            n,a = from_addrs[0]
            self['reply-author'] = n if n else a

        if str != "forward":
            if from_addrs:
                self.add_addr_header("To", from_addrs)
                if not to_addrs:
                    to_addrs = cc_addrs
                elif cc_addrs:
                    to_addrs += cc_addrs
                self.add_addr_header("Cc", to_addrs)
            else:
                self.add_addr_header("To", to_addrs)
                self.add_addr_header("Cc", cc_addrs)

        self.pfx = "Re"
        if str == "forward":
            self.pfx = "Fwd"
        self.check_header("To")
        self.check_header("Cc")
        focus.call("doc:multipart-0-list-headers", "subject", self.copy_subject)

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

    def copy_addrs(self, key, focus, str, **a):
        addr = email.utils.getaddresses([str])
        self.addrlist.extend(addr)
        return 1

    def copy_subject(self, key, focus, str, **a):
        m2, l = self.vmarks(self.view)
        if m2:
            subj = str.strip()
            if not subj.lower().startswith(self.pfx.lower() + ':'):
                subj = self.pfx + ': ' + subj
            self.call("doc:replace", m2, m2, ("Subject: "+ subj + "\n"))
        return edlib.Efalse

    def from_lists(self):
        me = []
        f = self['email:from']
        if f:
            me.append(f)
        af = self['email:altfrom']
        if af:
            for a in af.strip().split("\n"):
                a = a.strip()
                if a not in me:
                    me.append(a.strip())
        dme = []
        af = self['email:deprecated_from']
        if af:
            for a in af.strip().split("\n"):
                a = a.strip()
                if a not in dme:
                    dme.append(a.strip())
        return (me, dme)

    def filter_cc(self, list):
        # every unique address in list that isn't my address gets added
        # to the new list
        addrs = []
        ret = []
        me, dme = self.from_lists()
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
        m2, l = self.vmarks(self.view)
        if not m2:
            return
        if not addr:
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
        m2,l = self.vmarks(self.view)
        try:
            self.call("text-search", "^[!-9;-~]+\\h*:\\h*$", m, m2)
            return True
        except:
            return False
    def find_any_header(self, m):
        m2, l = self.vmarks(self.view)
        try:
            self.call("text-search", "^[!-9;-~]+\\h*:\\h*", m, m2)
            return True
        except:
            return False
    def to_body(self, m):
        m2, l = self.vmarks(self.view)
        if m2:
            m2 = m2.next()
        if m2:
            m.to_mark(m2)

    def handle_quote_content(self, key, focus, str, **a):
        "handle:compose-email:quote-content"
        m = edlib.Mark(self)
        self.to_body(m)
        who = self['reply-author']
        if not who:
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

            self.parent.call("doc:EOL", -1, m)
            m1 = edlib.Mark(self, self.view)
            m1.to_mark(m)
            self.parent.call("doc:set-attr", m,
                             "markup:func", "compose:markup-header")
            m2 = edlib.Mark(orig=m1)
            self.parent.call("doc:EOL", 1, m2, 1)
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
        m2, l = self.vmarks(self.view)
        try:
            self.call("text-search", "^(?i:%s)\\h*:" % header, m1, m2)
        except:
            self.call("doc:replace", m2, m2, header + ': ' + content+'\n')

    def markup_header(self, key, focus, num, mark, mark2, comm2, **a):
        "handle:compose:markup-header"
        # Display cursor at start or end, anything else should be suppressed.
        if num == 0 or (mark2 and mark2 == mark):
            # appear at the start of the line
            return comm2("cb", focus, "<fg:red>")
        # at least go to end of line
        self.parent.call("doc:EOL", 1, mark)
        m = self.vmark_at_or_before(self.view, mark)
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

    def map_attr(self, key, focus, str, str2, mark, comm2, **a):
        "handle:map-attr"
        if not str or not mark or not comm2:
            return edlib.Enoarg

        if str == "render:rfc822header-wrap":
            comm2("attr:callback", focus, int(str2), mark, "wrap", 30)
            return 1

        # get previous mark and see if it is here
        m = self.vmark_at_or_before(self.view, mark)
        if not m and str == "start-of-line":
            # start of a header line - set colour for tag and header, and wrap info
            rv = self.call("text-match", "(\\h+|[!-9;-~]+\\h*:)", mark.dup())
            if rv > 0:
                # make space or tag light blue, and body dark blue
                # If tag is unknown, make it grey
                rv2 = self.call("text-match",
                                "?i:(from|to|cc|subject|in-reply-to|references|date|message-id):",
                                mark.dup())
                if rv2 > 0:
                    comm2("cb", focus, mark, rv-1, "fg:blue+30", 20)
                else:
                    comm2("cb", focus, mark, rv-1, "fg:black+30", 20)
                comm2("cb", focus, mark, 100000, "fg:blue-70,bold", 10)
            else:
                # make whole line red
                comm2("cb", focus, mark, 100000, "bg:red+50,fg:red-50", 20)
            comm2("cb", focus, mark, 10000, "wrap-tail: ,wrap-head:    ", 1)
            return edlib.Efallthrough
        if m and str == 'start-of-line':
            # if line starts '>', give it some colour
            if focus.following(mark) == '>':
                colours = ['red', 'red-60', 'green-60', 'magenta-60']
                m = mark.dup()
                cnt = 0
                c = focus.next(m)
                while c and c in ' >':
                    if c == '>':
                        cnt += 1
                    c = focus.next(m)

                if cnt >= len(colours):
                    cnt = len(colours)
                comm2("cb", focus, mark, 10000, "fg:"+colours[cnt-1], 20)
            return edlib.Efallthrough

        return edlib.Efallthrough

    def handle_replace(self, key, focus, mark, mark2, str, **a):
        "handle:doc:replace"
        self.complete_start = None
        self.complete_end = None
        self.complete_list = None
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

        m = self.vmark_at_or_before(self.view, mark)
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
        m2 = self.vmark_at_or_before(self.view, mark2)
        if m2 and m2.prev() == m and mark2 == m2:
            # deleting text just before a marker is OK as long as we will have
            # a newline before the marker
            if (str and str[-1] == '\n') or focus.prior(mark) == '\n':
                return edlib.Efallthrough
        if m2 != m:
            # not completely within a part, so fail
            return 1
        # deleting/replacing something after m/m2.
        if m:
            m.step(0)
        return edlib.Efallthrough

    def handle_doc_char(self, key, focus, mark, num, num2, mark2, **a):
        "handle:doc:char"
        if not mark:
            return edlib.Enoarg
        end = mark2
        steps = num
        forward = 1 if steps > 0 else 0
        if end and end == mark:
            return 1
        if end and (end < mark) != (steps < 0):
            # can never cross 'end'
            return edlib.Einval
        ret = edlib.Einval
        while steps and ret != edlib.WEOF and (not end or mark == end):
            ret = self.handle_doc_step(key, mark, forward, 1)
            steps -= forward * 2 - 1
        if end:
            return 1 + (num - steps if forward else steps - num)
        if ret == edlib.WEOF or num2 == 0:
            return ret
        if num and (num2 < 0) == (num > 0):
            return ret
        # want the next character
        return self.handle_doc_step(key, mark, 1 if num2 > 0 else 0, 0)

    def handle_doc_step(self, key, mark, num, num2):
        # if in a marker, only allow a space and newline to be seen
        if not mark:
            return edlib.Enoarg
        m = self.vmark_at_or_before(self.view, mark)
        if mark == m:
            if m['compose-type']:
                # at start of marker
                if num > 0:
                    # forward
                    if num2:
                        self.parent.call("doc:EOL", 1, mark)
                    return ' '
                else:
                    # backward
                    return self.parent.call("doc:char", mark,
                                            -1 if num2 else 0, 0 if num2 else -1)
            else:
                # at end of marker
                if num > 0:
                    return self.parent.call("doc:char", mark,
                                            1 if num2 else 0, 0 if num2 else 1)
                else:
                    return self.parent.call("doc:char", mark,
                                            -1 if num2 else 0, 0 if num2 else -1)
        if not m or not m['compose-type']:
            # not in a marker
            if num > 0:
                return self.parent.call("doc:char", mark,
                                        1 if num2 else 0, 0 if num2 else 1)
            else:
                return self.parent.call("doc:char", mark,
                                        -1 if num2 else 0, 0 if num2 else -1)

        # should be just before newline
        if num > 0:
            #forward, return newline
            if num2:
                self.parent.call("doc:EOL", 1, mark)
                self.parent.next(mark)
            return '\n'
        else:
            # backward, return space
            if num2:
                self.parent.call("doc:EOL", -1, mark)
            return ' '

    def handle_doc_get_attr(self, key, focus, mark, str, comm2, **a):
        "handle:doc:get-attr"
        if not mark or not str or not comm2 or not str.startswith("fill:"):
            return edlib.Efallthrough
        m = self.vmark_at_or_before(self.view, mark)
        if m:
            return edlib.Efallthrough
        # in headers, need a min-prefix and start-re for fill
        if str == "fill:default-prefix":
            comm2("cb", focus, mark, "  ", str)
            return 1
        if str == "fill:start-re" or str == "fill:end-re" :
            comm2("cb", focus, mark, "^($|[^\\s])", str)
            return 1

    def try_address_complete(self, m):
        this = self.this_header(m.dup())
        if not this or this not in ["to","cc"]:
            return False
        st = m.dup()
        word = self.prev_addr(st)
        if not word:
            return False
        if self.complete_start and self.complete_end == m:
            self.parent.call("doc:replace", self.complete_start, m,
                             self.complete_list[self.complete_next])
            self.complete_end.step(0)
            self.complete_next += 1
            if self.complete_next >= len(self.complete_list):
                self.call("Message", "last completion - will cycle around")
                self.complete_next = 0
            return True
        if ('@' in word and '.' in word and
            ('>' in word or '<' not in word)):
            # looks convincing
            return False
        p = subprocess.Popen(["notmuch-addr", word],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        out,err = p.communicate()
        if not out:
            if err:
                self.call("Message", "Address expansion gave error: %s" % err.decode())
                return True
            self.call("Message", "No completions found for address %s" % word)
            return True
        self.complete_start = None
        self.complete_list = out.decode().strip().split("\n")
        if len(self.complete_list) > 1:
            self.call("Message", "%d completions found for address %s"
                      % (len(self.complete_list), word))
            self.complete_start = st
            self.complete_end = m.dup()
            self.complete_next = 1
        else:
            self.call("Message", "only 1 completion found for address %s"
                      % word)
        self.parent.call("doc:replace", st, m, self.complete_list[0])
        if self.complete_start:
            self.complete_end.step(0)
        return True

    def try_cycle_from(self, m):
        start = m.dup()
        end = m.dup()
        this = self.this_header(start, end)
        if not this or this != "from":
            return False
        current = self.call("doc:get-str", start, end, ret='str')
        name, addr = email.utils.parseaddr(current)
        me, dme = self.from_lists()
        if not me:
            self.call("Message", "No From addresses declared")
            return True
        try:
            i = me.index(addr) + 1
        except ValueError:
            i = 0
        if i < 0 or i >= len(me):
            addr = me[0]
        else:
            addr = me[i]
        self.call("doc:replace", start, end,
                  "%s <%s>" % (name, addr))
        m.to_mark(end)
        return True

    def this_header(self, mark, end = None, downcase = True):
        try:
            l = self.call("text-search", "^[!-9;-~]+\\h*:", 0, 1, mark)
            m1 = mark.dup()
            while l > 2:
                l -= 1
                self.next(mark)
            s = self.call("doc:get-str", m1, mark, ret='str')
            self.next(mark)
            while self.following(mark) == ' ':
                self.next(mark)
            if downcase:
                ret = s.strip().lower()
            else:
                ret = s.strip()
        except edlib.commandfailed:
            return None
        if not end:
            return ret
        # Now find the body - mark is at the start
        end.to_mark(mark)
        mbody, l = self.vmarks(self.view)
        try:
            self.call("text-search", "^[!-9;-~]", end, mbody)
        except:
            end.to_mark(mbody)
        self.call("doc:EOL", -1, 1, end)
        return ret

    def prev_addr(self, m):
        # if previous char is not a space, collect everything
        # back to , or : or \n and return 1
        c = self.prev(m)
        if not c or c in " \n":
            return None
        a = ''
        while c and c not in ",:\n":
            a = c + a
            c = self.prev(m)
        while c and c in ",: \n":
            c = self.next(m)
        if c:
            self.prev(m)
        return a.strip()

    def handle_tab(self, key, focus, mark, **a):
        "handle:K:Tab"
        m2, l = self.vmarks(self.view)
        if mark > m2:
            return edlib.Efallthrough
        # in headers, TAB does various things:
        # In 'to' or 'cc' this preceeding word looks like an
        # incomplete address, then address completion is tried.
        # Otherwise goes to next header if there is one, else to
        # the body
        if (not self.try_address_complete(mark) and
            not self.try_cycle_from(mark) and
            not self.find_any_header(mark)):
            self.to_body(mark)
        return 1
    def handle_s_tab(self, key, focus, mark, **a):
        "handle:K:S:Tab"
        m2, l = self.vmarks(self.view)
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

    def handle_commit(self, key, focus, **a):
        "handle:Commit"
        msg = email.message.EmailMessage()
        m, l = self.vmarks(self.view)
        if m:
            m = m.next()
        else:
            return edlib.Efail
        if m.next():
            m2 = m.next()
        else:
            m2 = m.dup()
            focus.call("doc:file", 1, m2)
        txt = focus.call("doc:get-str", m, m2, ret='str')
        msg.set_content(txt)
        self.check_header("Date", email.utils.formatdate(localtime=True))
        self.check_header("Message-id",
                          email.utils.make_msgid(
                              domain=focus['email:host-address']))
        h = edlib.Mark(focus)
        while self.find_empty_header(h):
            focus.call("doc:EOL", -1, h)
            h2 = h.dup()
            focus.call("doc:EOL", 1, 1, h)
            focus.call("doc:replace", h, h2)


        h = edlib.Mark(focus)
        whoto = None
        while self.find_any_header(h):
            he = h.dup()
            nm = self.this_header(h, he, downcase=False)
            if not nm:
                break
            bdy = focus.call("doc:get-str", h, he, ret='str')
            bdy = bdy.strip()
            if bdy:
                msg[nm] = bdy
            if nm.lower() == "to" and not whoto:
                try:
                    n,a = email.utils.parseaddr(bdy)
                    if n:
                        whoto = n
                    else:
                        whoto = a
                except:
                    pass

        s = msg.as_string()
        tf = tempfile.TemporaryFile()
        tf.write(s.encode())
        tf.seek(0)
        sendmail = focus['email:sendmail']
        if not sendmail:
            sendmail = "/sbin/sendmail -i"
        try:
            p = subprocess.Popen(sendmail.split(),
                                 stdin = tf.fileno(),
                                 stdout = subprocess.PIPE,
                                 stderr = subprocess.PIPE)
        except:
            p = None
        if not p:
            focus.call("Message", "Failed to run sendmail command")
            edlib.LOG("%s failed", sendmail)
            return 1
        if not whoto:
            whoto = "someone"
        focus.call("doc:set-name", "*Sent message to %s*" % whoto)
        focus.call("doc:set:email-sent", "yes")
        focus.call("doc:modified", -1)
        fn = self['filename']
        if fn and fn.startswith('/'):
            os.unlink(fn)

        root = self.call("RootPane", ret='pane')
        if root:
            root.call("event:read", p.stdout.fileno(),
                      lambda key, **a: read_status(p, key, **a))
            focus.call("Message", "Queueing message to %s." % whoto)
            focus.call("Window:bury")
            return 1

        # Cannot find pane to report status on, so do it sync
        out, err = p.communicate()
        if out:
            edlib.LOG("Email submission says:", out.decode())
        if err:
            focus.call("Message", "Email submission gives err: " + err.decode())
        focus.call("Message", "Email message to %s queued." % whoto)
        focus.call("Window:bury")

        return 1

    def handle_spell(self, key, focus, mark, **a):
        "handle:Spell:NextWord"
        m2, l = self.vmarks(self.view)
        if not mark or not m2 or not m2.next() or mark > m2:
            return edlib.Efallthrough
        found_end = False
        while not found_end:
            h = self.this_header(mark.dup())
            if h and h.lower() in ["subject"]:
                return edlib.Efallthrough
            found_end = not self.find_any_header(mark)
        mark.to_mark(m2.next())
        return edlib.Efallthrough

def compose_mode_attach(key, focus, comm2, **a):
    focus['fill-width'] = '72'
    p = focus.call("attach-textfill", ret='pane')
    if p:
        focus = p
    p = focus.call("attach-autospell", ret='pane')
    if p:
        focus = p
    p = compose_email(focus)
    if comm2:
        comm2("cb", p)
    return 1

editor.call("global-set-command", "attach-compose-email", compose_mode_attach)
