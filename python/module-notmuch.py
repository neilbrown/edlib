# -*- coding: utf-8 -*-
# Copyright Neil Brown ©2016-2023 <neil@brown.name>
# May be distributed under terms of GPLv2 - see file:COPYING
#
# edlib module for working with "notmuch" email.
#
# Two document types:
# - search list: list of saved searches with count of 'current', 'unread',
#   and 'new' messages
# - message list: provided by notmuch-search, though probably with enhanced threads
#
# Messages and composition is handled by a separte 'email' module.  This
# module will send messages to that module and provide services for composing
# and delivery.
#
# These can be all in with one pane, with sub-panes, or can sometimes
# have a pane to themselves.
#
# saved search are stored in config (file or database) as "query.foo"
# Some are special.
# "query.current" selects messages that have not been archived, and are not spam
# "query.unread" selects messages that should be highlighted. It is normally
# "tag:unread"
# "query.new" selects new messages. Normally "tag:new not tag:unread"
# "query.current-list" should be a conjunction of "query:" searches.  They are listed
#  in the "search list" together with a count of 'current' and 'current/new' messages.
# "query.misc-list" is a subset of current-list for which query:current should not
# be assumed.
# "query.from-list" is a list of other queries for which it is meaningful
#   to add "from:address" clauses for some address.

import edlib

from subprocess import Popen, PIPE, DEVNULL, TimeoutExpired
import re
import tempfile
import os, fcntl
import json
import time
import mimetypes
import email.utils

def cvt_size(n):
    if n < 1000:
        return "%3db" % n
    if n < 10000:
        return "%3.1fK" % (float(n)/1000.0)
    if n < 1000000:
        return "%3dK" % (n/1000)
    if n < 10000000:
        return "%3.1fM" % (float(n)/1000000.0)
    if n < 1000000000:
        return "%3dM" % (n/1000000)
    return "BIG!"

def notmuch_get_tags(msg=None,thread=None):
    if msg:
        query = "id:" + msg
    elif thread:
        query = "thread:" + thread
    else:
        query = '*'

    p = Popen(["/usr/bin/notmuch","search","--output=tags",query],
              stdout = PIPE, stderr = DEVNULL)
    if not p:
        return
    try:
        out,err = p.communicate(timeout=5)
    except IOError:
        return
    except TimeoutExpired:
        p.kill()
        out,err = p.communicate()
        return
    return out.decode("utf-8","ignore").strip('\n').split('\n')

def notmuch_get_files(msg):
    query = "id:" + msg

    p = Popen(["/usr/bin/notmuch","search","--output=files", "--format=text0",
               query],
              stdout = PIPE, stderr = DEVNULL)
    if not p:
        return
    try:
        out,err = p.communicate(timeout=5)
    except IOError:
        return
    except TimeoutExpired:
        p.kill()
        out,err = p.communicate()
        return
    return out.decode("utf-8","ignore").strip('\0').split('\0')

def notmuch_set_tags(msg=None, thread=None, add=None, remove=None):
    if not add and not remove:
        return
    if msg:
        query = "id:" + msg
    elif thread:
        query = "thread:" + thread
    else:
        return
    argv = ["/usr/bin/notmuch","tag"]
    if add:
        for i in add:
            argv.append("+" + i)
    if remove:
        for i in remove:
            argv.append("-" + i)
    argv.append(query)
    p = Popen(argv, stdin = DEVNULL, stdout = DEVNULL, stderr = DEVNULL)
    # FIXME I have to wait so that a subsequent 'get' works.
    p.communicate()

def notmuch_start_load_thread(tid, query=None):
    if query:
        q = "thread:%s and (%s)" % (tid, query)
    else:
        q = "thread:%s" % (tid)
    argv = ["/usr/bin/notmuch", "show", "--format=json", q]
    p = Popen(argv, stdin = DEVNULL, stdout = PIPE, stderr = DEVNULL)
    return p

def notmuch_load_thread(tid, query=None):
    p = notmuch_start_load_thread(tid, query)
    out,err = p.communicate()
    if not out:
        return None
    # we sometimes sees "[[[[]]]]" as the list of threads,
    # which isn't properly formatted. So add an extr check on
    # r[0][0][0]
    # This can happen when the file containing message cannot be found.
    r = json.loads(out.decode("utf-8","ignore"))
    if not r or type(r[0][0][0]) != dict:
        return None
    # r is a list of threads, we want just one thread.
    return r[0]

class counter:
    # manage a queue of queries that need to be counted.
    def __init__(self, make_search, pane, cb):
        self.make_search = make_search
        self.pane = pane
        self.cb = cb
        self.queue = []
        self.pending = None
        self.p = None

    def enqueue(self, q, priority = False):
        if priority:
            if q in self.queue:
                self.queue.remove(q)
            self.queue.insert(0, q)
        else:
            if q in self.queue:
                return
            self.queue.append(q)
        self.next()

    def is_pending(self, q):
        if self.pending == q:
            return 1 # first
        if q in self.queue:
            return 2 # later
        return 0

    def next(self):
        if self.p:
            return True
        if not self.queue:
            return False
        q = self.queue.pop(0)
        self.pending = q
        self.p = Popen("/usr/bin/notmuch count --batch", shell=True, stdin=PIPE,
                       stdout = PIPE, stderr = DEVNULL)
        try:
            self.p.stdin.write((self.make_search(q) + "\n").encode("utf-8"))
            self.p.stdin.write((self.make_search(q, 'unread') + "\n").encode("utf-8"))
            self.p.stdin.write((self.make_search(q, 'new') + "\n").encode("utf-8"))
        except BrokenPipeError:
            pass
        self.p.stdin.close()
        self.start = time.time()
        self.pane.call("event:read", self.p.stdout.fileno(), self.ready)
        return True

    def ready(self, key, **a):
        q = self.pending
        count = 0; unread = 0; new = 0
        slow = time.time() - self.start > 5
        self.pending = None
        try:
            c = self.p.stdout.readline()
            count = int(c)
            u = self.p.stdout.readline()
            unread = int(u)
            nw = self.p.stdout.readline()
            new = int(nw)
        except:
            pass
        p = self.p
        self.p = None
        more = self.next()
        self.cb(q, count, unread, new, slow, more)
        p.wait()
        # return False to tell event handler there is no more to read.
        return edlib.Efalse

class searches:
    # Manage the saved searches
    # We read all searches from the config file and periodically
    # update some stored counts.
    #
    # This is used to present the search-list document.
    def __init__(self, pane, cb):
        self.slist = {}
        self.current = []
        self.misc = []
        self.count = {}
        self.unread = {}
        self.new = {}
        self.tags = []
        self.slow = {}
        self.worker = counter(self.make_search, pane, self.updated)
        self.slow_worker = counter(self.make_search, pane, self.updated)
        self.cb = cb

        if 'NOTMUCH_CONFIG' in os.environ:
            self.path = os.environ['NOTMUCH_CONFIG']
        elif 'HOME' in os.environ:
            self.path = os.environ['HOME'] + "/.notmuch-config"
        else:
            self.path = ".notmuch-config"
        self.mtime = 0
        self.maxlen = 0

    def set_tags(self, tags):
        self.tags = list(tags)

    def load(self, reload = False):
        try:
            stat = os.stat(self.path)
            mtime = stat.st_mtime
        except OSError:
            mtime = 0
        if not reload and mtime <= self.mtime:
            return False

        p = Popen("notmuch config list", shell=True, stdout=PIPE)
        if not p:
            return False
        self.slist = {}
        for line in p.stdout:
            line = line.decode("utf-8", "ignore")
            if not line.startswith('query.'):
                continue
            w = line[6:].strip().split("=", 1)
            self.slist[w[0]] = w[1]
            if len(w[0]) > self.maxlen:
                self.maxlen = len(w[0])
        try:
            p.communicate()
        except IOError:
            pass
        if "current-list" not in self.slist:
            self.slist["current-list"] = "query:inbox query:unread"
            if "current" not in self.slist:
                self.slist["misc-list"] = "query:inbox query:unread"
            if "inbox" not in self.slist:
                self.slist["inbox"] = "tag:inbox"
            if "unread" not in self.slist:
                self.slist["unread"] = "tag:inbox AND tag:unread"

        if "misc-list" not in self.slist:
            self.slist["misc-list"] = ""
        if "unread" not in self.slist:
            self.slist["unread"] = "tag:unread"
        if "new" not in self.slist:
            self.slist["new"] = "(tag:new AND tag:unread)"

        self.current = self.searches_from("current-list")
        self.misc = self.searches_from("misc-list")

        self.slist["-ad hoc-"] = ""
        self.current.append("-ad hoc-")
        self.misc.append("-ad hoc-")

        for t in self.tags:
            tt = "tag:" + t
            if tt not in self.slist:
                self.slist[tt] = tt
                self.current.append(tt)
                self.misc.append(tt)

        for i in self.current:
            if i not in self.count:
                self.count[i] = None
                self.unread[i] = None
                self.new[i] = None
        self.mtime = mtime

        return True

    def is_pending(self, search):
        return self.worker.is_pending(search) + self.slow_worker.is_pending(search)

    def update(self):
        for i in self.current:
            if not self.slist[i]:
                # probably an empty -ad hoc-
                continue
            if i in self.slow:
                self.slow_worker.enqueue(i)
            else:
                self.worker.enqueue(i)
        return self.worker.pending != None and self.slow_worker.pending != None

    def update_one(self, search):
        if not self.slist[search]:
            return
        if search in self.slow:
            self.slow_worker.enqueue(search, True)
        else:
            self.worker.enqueue(search, True)

    def updated(self, q, count, unread, new, slow, more):
        changed = (self.count[q] != count or
                   self.unread[q] != unread or
                   self.new[q] != new)
        self.count[q] = count
        self.unread[q] = unread
        self.new[q] = new
        if slow:
            self.slow[q] = slow
        elif q in self.slow:
            del self.slow[q]
        self.cb(q, changed,
                self.worker.pending == None and
                self.slow_worker.pending == None)

    patn = "\\bquery:([-_A-Za-z0-9]*)\\b"
    def map_search(self, query):
        m = re.search(self.patn, query)
        while m:
            s = m.group(1)
            if s in self.slist:
                q = self.slist[s]
                query = re.sub('\\bquery:' + s + '\\b',
                               '(' + q + ')', query)
            else:
                query = re.sub('\\bquery:' + s + '\\b',
                               'query-'+s, query)
            m = re.search(self.patn, query)
        return query

    def make_search(self, name, extra = None):
        s = '(' + self.slist[name] + ')'
        if name not in self.misc:
            s = s + " AND query:current"
        if extra:
            s = s + " AND query:" + extra
        return self.map_search(s)

    def searches_from(self, n):
        ret = []
        if n in self.slist:
            for s in self.slist[n].split(" "):
                if s.startswith('query:'):
                    ret.append(s[6:])
        return ret

def make_composition(db, focus, which = "PopupTile", how = "MD3tsa", tag = None):
    dir = db['config:database.path']
    if not dir:
        dir = "/tmp"
    drafts = os.path.join(dir, "Drafts")
    try:
        os.mkdir(drafts)
    except FileExistsError:
        pass

    fd, fname = tempfile.mkstemp(dir=drafts)
    os.close(fd)
    m = focus.call("doc:open", fname, -1, ret='pane')
    m.call("doc:set-name", "*Unsent mail message*")
    # always good to have a blank line, incase we add an attachment early.
    m.call("doc:replace", "\n")
    m['view-default'] = 'compose-email'
    m['email-sent'] = 'no'
    name = db['config:user.name']
    mainfrom = db['config:user.primary_email']
    altfrom = db['config:user.other_email']
    altfrom2 = db['config:user.other_email_deprecated']
    host_address = db['config:user.host_address']
    if name:
        m['email:name'] = name
    if mainfrom:
        m['email:from'] = mainfrom
    if altfrom:
        m['email:altfrom'] = altfrom
    if altfrom2:
        m['email:deprecated_from'] = altfrom2
    if host_address:
        m['email:host-address'] = host_address
    set_tag = ""
    if tag:
        set_tag = "/usr/bin/notmuch %s;" % tag
    m['email:sendmail'] = set_tag + "/usr/bin/notmuch insert --folder=sent --create-folder -new -unread +outbox"
    # NOTE this cannot be in ThisPane, else the pane we want to copy
    # content from will disappear.
    # I think Popuptile is best, with maybe an option to expand it
    # after the copy is done.
    if which != "PopupTile":
        how = None
    p = focus.call(which, how, ret='pane')
    if not p:
        return edlib.Efail
    v = m.call("doc:attach-view", p, 1, ret='pane')
    if v:
        v.take_focus()
    return v

# There are two document types.
#   notmuch_main presents all the saved searches, and also represents the database
#      of all messages.  There is only one of these.
#   notmuch_query presents a single query as a number of threads, each with a number
#      of messages.

class notmuch_main(edlib.Doc):
    # This is the document interface for the saved-search list.
    # It contains the searches as items which have attributes
    # providing name, count, unread-count
    # Once activated it auto-updates every 5 minutes
    # Updating first handled "counts" where the 3 counters for each
    # saved search are checked, then "queries" which each current
    # thread-list document is refreshed.
    #
    # We create a container pane to collect all these thread-list documents
    # to hide them from the general *Documents* list.
    #
    # Only the 'offset' of doc-references is used.  It is an index
    # into the list of saved searches.
    #

    def __init__(self, focus):
        edlib.Doc.__init__(self, focus)
        self.searches = searches(self, self.updated)
        self.timer_set = False
        self.updating = False
        self.querying = False
        self.container = edlib.Pane(self.root)
        self.changed_queries = []

    def handle_shares_ref(self, key, **a):
        "handle:doc:shares-ref"
        return 1

    def handle_close(self, key, **a):
        "handle:Close"
        self.container.close()
        return 1

    def handle_val_mark(self, key, mark, mark2, **a):
        "handle:debug:validate-marks"
        if not mark or not mark2:
            return edlib.Enoarg
        if mark.pos == mark2.pos:
            if mark.offset < mark2.offset:
                return 1
            edlib.LOG("notmuch_main val_marks: same pos, bad offset:",
                      mark.offset, mark2.offset)
            return edlib.Efalse
        if mark.pos is None:
            edlib.LOG("notmuch_main val_mark: mark.pos is None")
            return edlib.Efalse
        if mark2.pos is None:
            return 1
        if mark.pos < mark2.pos:
            return 1
        edlib.LOG("notmuch_main val_mark: pos in wrong order:",
                  mark.pos, mark2.pos)
        return edlib.Efalse

    def handle_set_ref(self, key, mark, num, **a):
        "handle:doc:set-ref"
        self.to_end(mark, num != 1)
        if num == 1:
            mark.pos = 0
        else:
            mark.pos = len(self.searches.current)
        if mark.pos == len(self.searches.current):
            mark.pos = None
        mark.offset = 0
        return 1

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
            ret = self.handle_step(key, mark, forward, 1)
            steps -= forward * 2 - 1
        if end:
            return 1 + (num - steps if forward else steps - num)
        if ret == edlib.WEOF or num2 == 0:
            return ret
        if num and (num2 < 0) == (num > 0):
            return ret
        # want the next character
        return self.handle_step(key, mark, 1 if num2 > 0 else 0, 0)

    def handle_step(self, key, mark, num, num2):
        forward = num
        move = num2
        ret = edlib.WEOF
        target = mark
        if mark.pos is None:
            pos = len(self.searches.current)
        else:
            pos = mark.pos
        if forward and not mark.pos is None:
            ret = '\n'
            if move:
                mark.step_sharesref(forward)
                mark.pos = pos + 1
                mark.offset = 0
                if mark.pos == len(self.searches.current):
                    mark.pos = None
        if not forward and pos > 0:
            ret = '\n'
            if move:
                mark.step_sharesref(forward)
                mark.pos = pos - 1
                mark.offset = 0
        return ret

    def handle_doc_get_attr(self, key, focus, mark, str, comm2, **a):
        "handle:doc:get-attr"
        # This must support the line-format used in notmuch_list_view

        attr = str
        o = mark.pos
        val = None
        if not o is None and o >= 0:
            s = self.searches.current[o]
            if attr == 'query':
                val = s
            elif attr == 'fmt':
                if self.searches.new[s]:
                    val = "bold,fg:red"
                elif self.searches.unread[s]:
                    val = "bold,fg:blue"
                elif self.searches.count[s]:
                    val = "fg:black"
                else:
                    val = "fg:grey"
                if focus['qname'] == s:
                    if focus['filter']:
                        val = "bg:red+60,"+val
                    else:
                        val = "bg:yellow+20,"+val
            elif attr == 'name':
                val = s
            elif attr == 'count':
                c = self.searches.new[s]
                if not c:
                    c = self.searches.unread[s]
                if not c:
                    c = self.searches.count[s]
                if c is None:
                    val = "%5s" % "?"
                elif c < 100000:
                    val = "%5d" % c
                elif c < 10000000:
                    val = "%4dK" % int(c/1000)
                else:
                    val = "%4dM" % int(c/1000000)
            elif attr == 'space':
                p = self.searches.is_pending(s)
                if p == 1:
                    val = '*'
                elif p > 1:
                    val = '?'
                elif s in self.searches.slow:
                    val = '!'
                else:
                    val = ' '
        if val:
            comm2("callback", focus, val, mark, str)
            return 1
        return edlib.Efallthrough

    def handle_get_attr(self, key, focus, str, comm2, **a):
        "handle:get-attr"
        if not comm2 or not str:
            return edlib.Enoarg
        if str == "doc-type":
            comm2("callback", focus, "notmuch")
            return 1
        if str == "notmuch:max-search-len":
            comm2("callback", focus, "%d" % self.searches.maxlen)
            return 1
        if str.startswith('config:'):
            p = Popen(['/usr/bin/notmuch', 'config', 'get', str[7:]],
                      close_fds = True,
                      stderr = PIPE, stdout = PIPE)
            out,err = p.communicate()
            p.wait()
            if out:
                comm2("callback", focus,
                      out.decode("utf-8","ignore").strip(), str)
            return 1
        return edlib.Efallthrough

    def handle_request_notify(self, key, focus, **a):
        "handle:doc:notmuch:request:Notify:Tag"
        focus.add_notify(self, "Notify:Tag")
        return 1

    def handle_notmuch_update(self, key, **a):
        "handle:doc:notmuch:update"
        if not self.timer_set:
            self.timer_set = True
            self.call("event:timer", 5*60*1000, self.tick)
        tags = notmuch_get_tags()
        if tags:
            self.searches.set_tags(tags)
        self.tick('tick')
        return 1

    def handle_notmuch_update_one(self, key, str, **a):
        "handle:doc:notmuch:update-one"
        self.searches.update_one(str)
        # update display of updating status flags
        self.notify("doc:replaced")
        return 1

    def handle_notmuch_query(self, key, focus, str, comm2, **a):
        "handle:doc:notmuch:query"
        # Find or create a search-result document as a
        # child of the collection document - it remains private
        # and doesn't get registered in the global list
        q = self.searches.make_search(str)
        nm = None
        it = self.container.children()
        for child in it:
            if child("doc:notmuch:same-search", str, q) == 1:
                nm = child
                break
        if (nm and nm.notify("doc:notify-viewers") == 0 and
            int(nm['last-refresh']) + 8*60*60 < int(time.time())):
            # no-one is looking and they haven't for a long time,
            # so just discard this one.
            nm.close()
            nm = None
        if not nm:
            nm = notmuch_query(self, str, q)
            # FIXME This is a a bit ugly.  I should pass self.container
            # as the parent, but notmuch_query needs to stash maindoc
            # Also I should use an edlib call to get notmuch_query
            nm.reparent(self.container)
            nm.call("doc:set-name", str)
        elif nm.notify("doc:notify-viewers") == 0 and nm['need-update']:
            # no viewers, so trigger a full update to discard old content
            nm.call("doc:notmuch:query:reload")
        elif nm['need-update'] or int(nm['last-refresh']) + 60 < int(time.time()):
            nm.call("doc:notmuch:query-refresh")
        nm['background-update'] = "0"
        if comm2:
            comm2("callback", focus, nm)
        return 1

    def handle_notmuch_byid(self, key, focus, str1, str2, comm2, **a):
        "handle:doc:notmuch:byid"
        # Return a document for the email message.
        # This is a global document.
        fn = notmuch_get_files(str1)
        if not fn:
            return Efail
        doc = focus.call("doc:open", "email:"+fn[0], -2, ret='pane')
        if doc:
            doc['notmuch:id'] = str1
            doc['notmuch:tid'] = str2
            for i in range(len(fn)):
                doc['notmuch:fn-%d' % i] = fn[i]
            comm2("callback", doc)
        return 1

    def handle_notmuch_byid_tags(self, key, focus, num2, str, comm2, **a):
        "handle:doc:notmuch:byid:tags"
        # return a string with tags of message
        t = notmuch_get_tags(msg = str)
        if t is None:
            return edlib.Efalse
        tags = ",".join(t)
        comm2("callback", focus, tags)
        return 1

    def handle_notmuch_bythread_tags(self, key, focus, str, comm2, **a):
        "handle:doc:notmuch:bythread:tags"
        # return a string with tags of all messages in thread
        t = notmuch_get_tags(thread = str)
        if t is None:
            return edlib.Efalse
        tags = ",".join(t)
        comm2("callback", focus, tags)
        return 1

    def handle_notmuch_query_updated(self, key, **a):
        "handle:doc:notmuch:query-updated"
        # A child search document has finished updating.
        self.next_query()
        return 1

    def handle_notmuch_mark_read(self, key, str, str2, **a):
        "handle:doc:notmuch:mark-read"
        notmuch_set_tags(msg=str2, remove = ["unread", "new"])
        self.notify("Notify:Tag", str, str2)
        return 1

    def handle_notmuch_remove_tag(self, key, str, str2, **a):
        "handle-prefix:doc:notmuch:tag-"
        if key.startswith("doc:notmuch:tag-add-"):
            add = True
            tag = key[20:]
        elif key.startswith("doc:notmuch:tag-remove-"):
            add = False
            tag = key[23:]
        else:
            return Enoarg

        if str2:
            # adjust a list of messages
            for id in str2.split("\n"):
                if add:
                    notmuch_set_tags(msg=id, add=[tag])
                else:
                    notmuch_set_tags(msg=id, remove=[tag])
                self.notify("Notify:Tag", str, id)
        else:
            # adjust whole thread
            if add:
                notmuch_set_tags(thread=str, add=[tag])
            else:
                notmuch_set_tags(thread=str, remove=[tag])
            self.notify("Notify:Tag", str)
        # FIXME can I ever optimize out the Notify ??
        return 1

    def handle_set_adhoc(self, key, focus, str, **a):
        "handle:doc:notmuch:set-adhoc"
        if str:
            self.searches.slist["-ad hoc-"] = str
        else:
            self.searches.slist["-ad hoc-"] = ""
        return 1

    def handle_get_query(self, key, focus, str1, comm2, **a):
        "handle:doc:notmuch:get-query"
        if str1 and str1 in self.searches.slist:
            comm2("cb", focus, self.searches.slist[str1])
        return 1

    def handle_set_query(self, key, focus, str1, str2, **a):
        "handle:doc:notmuch:set-query"
        if not (str1 and str2):
            return edlib.Enoarg
        self.searches.slist[str1] = str2
        p = Popen(["/usr/bin/notmuch", "config", "set",
                   "query."+str1, str2],
                  stdout = DEVNULL, stderr=DEVNULL, stdin=DEVNULL)
        try:
            p.communicate(timeout=5)
        except TimeoutExpired:
            p.kill()
            p.communicate()
            return edlib.Efalse
        if p.returncode != 0:
            return edlib.Efalse
        return 1

    def tick(self, key, **a):
        if not self.updating:
            self.searches.load(False)
            self.updating = True
            self.searches.update()
            # updating status flags might have change
            self.notify("doc:replaced")
        for c in self.container.children():
            if c.notify("doc:notify-viewers") == 0:
                # no point refreshing this, might be time to close it
                lr = c['last-refresh']
                if int(lr) + 8*60*60 < int(time.time()):
                    c.call("doc:closed")
        return 1

    def updated(self, query, changed, finished):
        if finished:
            self.updating = False
        if changed:
            self.changed_queries.append(query)
            if not self.querying:
                self.next_query()
        # always trigger 'replaced' as scan-status symbols may change
        self.notify("doc:replaced")

    def next_query(self):
        self.querying = False
        while self.changed_queries:
            q = self.changed_queries.pop(0)
            for c in self.container.children():
                if c['qname'] == q:
                    if c.notify("doc:notify-viewers") > 0:
                        # there are viewers, so just do a refresh.
                        self.querying = True
                        c("doc:notmuch:query-refresh")
                        # will get callback when time to continue
                        return
                    elif int(c['background-update']) == 0:
                        # First update with no viewers - full refresh
                        c.call("doc:set:background-update",
                               "%d" % int(time.time()))
                        self.querying = True
                        c("doc:notmuch:query:reload")
                        # will get callback when time to continue
                        return
                    elif int(time.time()) - int(c['background-update']) < 5*60:
                        # less than 5 minutes, keep updating
                        self.querying = True
                        c("doc:notmuch:query-refresh")
                        # will get callback when time to continue
                        return
                    else:
                        # Just mark for refresh-on-visit
                        c.call("doc:set:need-update", "true")

# notmuch_query document
# a mark.pos is a list of thread-id and message-id.

class notmuch_query(edlib.Doc):
    def __init__(self, focus, qname, query):
        edlib.Doc.__init__(self, focus)
        self.maindoc = focus
        self.query = query
        self.filter = ""
        self['qname'] = qname
        self['query'] = query
        self['filter'] = ""
        self['last-refresh'] = "%d" % int(time.time())
        self['need-update'] = ""
        self.threadids = []
        self.threads = {}
        self.messageids = {}
        self.threadinfo = {}
        self["render-default"] = "notmuch:threads"
        self["line-format"] = ("<%BG><%TM-hilite>%TM-date_relative</>" +
                               "<tab:130> <fg:blue>%TM-authors</>" +
                               "<tab:350>%TM-size%TM-threadinfo<%TM-hilite>" +
                               "<fg:red,bold>%TM-flag</>" +
                               "<wrap-tail:,wrap-nounderline,wrap-head:         ,wrap> </>" +
                               "<wrap-margin><fg:#FF8C00-40,action-activate:notmuch:select-1>%TM-subject</></></>")
        self.add_notify(self.maindoc, "Notify:Tag")
        self.add_notify(self.maindoc, "Notify:Close")
        self['doc-status'] = ""
        self.p = None
        self.marks_unstable = False

        self.this_load = None
        self.load_thread_active = False
        self.thread_queue = []
        self.load_full()

    def open_email(self, key, focus, str1, str2, comm2, **a):
        "handle:doc:notmuch:open"
        if not str1 or not str2:
            return edlib.Enoarg
        if str2 not in self.threadinfo:
            return edlib.Efalse
        minfo = self.threadinfo[str2]
        if str1 not in minfo:
            return edlib.Efalse
        try:
            fn = minfo[str1][0][0]
        except:
            return edlib.Efalse
        try:
            # timestamp
            ts = minfo[str1][1]
        except:
            ts = 0
        doc = focus.call("doc:open", "email:"+fn, -2, ret='pane')
        if doc:
            doc['notmuch:id'] = str1
            doc['notmuch:tid'] = str2
            doc['notmuch:timestamp'] = "%d"%ts
            for i in range(len(minfo[str1][0])):
                doc['notmuch:fn-%d' % i] = minfo[str1][0][i]
            comm2("callback", doc)
        return 1

    def set_filter(self, key, focus, str, **a):
        "handle:doc:notmuch:set-filter"
        if not str:
            str = ""
        if self.filter == str:
            return 1
        self.filter = str
        self['filter'] = str
        self.load_full()
        self.notify("doc:replaced")
        self.maindoc.notify("doc:replaced", 1)
        return 1

    def handle_shares_ref(self, key, **a):
        "handle:doc:shares-ref"
        return 1

    def handle_val_mark(self, key, mark, mark2, **a):
        "handle:debug:validate-marks"
        if not mark or not mark2:
            return edlib.Enoarg
        if mark.pos == mark2.pos:
            if mark.offset < mark2.offset:
                return 1
            edlib.LOG("notmuch_query val_marks: same pos, bad offset:",
                      mark.offset, mark2.offset)
            return edlib.Efalse
        if mark.pos is None:
            edlib.LOG("notmuch_query val_mark: mark.pos is None")
            return edlib.Efalse
        if mark2.pos is None:
            return 1
        t1,m1 = mark.pos
        t2,m2 = mark2.pos
        if t1 == t2:
            if m1 is None:
                edlib.LOG("notmuch_query val_mark: m1 mid is None",
                          mark.pos, mark2.pos)
                return edlib.Efalse
            if m2 is None:
                return 1
            if self.messageids[t1].index(m1) < self.messageids[t1].index(m2):
                return 1
            edlib.LOG("notmuch_query val_mark: messages in wrong order",
                      mark.pos, mark2.pos)
            return edlib.Efalse
        if self.marks_unstable:
            return 1
        if self.threadids.index(t1) < self.threadids.index(t2):
            return 1
        edlib.LOG("notmuch_query val_mark: pos in wrong order:",
                  mark.pos, mark2.pos)
        edlib.LOG_BT()
        return edlib.Efalse

    def setpos(self, mark, thread, msgnum = 0):
        if thread is None:
            mark.pos = None
            return
        if thread in self.messageids:
            msg = self.messageids[thread][msgnum]
        else:
            msg = None
        mark.pos = (thread, msg)
        mark.offset = 0

    def load_full(self):
        if self.p:
            # busy, don't reload just now
            return
        self.partial = False
        self.age = 1

        # mark all threads inactive, so any that remain that way
        # can be pruned.
        for id in self.threads:
            self.threads[id]['total'] = 0
        self.offset = 0
        self.tindex = 0
        self.pos = edlib.Mark(self)
        self['need-update'] = ""
        self.start_load()

    def load_update(self):
        if self.p:
            # busy, don't reload just now
            return

        self.partial = True
        self.age = None

        self.offset = 0
        self.tindex = 0
        self.pos = edlib.Mark(self)
        self['need-update'] = ""
        self.start_load()

    def start_load(self):
        self['last-refresh'] = "%d" % int(time.time())
        cmd = ["/usr/bin/notmuch", "search", "--output=summary",
               "--format=json", "--limit=100", "--offset=%d" % self.offset ]
        if self.partial:
            cmd += [ "date:-24hours.. AND " ]
        elif self.age:
            cmd += [ "date:-%ddays.. AND " % (self.age * 30)]
        if self.filter:
            cmd += [ "( %s ) AND " % self.filter ]
        cmd += [ "( %s )" % self.query ]
        self['doc-status'] = "Loading..."
        self.notify("doc:status-changed")
        self.p = Popen(cmd, shell=False, stdout=PIPE, stderr = DEVNULL)
        self.call("event:read", self.p.stdout.fileno(), self.get_threads)

    def get_threads(self, key, **a):
        found = 0
        was_empty = not self.threadids
        try:
            tl = json.load(self.p.stdout)
        except:
            tl = []
        for j in tl:
            tid = j['thread']
            found += 1
            while (self.tindex < len(self.threadids) and
                   self.threads[self.threadids[self.tindex]]["timestamp"] > j["timestamp"]):
                # Skip over this thread before inserting
                tid2 = self.threadids[self.tindex]
                self.tindex += 1
                while self.pos.pos and self.pos.pos[0] == tid2:
                    self.call("doc:step-thread", self.pos, 1, 1)
            need_update = False
            if tid in self.threads and tid in self.messageids:
                oj = self.threads[tid]
                if  (oj['timestamp'] != j['timestamp'] or
                     (oj['total'] != 0 and oj['total'] != j['total']) or
                     oj['matched'] != j['matched']):
                    need_update = True

            self.threads[tid] = j
            old = -1
            if self.tindex >= len(self.threadids) or self.threadids[self.tindex] != tid:
                # need to insert and possibly move the old marks
                try:
                    old = self.threadids.index(tid)
                except ValueError:
                    pass
                if old >= 0:
                    # debug:validate-marks looks in self.threadids
                    # which is about to become inconsistent
                    self.marks_unstable = True
                self.threadids.insert(self.tindex, tid)
                self.tindex += 1
            if old >= 0:
                # move marks on tid to before self.pos
                if old < self.tindex - 1:
                    m = self.first_mark()
                    self.tindex -= 1
                else:
                    m = self.pos
                    old += 1
                self.threadids.pop(old)
                while (m and m.pos and m.pos[0] != tid and
                       self.threadids.index(m.pos[0]) < old):
                    m = m.next_any()
                self.pos.step_sharesref(0)
                mp = self.pos.prev_any()
                if mp and mp.pos and mp.pos[0] == tid:
                    # All marks for tid are already immediately before self.pos
                    # so nothing to be moved.
                    m = None
                while m and m.pos and m.pos[0] == tid:
                    m2 = m.next_any()
                    # m needs to be before pos
                    if m.seq > self.pos.seq:
                        m.to_mark_noref(self.pos)
                    elif self.pos.prev_any().seq != m.seq:
                        m.to_mark_noref(self.pos.prev_any())
                    m = m2
                self.marks_unstable = False
                self.notify("notmuch:thread-changed", tid, 1)
            if need_update:
                if self.pos.pos and self.pos.pos[0] == tid:
                    self.load_thread(self.pos, sync=False)
                else:
                    # might be previous thread
                    m = self.pos.dup()
                    self.prev(m)
                    self.call("doc:step-thread", m, 0, 1)
                    if m.pos and m.pos[0] == tid:
                        self.load_thread(m, sync=False)

        tl = None
        if self.p:
            self.p.wait()
        self['doc-status'] = ""
        self.notify("doc:status-changed")
        self.p = None
        if was_empty and self.threadids:
            # first insertion, all marks other than self.pos must be at start
            m = self.first_mark()
            while m and m.pos is None:
                m2 = m.next_any()
                if m.seq != self.pos.seq:
                    m.step_sharesref(0)
                    self.setpos(m, self.threadids[0])
                m = m2
        self.notify("doc:replaced")
        if found < 100 and self.age == None:
            # must have found them all
            self.pos = None
            if not self.partial:
                self.prune()
            self.call("doc:notmuch:query-updated")
            return edlib.Efalse
        # request some more
        if found > 3:
            # allow for a little over-lap across successive calls
            self.offset += found - 3
        if found < 5:
            # stop worrying about age
            self.age = None
        if found < 100 and self.age:
            self.age += 1
        self.start_load()
        return edlib.Efalse

    def prune(self):
        # remove any threads with a 'total' of zero.
        # Any marks on them must be moved later
        m = edlib.Mark(self)
        while m.pos != None:
            m2 = m.dup()
            tid = m.pos[0]
            self.call("doc:step-thread", 1, 1, m2)
            if self.threads[tid]['total'] == 0:
                # notify viewers to close threads
                self.notify("notmuch:thread-changed", tid)
                m.step_sharesref(0)
                while m < m2:
                    m.pos = m2.pos
                    m = m.next_any()
                del self.threads[tid]
                self.threadids.remove(tid)
            m = m2

    def cvt_depth(self, depth):
        # depth is an array of int
        # 2 is top-level in the thread, normally only one of these
        # 1 at the end of the array means there are children
        # 1 before the end means there are more children at this depth
        # 0 means no more children at this depth
        ret = ""

        for level in depth[:-2]:
            ret += u" │ "[level]
        ret += u"╰├─"[depth[-2]]
        ret += u"─┬"[depth[-1]]

        return ret + "> "

    def add_message(self, msg, lst, info, depth, old_ti):
        # Add the message ids in depth-first order into 'lst',
        # and for each message, place summary info info in info[mid]
        # particularly including a 'depth' description which is a
        # list of "1" if this message is not the last reply to the parent,
        # else "0".
        # If old_ti, then the thread is being viewed and messages mustn't
        # disappear - so preserve the 'matched' value.
        m = msg[0]
        mid = m['id']
        lst.append(mid)
        l = msg[1]
        was_matched = old_ti and mid in old_ti and old_ti[mid][2]
        info[mid] = (m['filename'], m['timestamp'],
                     m['match'] or was_matched,
                     depth + [1 if l else 0],
                     m['headers']["From"],
                     m['headers']["Subject"],
                     m['tags'], -1)
        if l:
            l.sort(key=lambda m:(m[0]['timestamp'],m[0]['headers']['Subject']))
            for m in l[:-1]:
                self.add_message(m, lst, info, depth + [1], old_ti)
            self.add_message(l[-1], lst, info, depth + [0], old_ti)

    def step_load_thread(self):
        # start thread loading, either current with query, or next
        if not self.this_load:
            if not self.thread_queue:
                self.load_thread_active = False
                return
            self.this_load = self.thread_queue.pop(0)
        tid, mid, m, query = self.this_load
        self.thread_text = b""
        self.load_thread_active = True
        self.thread_p = notmuch_start_load_thread(tid, query)
        fd = self.thread_p.stdout.fileno()
        fl = fcntl.fcntl(fd, fcntl.F_GETFL)
        fcntl.fcntl(fd, fcntl.F_SETFL, fl | os.O_NONBLOCK)
        self.call("event:read", fd, self.load_thread_read)

    def load_thread_read(self, key, **a):
        try:
            b = os.read(self.thread_p.stdout.fileno(), 4096)
            while b:
                self.thread_text += b
                b = os.read(self.thread_p.stdout.fileno(), 4096)
        except IOError:
            # More to be read
            return 1
        self.thread_p.wait()
        self.thread_p = None
        # Must have read EOF to get here.
        th = json.loads(self.thread_text.decode("utf-8","ignore"))
        self.thread_text = None
        tid, mid, m, query = self.this_load
        if query and not th:
            # query must exclude everything
            self.this_load = (tid, mid, m, None)
        else:
            self.merge_thread(tid, mid, m, th[0])
            self.this_load = None
        self.step_load_thread()
        return edlib.Efalse

    def load_thread(self, mark, sync):
        (tid, mid) = mark.pos
        if not sync:
            self.thread_queue.append((tid, mid, mark.dup(), self.query))
            if not self.load_thread_active:
                self.step_load_thread()
            return

        thread = notmuch_load_thread(tid, self.query)
        if not thread:
            thread = notmuch_load_thread(tid)
        if not thread:
            return
        self.merge_thread(tid, mid, mark, thread)

    def thread_is_open(self, tid):
        return self.notify("notmuch:thread-open", tid) > 0

    def merge_thread(self, tid, mid, mark, thread):
        # thread is a list of top-level messages
        # in each m[0] is the message as a dict
        thread.sort(key=lambda m:(m[0]['timestamp'],m[0]['headers']['Subject']))
        midlist = []
        minfo = {}

        if tid in self.threadinfo and self.thread_is_open(tid):
            # need to preserve all messages currently visible
            old_ti = self.threadinfo[tid]
        else:
            old_ti = None
        for m in thread:
            self.add_message(m, midlist, minfo, [2], old_ti)
        self.messageids[tid] = midlist
        self.threadinfo[tid] = minfo

        if mid is None:
            # need to update all marks at this location to hold mid
            m = mark
            pos = (tid, midlist[0])
            while m and m.pos and m.pos[0] == tid:
                m.pos = pos
                m = m.prev_any()
            m = mark.next_any()
            while m and m.pos and m.pos[0] == tid:
                m.pos = pos
                m = m.next_any()
        else:
            # Need to make sure all marks on this thread are properly
            # ordered.  If we find two marks out of order, the pos of
            # the second is changed to match the first.
            m = mark
            prev = m.prev_any()
            while prev and prev.pos and prev.pos[0] == tid:
                m = prev
                prev = m.prev_any()
            ind = 0
            midlist = self.messageids[tid]
            while m and m.pos and m.pos[0] == tid:
                if m.pos[1] not in midlist:
                    self.setpos(m, tid, ind)
                else:
                    mi = midlist.index(m.pos[1])
                    if mi < ind:
                        self.setpos(m, tid, ind)
                    else:
                        ind = mi
                m = m.next_any()
            self.notify("notmuch:thread-changed", tid, 2)

    def get_matched(self, key, focus, num, str, comm2, **a):
        "handle:doc:notmuch-query:matched-mids"
        if str not in self.threadinfo:
            return edlib.Efalse
        ti = self.threadinfo[str]
        ret = []
        for mid in ti:
            if num or ti[mid][2]:
                # this message matches, or viewing all messages
                ret.append(mid)
        comm2("cb", focus, '\n'.join(ret))
        return 1

    def get_replies(self, key, focus, num, str, str2, comm2, **a):
        "handle:doc:notmuch-query:matched-replies"
        if str not in self.threadinfo:
            return edlib.Efalse
        ti = self.threadinfo[str]
        mi = self.messageids[str]
        if str2 not in mi:
            return edlib.Efalse
        i = mi.index(str2)
        d = ti[str2][3]
        dpos = len(d) - 1
        # d[ppos] will be 1 if there are more replies.
        ret = [str2]
        i += 1
        while i < len(mi) and dpos < len(d) and d[dpos]:
            mti = ti[mi[i]]
            if num or mti[2]:
                # is a match
                ret.append(mi[i])
            d = ti[mi[i]][3]
            i += 1
            while dpos < len(d) and d[dpos] == 0:
                # no more children at this level, but maybe below
                dpos += 1

        comm2("cb", focus, '\n'.join(ret))
        return 1

    def rel_date(self, sec):
        then = time.localtime(sec)
        now = time.localtime()
        nows = time.time()
        if sec < nows and sec > nows - 60:
            val = "%d secs. ago" % (nows - sec)
        elif sec < nows and sec > nows - 60*60:
            val = "%d mins. ago" % ((nows - sec)/60)
        elif sec < nows and sec > nows - 60*60*6:
            mn = (nows - sec) / 60
            hr = int(mn/60)
            mn -= 60*hr
            val = "%dh%dm ago" % (hr,mn)
        elif then[:3] == now[:3]:
            val = time.strftime("Today %H:%M", then)
        elif sec > nows:
            val = time.strftime("%Y-%b-%d!", then)
        elif sec > nows - 7 * 24 * 3600:
            val = time.strftime("%a %H:%M", then)
        elif then[0] == now[0]:
            val = time.strftime("%d/%b %H:%M", then)
        else:
            val = time.strftime("%Y-%b-%d", then)
        val = "              " + val
        val = val[-13:]
        return val

    def step(self, mark, forward, move):
        if forward:
            if mark.pos is None:
                return edlib.WEOF
            if not move:
                return '\n'
            (tid,mid) = mark.pos
            mark.step_sharesref(1)
            i = self.threadids.index(tid)
            if mid:
                j = self.messageids[tid].index(mid) + 1
            if mid and j < len(self.messageids[tid]):
                self.setpos(mark, tid, j)
            elif i+1 < len(self.threadids):
                tid = self.threadids[i+1]
                self.setpos(mark, tid, 0)
            else:
                self.setpos(mark, None)
            mark.step_sharesref(1)
            return '\n'
        else:
            j = 0
            if mark.pos == None:
                i = len(self.threadids)
            else:
                (tid,mid) = mark.pos
                i = self.threadids.index(tid)
                if mid:
                    j = self.messageids[tid].index(mid)
            if i == 0 and j == 0:
                return edlib.WEOF
            if not move:
                return '\n'
            mark.step_sharesref(0)

            if j == 0:
                i -= 1
                tid = self.threadids[i]
                if tid in self.messageids:
                    j = len(self.messageids[tid])
                else:
                    j = 1
            j -= 1
            self.setpos(mark, tid, j)
            mark.step_sharesref(0)

            return '\n'

    def handle_notify_tag(self, key, str, str2, **a):
        "handle:Notify:Tag"
        if str2:
            # re-evaluate tags of a single message
            if str in self.threadinfo:
                t = self.threadinfo[str]
                if str2 in t:
                    tg = t[str2][6]
                    s = self.maindoc.call("doc:notmuch:byid:tags", str2, ret='str')
                    tg[:] = s.split(",")

        if str in self.threads:
            t = self.threads[str]
            s = self.maindoc.call("doc:notmuch:bythread:tags", str, ret='str')
            t['tags'] = s.split(",")
        self.notify("doc:replaced")
        return 1

    def handle_notify_close(self, key, focus, **a):
        "handle:Notify:Close"
        if focus == self.maindoc:
            # Main doc is closing, so must we
            self.close()
            return 1
        return edlib.Efallthrough

    def handle_set_ref(self, key, mark, num, **a):
        "handle:doc:set-ref"
        self.to_end(mark, num != 1)
        mark.pos = None
        if num == 1 and len(self.threadids) > 0:
            self.setpos(mark, self.threadids[0], 0)
        mark.offset = 0
        return 1

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
            ret = self.handle_step(key, mark, forward, 1)
            steps -= forward * 2 - 1
        if end:
            return 1 + (num - steps if forward else steps - num)
        if ret == edlib.WEOF or num2 == 0:
            return ret
        if num and (num2 < 0) == (num > 0):
            return ret
        # want the next character
        return self.handle_step(key, mark, 1 if num2 > 0 else 0, 0)

    def handle_step(self, key, mark, num, num2):
        forward = num
        move = num2
        return self.step(mark, forward, move)

    def handle_step_thread(self, key, mark, num, num2, **a):
        "handle:doc:step-thread"
        # Move to the start of the current thread, or the start
        # of the next one.
        forward = num
        move = num2
        if forward:
            if mark.pos == None:
                return edlib.WEOF
            if not move:
                return "\n"
            (tid,mid) = mark.pos
            m2 = mark.next_any()
            while m2 and m2.pos != None and m2.pos[0] == tid:
                mark.to_mark(m2)
                m2 = mark.next_any()
            i = self.threadids.index(tid) + 1
            if i < len(self.threadids):
                self.setpos(mark, self.threadids[i], 0)
            else:
                self.setpos(mark, None)
            return '\n'
        else:
            if mark.pos == None:
                # EOF is not in a thread, so to move to the start
                # we must stary where we are. Moving further would be in
                # a different thread.
                return self.prior(mark)

            (tid,mid) = mark.pos
            m2 = mark.prev_any()
            while m2 and (m2.pos == None or m2.pos[0] == tid):
                mark.to_mark(m2)
                m2 = mark.prev_any()
            self.setpos(mark, tid, 0)
            return '\n'

    def handle_step_matched(self, key, mark, num, num2, **a):
        "handle:doc:step-matched"
        # Move to the next/prev message which is matched
        # or to an unopened thread
        forward = num
        move = num2
        m = mark
        if not move:
            m = mark.dup()
        ret = self.step(m, forward, 1)
        while ret != edlib.WEOF and m.pos != None:
            (tid,mid) = m.pos
            if not mid:
                break
            ms = self.threadinfo[tid][mid]
            if ms[2]:
                break
            ret = self.step(m, forward, 1)
        return ret

    def handle_to_thread(self, key, mark, str, **a):
        "handle:doc:notmuch:to-thread"
        # move to first message of given thread.
        if not mark or not str:
            return edlib.Enoarg
        if str not in self.threadids:
            return edlib.Efalse
        if not mark.pos or self.threadids.index(mark.pos[0]) > self.threadids.index(str):
            # step backward
            self.call("doc:step-thread", 0, 1, mark)
            while (self.prev(mark) and
                   self.call("doc:step-thread", 0, 1, mark, ret='char')  and
                   mark.pos and
                   self.threadids.index(mark.pos[0]) > self.threadids.index(str)):
                # keep going
                pass
            return 1
        elif self.threadids.index(mark.pos[0]) < self.threadids.index(str):
            # step forward
            while (self.call("doc:step-thread", 1, 1, mark, ret='char') and
                   mark.pos and
                   self.threadids.index(mark.pos[0]) < self.threadids.index(str)):
                # keep going
                pass
            return 1
        else:
            # start of thread
            self.call("doc:step-thread", 0, 1, mark)
        return 1

    def handle_to_message(self, key, mark, str, **a):
        "handle:doc:notmuch:to-message"
        # move to given message in current thread
        if not mark or not str:
            return edlib.Enoarg
        if not mark.pos or mark.pos[0] not in self.messageids:
            return edlib.Efalse
        mlist = self.messageids[mark.pos[0]]
        if str not in mlist:
            return edlib.Efalse
        i = mlist.index(str)
        while mark.pos and mark.pos[1] and mlist.index(mark.pos[1]) > i and self.prev(mark):
            # keep going back
            pass
        while mark.pos and mark.pos[1] and mlist.index(mark.pos[1]) < i and self.next(mark):
            # keep going forward
            pass
        return 1 if mark.pos and mark.pos[1] == str else edlib.Efalse

    def handle_doc_get_attr(self, key, mark, focus, str, comm2, **a):
        "handle:doc:get-attr"
        attr = str
        if not mark or mark.pos == None:
            # No attributes for EOF
            return 1
        (tid,mid) = mark.pos
        i = self.threadids.index(tid)
        j = 0
        if mid:
            j = self.messageids[tid].index(mid)

        val = None

        tid = self.threadids[i]
        t = self.threads[tid]
        if mid:
            m = self.threadinfo[tid][mid]
        else:
            m = ("", 0, False, [0,0], "" ,"", t["tags"], 0)
        (fn, dt, matched, depth, author, subj, tags, size) = m
        if attr == "message-id":
            val = mid
        elif attr == "thread-id":
            val = tid
        elif attr == "T-hilite":
            if "inbox" not in t["tags"]:
                # FIXME maybe I should test 'current' ??
                val = "fg:grey"
            elif "new" in t["tags"] and "unread" in t["tags"]:
                val = "fg:red,bold"
            elif "unread" in t["tags"]:
                val = "fg:blue"
            else:
                val = "fg:black"
        elif attr == "T-flag":
            if 'deleted' in t["tags"]:
                val = "🗑"  # WASTEBASKET     #1f5d1
            elif 'flagged' in t["tags"]:
                val = "★"  # BLACK STAR       #2605
            elif 'newspam' in t["tags"]:
                val = "✘"  # HEAVY BALLOT X   #2718
            elif 'notspam' in t["tags"]:
                val = "✔"  # HEAVY CHECK MARK #2714
            elif 'replied' in t["tags"]:
                val = "↵"  # DOWNWARDS ARROW WITH CORNER LEFTWARDS #21B5
            elif 'forwarded' in t["tags"]:
                val = "→"  # RIGHTWARDS ARROW #2192
            else:
                val = " "
        elif attr == "T-date_relative":
            val = self.rel_date(t['timestamp'])
        elif attr == "T-threadinfo":
            val = "[%d/%d]" % (t['matched'],t['total'])
            while len(val) < 7:
                val += ' '
        elif attr == "T-size":
            val = ""
        elif attr[:2] == "T-" and attr[2:] in t:
            val = t[attr[2:]]
            if type(val) == int:
                val = "%d" % val
            elif type(val) == list:
                val = ','.join(val)
            else:
                # Some mailers use ?Q to insert =0A (newline) in a subject!!
                val = t[attr[2:]].replace('\n',' ')
            if attr == "T-authors":
                val = val[:20]

        elif attr == "matched":
            val = "True" if matched else "False"
        elif attr == "tags" or attr == "M-tags":
            val = ','.join(tags)
        elif attr == "M-hilite":
            if "inbox" not in tags:
                val = "fg:grey"
                if "new" in tags and "unread" in tags:
                    val = "fg:pink"
            elif "new" in tags and "unread" in tags:
                val = "fg:red,bold"
            elif "unread" in tags:
                val = "fg:blue"
            else:
                val = "fg:black"
        elif attr == "M-flag":
            if 'deleted' in tags:
                val = "🗑"  # WASTEBASKET     #1f5d1
            elif 'flagged' in tags:
                val = "★"  # BLACK STAR       #2605
            elif 'newspam' in tags:
                val = "✘"  # HEAVY BALLOT X   #2718
            elif 'notspam' in tags:
                val = "✔"  # HEAVY CHECK MARK #2714
            elif 'replied' in tags:
                val = "↵"  # DOWNWARDS ARROW WITH CORNER LEFTWARDS #21B5
            elif 'forwarded' in tags:
                val = "→"  # RIGHTWARDS ARROW #2192
            else:
                val = " "
        elif attr == "M-date_relative":
            val = self.rel_date(dt)
        elif attr == "M-authors":
            val = author[:20].replace('\n',' ')
        elif attr == "M-subject":
            val = subj.replace('\n',' ')
        elif attr == "M-threadinfo":
            val = self.cvt_depth(depth)
        elif attr == "M-size":
            if size < 0 and fn and fn[0]:
                try:
                    st = os.lstat(fn[0])
                    size = st.st_size
                except FileNotFoundError:
                    size = 0
                self.threadinfo[tid][mid] = (fn, dt, matched, depth,
                                             author, subj, tags, size)
            if size > 0:
                val = cvt_size(size)
            else:
                val = "????"

        if not val is None:
            comm2("callback", focus, val, mark, attr)
            return 1
        return edlib.Efallthrough

    def handle_get_attr(self, key, focus, str, comm2, **a):
        "handle:get-attr"
        if str == "doc-type" and comm2:
            comm2("callback", focus, "notmuch-query")
            return 1
        return edlib.Efallthrough

    def handle_maindoc(self, key, **a):
        "handle-prefix:doc:notmuch:"
        # any doc:notmuch calls that we don't handle directly
        # are handed to the maindoc
        return self.maindoc.call(key, **a)

    def handle_reload(self, key, **a):
        "handle:doc:notmuch:query:reload"
        self.load_full()
        return 1

    def handle_load_thread(self, key, mark, **a):
        "handle:doc:notmuch:load-thread"
        if mark.pos == None:
            return edlib.Efail
        (tid,mid) = mark.pos
        self.load_thread(mark, sync=True)
        if tid in self.threadinfo:
            return 1
        return 2

    def handle_same_search(self, key, str2, **a):
        "handle:doc:notmuch:same-search"
        if self.query == str2:
            return 1
        return 2

    def handle_query_refresh(self, key, **a):
        "handle:doc:notmuch:query-refresh"
        self.load_update()
        return 1

    def handle_mark_read(self, key, str, str2, **a):
        "handle:doc:notmuch:mark-read"
        # Note that the thread might already have been pruned,
        # in which case there is no cached info to update.
        # In that case we just pass the request down to the db.
        if str in self.threadinfo:
            ti = self.threadinfo[str]
            m = ti[str2]
            tags = m[6]
            if "unread" not in tags and "new" not in tags:
                return
            if "unread" in tags:
                tags.remove("unread")
            if "new" in tags:
                tags.remove("new")
            is_unread = False
            for mid in ti:
                if "unread" in ti[mid][6]:
                    # still has unread messages
                    is_unread = True
                    break
            if not is_unread and str in self.threads:
                # thread is no longer 'unread'
                j = self.threads[str]
                t = j["tags"]
                if "unread" in t:
                    t.remove("unread")
            self.notify("doc:replaced")
        # Cached info is updated, pass down to
        # database document for permanent change
        self.maindoc.call(key, str, str2)
        return 1

class tag_popup(edlib.Pane):
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus)

    def handle_enter(self, key, focus, **a):
        "handle:K:Enter"
        str = focus.call("doc:get-str", ret='str')
        focus.call("popup:close", str)
        return 1

class query_popup(edlib.Pane):
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus)

    def handle_enter(self, key, focus, **a):
        "handle:K:Enter"
        str = focus.call("doc:get-str", ret='str')
        focus.call("popup:close", str)
        return 1

#
# There are 4 viewers
#  notmuch_master_view  manages multiple notmuch tiles.  When the notmuch_main
#       is displayed, this gets gets attached *under* (closer to root) the
#       doc-view pane together with a tiling window.  One tile is used to
#       display the query list from the main doc, other tiles are used to
#       display other components.
#  notmuch_list_view  displays the list of saved-searched registered with
#       notmuch_main.  It display list-name and a count of the most interesting
#       sorts of message.
#  notmuch_query_view displays the list of threads and messages for a single
#       query - a notmuch_query document
#  notmuch_message_view displays a single message, a doc-email document
#       which is implemented separately.  notmuch_message_view primarily
#       provides interactions consistent with the rest of edlib-notmuch

class notmuch_master_view(edlib.Pane):
    # This pane controls one visible instance of the notmuch application.
    # It manages the size and position of the 3 panes and provides common
    # handling for some keystrokes.
    # 'focus' is normally None and we are created parentless.  The creator
    # then attaches us and a tiler beneath the main notmuch document.
    #
    def __init__(self, focus = None):
        edlib.Pane.__init__(self, focus)
        self.maxlen = 0 # length of longest query name in list_pane
        self.list_pane = None
        self.query_pane = None
        self.message_pane = None

    def handle_set_main_view(self, key, focus, **a):
        "handle:notmuch:set_list_pane"
        self.list_pane = focus
        return 1

    def resize(self):
        if self.list_pane and (self.query_pane or self.message_pane):
            # list_pane must be no more than 25% total width, and no more than
            # 5+1+maxlen+1
            if self.maxlen <= 0:
                m = self.list_pane["notmuch:max-search-len"]
                if m and m.isnumeric():
                    self.maxlen = int(m)
                else:
                    self.maxlen = 20
            tile = self.list_pane.call("ThisPane", "notmuch", ret='pane')
            space = self.w
            ch,ln = tile.scale()
            max = 5 + 1 + self.maxlen + 1
            if space * 100 / ch < max * 4:
                w = space / 4
            else:
                w = ch * 10 * max / 1000
            if tile.w != w:
                tile.call("Window:x+", "notmuch", int(w - tile.w))
        if self.query_pane and self.message_pane:
            # query_pane must be at least 4 lines, else 1/4 height
            # but never more than 1/2 the height
            tile = self.query_pane.call("ThisPane", "notmuch", ret='pane')
            ch,ln = tile.scale()
            space = self.h
            min = 4
            if space * 100 / ln > min * 4:
                h = space / 4
            else:
                h = ln * 10 * min / 1000
                if h > space / 2:
                    h = space / 2
            if tile.h != h:
                tile.call("Window:y+", "notmuch", int(h - tile.h))

    def handle_getattr(self, key, focus, str, comm2, **a):
        "handle:get-attr"
        if comm2:
            val = None
            if str in ["qname","query","filter"] and self.query_pane:
                # WARNING these must always be set in the query doc,
                # otherwise we can recurse infinitely.
                val = self.query_pane[str]
            if val:
                comm2("callback", focus, val, str)
                return 1
        return edlib.Efallthrough

    def handle_choose(self, key, **a):
        "handle:docs:choose"
        # If a notmuch tile needs to find a new doc, e.g. because
        # a message doc was killed, reject the request so that the
        # pane will be closed.
        return 1

    def handle_clone(self, key, focus, **a):
        "handle:Clone"
        main = notmuch_master_view(focus)
        p = main.call("attach-tile", "notmuch", "main", ret='pane')
        frm = self.list_pane.call("ThisPane", "notmuch", ret='pane')
        frm.clone_children(p)
        return 1

    recursed = None
    def handle_maindoc(self, key, **a):
        "handle-prefix:doc:notmuch:"
        # any doc:notmuch calls that haven't been handled
        # are handled to the list_pane
        if self.recursed == key:
            edlib.LOG("doc:notmuch: recursed!", key)
            return edlib.Efail
        prev = self.recursed
        self.recursed = key
        # FIXME catch exception to return failure state properly
        ret = self.list_pane.call(key, **a)
        self.recursed = prev
        return ret

    def handle_size(self, key, **a):
        "handle:Refresh:size"
        # First, make sure the tiler has adjusted to the new size
        self.focus.w = self.w
        self.focus.h = self.h
        self.focus("Refresh:size")
        # then make sure children are OK
        self.resize()
        return 1

    def handle_dot(self, key, focus, mark, **a):
        "handle:doc:char-."
        # select thing under point, but don't move
        focus.call("notmuch:select", mark, 0)
        return 1

    def handle_return(self, key, focus, mark, **a):
        "handle:K:Enter"
        # select thing under point, and enter it
        focus.call("notmuch:select", mark, 1)
        return 1

    def handle_select_1(self, key, focus, mark, **a):
        "handle:notmuch:select-1"
        # select thing under point, and enter it
        focus.call("notmuch:select", mark, 1)
        return 1

    def handle_search(self, key, focus, **a):
        "handle:doc:char-s"
        pup = focus.call("PopupTile", "3", "", ret='pane')
        if not pup:
            return edlib.Efail
        pup['done-key'] = "notmuch-do-ad hoc"
        pup['prompt'] = "Ad hoc query"
        pup.call("doc:set-name", "Ad hoc query")
        p = pup.call("attach-history", "*Notmuch Query History*",
                     "popup:close", ret='pane')
        if p:
            pup = p
        query_popup(pup)
        return 1

    def handle_compose(self, key, focus, **a):
        "handle:doc:char-c"
        choice = []
        def choose(choice, a):
            focus = a['focus']
            if focus['email-sent'] == 'no':
                choice.append(focus)
                return 1
            return 0
        focus.call("docs:byeach", lambda key,**a:choose(choice, a))
        if len(choice):
            par = focus.call("PopupTile", "MD3tsa", ret='pane')
            if par:
                par = choice[0].call("doc:attach-view", par, 1, ret='pane')
                par.take_focus()
        else:
            focus.call("Message:modal",
                       "No active email composition documents found.")
        return 1

    def do_search(self, key, focus, str, **a):
        "handle:notmuch-do-ad hoc"
        if str:
            self.list_pane.call("doc:notmuch:set-adhoc", str)
            self.list_pane.call("notmuch:select-adhoc", 1)
        return 1

    def handle_filter(self, key, focus, **a):
        "handle:doc:char-f"
        if not self.query_pane:
            return 1
        f = focus['filter']
        if not f:
            f = ""
        pup = focus.call("PopupTile", "3", f, ret='pane')
        if not pup:
            return edlib.Efail
        pup['done-key'] = "notmuch-do-filter"
        pup['prompt'] = "Query filter"
        pup.call("doc:set-name", "*Query filter for %s*" % focus['qname'])
        p = pup.call("attach-history", "*Notmuch Filter History*",
                     "popup:close", ret='pane')
        if p:
            pup = p
        query_popup(pup)
        return 1

    def do_filter(self, key, focus, str1, **a):
        "handle:notmuch-do-filter"
        if self.query_pane and str1:
            self.query_pane.call("doc:notmuch:set-filter", str1)
        return 1

    def handle_space(self, key, **a):
        "handle:doc:char- "
        if self.message_pane:
            m = self.message_pane.call("doc:point", ret='mark')
            self.message_pane.call(key, m)
        elif self.query_pane:
            m = self.query_pane.call("doc:point", ret='mark')
            self.query_pane.call("K:Enter", m)
        else:
            m = self.list_pane.call("doc:point", ret='mark')
            self.list_pane.call("K:Enter", m)
        return 1

    def handle_bs(self, key, **a):
        "handle:K:Backspace"
        if self.message_pane:
            m = self.message_pane.call("doc:point", ret='mark')
            self.message_pane.call(key, m)
        elif self.query_pane:
            m = self.query_pane.call("doc:point", ret='mark')
            self.query_pane.call("doc:char-p", m)
        else:
            m = self.list_pane.call("doc:point", ret='mark')
            self.list_pane.call("K:A-p", m)
        return 1

    def handle_move(self, key, **a):
        "handle-list/K:A-n/K:A-p/doc:char-n/doc:char-p"
        if key.startswith("K:A-") or not self.query_pane:
            p = self.list_pane
            op = self.query_pane
        else:
            p = self.query_pane
            op = self.message_pane
        if not p:
            return 1

        direction = 1 if key[-1] in "na" else -1
        if op:
            # secondary window exists so move, otherwise just select
            try:
                p.call("Move-Line", direction)
            except edlib.commandfailed:
                pass

        m = p.call("doc:dup-point", 0, edlib.MARK_UNGROUPED, ret='mark')
        p.call("notmuch:select", m, direction)
        return 1

    def handle_j(self, key, focus, **a):
        "handle:doc:char-j"
        # jump to the next new/unread message/thread
        p = self.query_pane
        if not p:
            return 1
        m = p.call("doc:dup-point", 0, edlib.MARK_UNGROUPED, ret='mark')
        p.call("Move-Line", m, 1)
        tg = p.call("doc:get-attr", m, "tags", ret='str')
        while tg is not None:
            tl = tg.split(',')
            if "unread" in tl:
                break
            p.call("Move-Line", m, 1)
            tg = p.call("doc:get-attr", m, "tags", ret='str')
        if tg is None:
            focus.call("Message", "All messsages read!")
            return 1
        p.call("Move-to", m)
        if self.message_pane:
            p.call("notmuch:select", m, 1)
        return 1

    def handle_move_thread(self, key, **a):
        "handle-list/doc:char-N/doc:char-P"
        p = self.query_pane
        op = self.message_pane
        if not self.query_pane:
            return 1

        direction = 1 if key[-1] in "N" else -1
        if self.message_pane:
            # message window exists so move, otherwise just select
            self.query_pane.call("notmuch:close-thread")
            self.query_pane.call("Move-Line", direction)

        m = p.call("doc:dup-point", 0, edlib.MARK_UNGROUPED, ret='mark')
        p.call("notmuch:select", m, direction)
        return 1

    def handle_A(self, key, focus, num, mark, str, **a):
        "handle-list/doc:char-a/doc:char-A/doc:char-k/doc:char-S/doc:char-H/doc:char-*/doc:char-!/doc:char-d/doc:char-D/"
        # adjust flags for this message or thread, and move to next
        # a - remove inbox
        # A - remove inbox from entire thread
        # k - remove inbox from this message and replies
        # d - remove inbox, add deleted
        # D - as D, for entire thread
        # S - add newspam
        # H - ham: remove newspam and add notspam
        # * - add flagged
        # ! - add unread,inbox remove newspam,notspam,flagged,deleted
        # If num is negative reverse the change (except for !)
        # If num is not +/-NO_NUMERIC, apply to whole thread
        which = focus['notmuch:pane']
        if which not in ['message', 'query']:
            return 1

        wholethread = False
        replies = False
        if num != edlib.NO_NUMERIC and num != -edlib.NO_NUMERIC:
            wholethread = True

        adds = []; removes = []
        if key[-1] == 'a':
            removes = ['inbox']
        if key[-1] == 'A':
            removes = ['inbox']
            wholethread = True
        if key[-1] == 'd':
            removes = ['inbox']
            adds = ['deleted']
        if key[-1] == 'D':
            removes = ['inbox']
            adds = ['deleted']
            wholethread = True
        if key[-1] == 'k':
            removes = ['inbox']
            replies = True
        if key[-1] == 'S':
            adds = ['newspam']
        if key[-1] == 'H':
            adds = ['notspam']
            removes = ['newspam']
        if key[-1] == '*':
            adds = ['flagged']
        if key[-1] == '!':
            adds = ['unread','inbox']
            removes = ['newspam','notspam','flagged','deleted']

        if num < 0 and key[-1] != '!':
            adds, removes = removes, adds

        if which == "message":
            thid = self.message_pane['thread-id']
            msid = self.message_pane['message-id']
        elif which == "query":
            thid = focus.call("doc:get-attr", "thread-id", mark, ret = 'str')
            msid = focus.call("doc:get-attr", "message-id", mark, ret = 'str')
        else:
            return 1
        if not thid:
            return 1

        if wholethread:
            mids = self.query_pane.call("doc:notmuch-query:matched-mids",
                                    thid, ret='str')
        elif replies:
            # only mark messages which are replies to msid
            mids = self.query_pane.call("doc:notmuch-query:matched-replies",
                                        thid, msid, ret='str')
        else:
            mids = msid
        self.do_update(thid, mids, adds, removes)
        if mids:
            mid = mids.split("\n")[-1]
        else:
            mid = None
        m = edlib.Mark(self.query_pane)
        self.query_pane.call("notmuch:find-message", thid, mid, m)
        if m:
            self.query_pane.call("Move-to", m)
        self.query_pane.call("Move-Line", 1)
        if self.message_pane:
            # open the thread, and maybe the message, if the msid was open
            m = self.query_pane.call("doc:dup-point", 0,
                                     edlib.MARK_UNGROUPED, ret='mark')
            if msid and self.message_pane['notmuch:id'] == msid:
                self.query_pane.call("notmuch:select", m, 1)
            else:
                self.query_pane.call("notmuch:select", m, 0)
        return 1

    def handle_new_mail(self, key, focus, **a):
        "handle:doc:char-m"
        v = make_composition(self.list_pane, focus)
        if v:
            v.call("compose-email:empty-headers")
        return 1

    def handle_reply(self, key, focus, num, **a):
        "handle-list/doc:char-r/doc:char-R/doc:char-F/doc:char-z"
        if not self.message_pane:
            focus.call("Message", "Can only reply when a message is open")
            return edlib.Efail
        quote_mode = "inline"
        if num != edlib.NO_NUMERIC:
            quote_mode = "none"
        if key[-1] == 'F':
            hdr_mode = "forward"
            tag = "forwarded"
            if quote_mode == "none":
                quote_mode = "attach"
        elif key[-1] == 'z':
            hdr_mode = "forward"
            tag = "forwarded"
            quote_mode = "attach"
        elif key[-1] == 'R':
            hdr_mode = "reply-all"
            tag = "replied"
        else:
            hdr_mode = "reply"
            tag = "replied"
        v = make_composition(self.list_pane, focus,
                             tag="tag +%s -new -unread id:%s" % (tag, self.message_pane['message-id']))
        if v:
            v.call("compose-email:copy-headers", self.message_pane, hdr_mode)
            if quote_mode == "inline":
                # find first visible text part and copy it
                msg = self.message_pane
                m = edlib.Mark(msg)
                while True:
                    msg.call("doc:step-part", m, 1)
                    which = msg.call("doc:get-attr",
                                     "multipart-this:email:which",
                                     m, ret='str')
                    if not which:
                        break
                    if which != "spacer":
                        continue
                    vis = msg.call("doc:get-attr", "email:visible", m,
                                   ret = 'str')
                    if not vis or vis == 'none':
                        continue
                    type = msg.call("doc:get-attr",
                                    "multipart-prev:email:content-type",
                                    m, ret='str')
                    if (not type or not type.startswith("text/") or
                        type == "text/rfc822-headers"):
                        continue
                    part = msg.call("doc:get-attr", m,
                                    "multipart:part-num", ret='str')
                    # try transformed first
                    c = msg.call("doc:multipart-%d-doc:get-str" % (int(part) - 1),
                                 ret = 'str')
                    if not c or not c.strip():
                        c = msg.call("doc:multipart-%d-doc:get-str" % (int(part) - 2),
                                     ret = 'str')
                    if c and c.strip():
                        break

                if c:
                    v.call("compose-email:quote-content", c)

            if quote_mode == "attach":
                fn = self.message_pane["filename"]
                if fn:
                    v.call("compose-email:attach", fn, "message/rfc822")
        return 1

    def handle_mesg_cmd(self, key, focus, mark, num, **a):
        "handle-list/doc:char-X"
        # general commands to be directed to message view
        if self.message_pane:
            self.message_pane.call(key, num)
        return 1

    def tag_ok(self, t):
        for c in t:
            if not (c.isupper() or c.islower() or c.isdigit()):
                return False
        return True

    def do_update(self, tid, mid, adds, removes):
        skipped = []
        for t in adds:
            if self.tag_ok(t):
                self.list_pane.call("doc:notmuch:tag-add-%s" % t, tid, mid)
            else:
                skipped.append(t)
        for t in removes:
            if self.tag_ok(t):
                self.list_pane.call("doc:notmuch:tag-remove-%s" % t, tid, mid)
            else:
                skipped.append(t)
        if skipped:
            self.list_pane.call("Message", "Skipped illegal tags:" + ','.join(skipped))

    def handle_tags(self, key, focus, mark, num, **a):
        "handle-list/doc:char-+"
        # add or remove flags, prompting for names

        if self.message_pane and self.mychild(focus) == self.mychild(self.message_pane):
            curr = 'message'
        elif self.query_pane and self.mychild(focus) == self.mychild(self.query_pane):
            curr = 'query'
        else:
            curr = 'main'

        if curr == 'main':
            return 1
        if curr == 'query':
            thid = focus.call("doc:get-attr", "thread-id", mark, ret='str')
            msid = focus.call("doc:get-attr", "message-id", mark, ret='str')
        else:
            thid = self.message_pane['thread-id']
            msid = self.message_pane['message-id']
        if not thid:
            # FIXME maybe warn that there is no message here.
            # Might be at EOF
            return 1

        pup = focus.call("PopupTile", "2", '-' if num < 0 else '+', ret='pane')
        if not pup:
            return edlib.Fail
        done = "notmuch-do-tags-%s" % thid
        if msid:
            done += " " + msid
        pup['done-key'] = done
        pup['prompt'] = "[+/-]Tags"
        pup.call("doc:set-name", "Tag changes")
        tag_popup(pup)
        return 1

    def handle_neg(self, key, focus, num, mark, **a):
        "handle:doc:char--"
        if num < 0:
            # double negative is 'tags'
            return self.handle_tags(key, focus, mark, num)
        # else negative prefix arg
        focus.call("Mode:set-num", -num)
        return 1

    def parse_tags(self, tags):
        adds = []
        removes = []
        if not tags or tags[0] not in "+-":
            return None
        mode = ''
        tl = []
        for t in tags.split(','):
            tl.extend(t.split(' '))
        for t in tl:
            tg = ""
            for c in t:
                if c in "+-":
                    if tg != "" and mode == '+':
                        adds.append(tg)
                    if tg != "" and mode == '-':
                        removes.append(tg)
                    tg = ""
                    mode = c
                else:
                    tg += c
            if tg != "" and mode == '+':
                adds.append(tg)
            if tg != "" and mode == '-':
                removes.append(tg)
        return (adds, removes)

    def do_tags(self, key, focus, str1, **a):
        "handle-prefix:notmuch-do-tags-"
        if not str1:
            return edlib.Efail
        suffix = key[16:]
        ids = suffix.split(' ', 1)
        thid = ids[0]
        if len(ids) == 2:
            msid = ids[1]
        else:
            msid = None
        t = self.parse_tags(str1)
        if t is None:
            focus.call("Message", "Tags list must start with + or -")
        else:
            self.do_update(thid, msid, t[0], t[1])
        return 1

    def handle_close_message(self, key, num, **a):
        "handle:notmuch-close-message"
        self.message_pane = None
        if num and self.query_pane:
            pnt = self.query_pane.call("doc:point", ret='mark')
            if pnt:
                pnt['notmuch:current-message'] = ''
                pnt['notmuch:current-thread'] = ''
        return 1

    def handle_xq(self, key, **a):
        "handle-list/doc:char-x/doc:char-q/doc:char-Q"
        if self.message_pane:
            if key != "doc:char-x":
                self.mark_read()
            p = self.message_pane
            self("notmuch-close-message", 1)
            p.call("Window:close", "notmuch")
        elif self.query_pane:
            if (self.query_pane.call("notmuch:close-whole-thread") == 1 and
                key != "doc:char-Q"):
                return 1
            if (self.query_pane.call("notmuch:close-thread") == 1 and
                key != "doc:char-Q"):
                return 1
            if self.query_pane['filter']:
                self.query_pane.call("doc:notmuch:set-filter")
                if key != "doc:char-Q":
                    return 1
            if key != "doc:char-x":
                self.query_pane.call("notmuch:mark-seen")
            p = self.query_pane
            self.query_pane = None
            pnt = self.list_pane.call("doc:point", ret='mark')
            pnt['notmuch:query-name'] = ""
            self.list_pane.call("view:changed")

            p.call("Window:close", "notmuch")
        elif key == "doc:char-Q":
            p = self.call("ThisPane", ret='pane')
            if p and p.focus:
                p.focus.close()
        return 1

    def handle_v(self, key, **a):
        "handle:doc:char-V"
        # View the current message as a raw file
        if not self.message_pane:
            return 1
        p2 = self.call("doc:open", self.message_pane["filename"], -1,
                       ret='pane')
        p2.call("doc:set:autoclose", 1)
        p0 = self.call("DocPane", p2, ret='pane')
        if p0:
            p0.take_focus()
            return 1
        p0 = self.call("OtherPane", ret='pane')
        if p0:
            p2.call("doc:attach-view", p0, 1, "viewer")
        return 1

    def handle_o(self, key, focus, **a):
        "handle:doc:char-o"
        # focus to next window
        focus.call("Window:next", "notmuch")
        return 1

    def handle_O(self, key, focus, **a):
        "handle:doc:char-O"
        # focus to prev window
        focus.call("Window:prev", "notmuch")
        return 1

    def handle_g(self, key, focus, **a):
        "handle:doc:char-g"
        focus.call("doc:notmuch:update")
        return 1

    def handle_Z(self, key, **a):
        "handle-list/doc:char-Z/doc:char-=/"
        if self.query_pane:
            return self.query_pane.call(key)
        return edlib.Efallthrough

    def handle_select_query(self, key, num, str, **a):
        "handle:notmuch:select-query"
        # A query was selected, identifed by 'str'.  Close the
        # message window and open a threads window.
        if self.message_pane:
            p = self.message_pane
            self("notmuch-close-message", 1)
            p.call("Window:close", "notmuch")
            self.message_pane = None

        # doc:notmuch:query might auto-select a message, which will
        # call doc:notmuch:open, which tests self.query_pane,
        # which might be in the process of being closed.  Don't want
        # that, so just clear query_pane early.
        self.query_pane = None
        p0 = self.list_pane.call("doc:notmuch:query", str, ret='pane')
        p1 = self.list_pane.call("OtherPane", "notmuch", "threads", 15,
                                 ret='pane')
        self.query_pane = p0.call("doc:attach-view", p1, ret='pane')

        pnt = self.list_pane.call("doc:point", ret='mark')
        pnt['notmuch:query-name'] = str
        self.list_pane.call("view:changed");

        if num:
            self.query_pane.take_focus()
        self.resize()
        return 1

    def handle_select_message(self, key, focus, num, str1, str2, **a):
        "handle:notmuch:select-message"
        # a thread or message was selected. id in 'str1'. threadid in str2
        # Find the file and display it in a 'message' pane
        self.mark_read()

        p0 = None
        if self.query_pane:
            p0 = self.query_pane.call("doc:notmuch:open", str1, str2, ret='pane')
        if not p0:
            p0 = self.list_pane.call("doc:notmuch:byid", str1, str2, ret='pane')
        if not p0:
            focus.call("Message", "Failed to find message %s" % str2)
            return edlib.Efail
        p0['notmuch:tid'] = str2

        qp = self.query_pane
        if not qp:
            qp = focus
        p1 = focus.call("OtherPane", "notmuch", "message", 13,
                                  ret='pane')
        p3 = p0.call("doc:attach-view", p1, ret='pane')
        p3 = p3.call("attach-render-notmuch:message", ret='pane')

        # FIXME This still doesn't work: there are races: attaching a doc to
        # the pane causes the current doc to be closed.  But the new doc
        # hasn't been anchored yet so if they are the same, we lose.
        # Need a better way to anchor a document.
        #p0.call("doc:set:autoclose", 1)
        p3['thread-id'] = str2
        p3['message-id'] = str1
        self.message_pane = p3
        if self.query_pane:
            pnt = self.query_pane.call("doc:point", ret='mark')
            if pnt:
                pnt['notmuch:current-thread'] = str2
                pnt['notmuch:current-message'] = str1
        if num:
            self.message_pane.take_focus()
        self.resize()
        return 1

    def mark_read(self):
        p = self.message_pane
        if not p:
            return
        if self.query_pane:
            self.query_pane.call("doc:notmuch:mark-read",
                                 p['thread-id'], p['message-id'])

class notmuch_list_view(edlib.Pane):
    # This pane provides view on the search-list document.
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus)
        self['notmuch:pane'] = 'main'
        self['background'] = 'color:#A0FFFF'
        self['line-format'] = '<%fmt>%count%space<underline,action-activate:notmuch:select>%name</></>'
        self.call("notmuch:set_list_pane")
        self.call("doc:request:doc:replaced")
        self.selected = None
        pnt = self.call("doc:point", ret='mark')
        if pnt and pnt['notmuch:query-name']:
            self.call("notmuch:select-query", pnt['notmuch:query-name'])

    def handle_clone(self, key, focus, **a):
        "handle:Clone"
        p = notmuch_list_view(focus)
        self.clone_children(focus.focus)
        return 1

    def handle_notify_replace(self, key, **a):
        "handle:doc:replaced"
        # FIXME do I need to do anything here? - of not, why not
        return edlib.Efallthrough

    def handle_select(self, key, focus, mark, num, **a):
        "handle:notmuch:select"
        s = focus.call("doc:get-attr", "query", mark, ret='str')
        if s:
            focus.call("notmuch:select-query", s, num)
        return 1

    def handle_select_adhoc(self, key, focus, mark, num, **a):
        "handle:notmuch:select-adhoc"
        focus.call("notmuch:select-query", "-ad hoc-", num)
        return 1

##################
# query_view shows a list of threads/messages that match a given
# search query.
# We generate the thread-ids using "notmuch search --output=summary"
# For a full-scan we collect at most 100 and at most 1 month at a time, until
# we reach an empty month, then get all the rest together
# For an update, we just check the last day and add anything missing.
# We keep an array of thread-ids
#
# Three different views are presented of this document depending on
# whether 'selected' and possibly 'whole_thread' are set.
#
# By default when neither is set, only the threads are displayed.
# If a threadid is 'selected' but not 'whole_thread', then other threads
# appear as a single line, but the selected thread displays as all
# matching messages.
# If whole_thread is set, then only the selected thread is visible,
# and all messages, matched or not, of that thread are visisble.
#

class notmuch_query_view(edlib.Pane):
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus)
        self.selected = None
        self.selmsg = None
        self.whole_thread = False
        self.seen_threads = {}
        self.seen_msgs = {}
        self['notmuch:pane'] = 'query'

        # thread_start and thread_end are marks which deliniate
        # the 'current' thread. thread_end is the start of the next
        # thread (if there is one).
        # thread_matched is the first matched message in the thread.
        self.thread_start = None
        self.thread_end = None
        self.thread_matched = None
        (xs,ys) = self.scale()
        ret = []
        self.call("Draw:text-size", "M", -1, ys,
                  lambda key, **a: ret.append(a))
        if ret:
            lh = ret[0]['xy'][1]
        else:
            lh = 1
        # fixme adjust for pane size
        self['render-vmargin'] = "%d" % (4 * lh)

        # if first thread is new, move to it.
        m = edlib.Mark(self)
        t = self.call("doc:get-attr", m, "T-tags", ret='str')
        if t and 'new' in t.split(','):
            self.call("Move-to", m)
        else:
            # otherwise restore old state
            pt = self.call("doc:point", ret='mark')
            if pt:
                if pt['notmuch:selected']:
                    self("notmuch:select", 1, pt, pt['notmuch:selected'])
                mid = pt['notmuch:current-message']
                tid = pt['notmuch:current-thread']
                if mid and tid:
                    self.call("notmuch:select-message", mid, tid)
                    self.selmsg = mid

        self.call("doc:request:doc:replaced")
        self.call("doc:request:notmuch:thread-changed")
        self.updating = False
        self.call("doc:request:notmuch:thread-open")

    def handle_getattr(self, key, focus, str, comm2, **a):
        "handle:get-attr"
        if comm2 and str == "doc-status":
            val = self.parent['doc:status']
            if not val:
                val = ""
            if self['filter']:
                val = "filter: %s %s" % (
                    self['filter'], val)
            elif self['qname'] == '-ad hoc-':
                val = "query: %s %s" % (
                    self['query'], val)
            comm2("callback", focus, val)
            return 1

    def handle_clone(self, key, focus, **a):
        "handle:Clone"
        p = notmuch_query_view(focus)
        self.clone_children(focus.focus)
        return 1

    def handle_close(self, key, focus, **a):
        "handle:Close"

        # Reload the query so archived messages disappear
        self.call("doc:notmuch:query:reload")
        self.call("doc:notmuch:update-one", self['qname'])
        return 1

    def handle_matched_mids(self, key, focus, str, str2, comm2, **a):
        "handle-prefix:doc:notmuch-query:matched-"
        # if whole_thread, everything should be considered matched.
        if str and str == self.selected and self.whole_thread:
            return self.parent.call(key, focus, str, str2, 1, comm2)
        return edlib.Efallthrough

    def handle_notify_replace(self, key, **a):
        "handle:doc:replaced"
        if self.thread_start:
            # Possible insertion before thread_end - recalc.
            self.thread_end = self.thread_start.dup()
            self.leaf.call("doc:step-thread", 1, 1, self.thread_end)
        self.leaf.call("view:changed")
        self.call("doc:notify:doc:status-changed")
        return edlib.Efallthrough

    def close_thread(self, gone = False):
        if not self.selected:
            return None
        # old thread is disappearing.  If it is not gone, clip marks
        # to start, else clip to next thread.
        self.leaf.call("Notify:clip", self.thread_start, self.thread_end,
                       0 if gone else 1)
        if self.whole_thread:
            # And clip anything after (at eof) to thread_end
            eof = edlib.Mark(self)
            self.leaf.call("doc:set-ref", eof, 0)
            eof.step(1)
            eof.index = 1 # make sure all eof marks are different
            self.leaf.call("Notify:clip", self.thread_end, eof, 1)
            eof.index = 0
        self.leaf.call("view:changed", self.thread_start, self.thread_end)
        self.selected = None
        self.thread_start = None
        self.thread_end = None
        self.thread_matched = None
        self.whole_thread = False
        return 1

    def move_thread(self):
        if not self.selected:
            return
        # old thread is or moving
        # thread is still here, thread_start must have moved, thread_end
        # might be where thread used to be.
        self.thread_end = self.thread_start.dup()
        self.call("doc:step-thread", self.thread_end, 1, 1)
        self.leaf.call("view:changed", self.thread_start, self.thread_end)
        return 1

    def find_message(self, key, focus, mark, str, str2, **a):
        "handle:notmuch:find-message"
        if not str or not mark:
            return edlib.Enoarg
        if not self.selected or self.selected != str:
            str2 = None
        if self.call("doc:notmuch:to-thread", mark, str) <= 0:
            return edlib.Efalse
        if str2 and self.call("doc:notmuch:to-message", mark, str2) <= 0:
            return edlib.Efalse
        return 1

    def trim_thread(self):
        if self.whole_thread:
            return
        # clip any non-matching messages
        self.thread_matched = None
        m = self.thread_start.dup()
        while m < self.thread_end:
            mt = self.call("doc:get-attr", m, "matched", ret='str')
            if mt != "True":
                m2 = m.dup()
                self.call("doc:step-matched", m2, 1, 1)
                self.leaf.call("Notify:clip", m, m2)
                m = m2
            if not self.thread_matched:
                self.thread_matched = m.dup()
            self.parent.next(m)
        self.leaf.call("view:changed", self.thread_start, self.thread_end)

    def handle_notify_thread(self, key, str, num, **a):
        "handle:notmuch:thread-changed"
        if not str or self.selected != str:
            return 0
        if num == 0:
            self.close_thread(True)
        elif num == 1:
            self.move_thread()
        elif num == 2:
            self.trim_thread()
            self.updating = False
        return 1

    def handle_notify_thread_open(self, key, str1, **a):
        "handle:notmuch:thread-open"
        # If we have requested an update (so .updating is set) pretend
        # the thread isn't open, so a full update happens
        return 1 if not self.updating and str1 and self.selected == str1 else 0

    def handle_set_ref(self, key, mark, num, **a):
        "handle:doc:set-ref"
        start = num
        if start:
            if self.whole_thread:
                mark.to_mark(self.thread_start)
                return 1
            if (self.selected and self.thread_matched and
                self.parent.prior(self.thread_start) is None):
                # first thread is open
                mark.to_mark(self.thread_matched)
                return 1
        # otherwise fall-through to real start or end
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
            ret = self.handle_step(key,focus, mark, forward, 1)
            steps -= forward * 2 - 1
        if end:
            return 1 + (num - steps if forward else steps - num)
        if ret == edlib.WEOF or num2 == 0:
            return ret
        if num and (num2 < 0) == (num > 0):
            return ret
        # want the next character
        return self.handle_step(key, focus, mark, 1 if num2 > 0 else 0, 0)

    def handle_step(self, key, focus, mark, num, num2):
        forward = num
        move = num2
        if self.whole_thread:
            # move one message, but stop at thread_start/thread_end
            if forward:
                if mark < self.thread_start:
                    mark.to_mark(self.thread_start)
                if mark >= self.thread_end:
                    ret = edlib.WEOF
                    if mark.pos:
                        focus.call("doc:set-ret", mark, 0)
                        mark.step(0)
                else:
                    ret = self.parent.call("doc:char", focus, mark,
                                           1 if move else 0, 0 if move else 1)
                    if mark.pos and mark.pos[0] != self.selected:
                        focus.call("doc:set-ref", mark, 0)
                        mark.step(0)
            else:
                if mark <= self.thread_start:
                    # at start already
                    ret = edlib.WEOF
                else:
                    if mark > self.thread_end:
                        mark.to_mark(self.thread_end)
                    ret = self.parent.call("doc:char", focus, mark,
                                           -1 if move else 0, 0 if move else -1)
            return ret
        else:
            # if between thread_start/thread_end, move one message,
            # else move one thread
            if not self.thread_start:
                in_thread = False
            elif forward and mark >= self.thread_end:
                in_thread = False
            elif not forward and mark <= self.thread_start:
                in_thread = False
            elif forward and mark < self.thread_start:
                in_thread = False
            elif not forward and mark > self.thread_end:
                in_thread = False
            else:
                in_thread = True
            if in_thread:
                # move one matched message
                ret = self.parent.call("doc:step-matched", focus, mark, forward, move)
                # We might be in the next thread, make sure we are at the
                # start
                if forward and move and mark > self.thread_end:
                    mark.to_mark(self.thread_end)
                if not forward and move and mark < self.thread_start:
                    focus.call("doc:step-thread", mark, forward, move)
                if not forward and move and mark == self.thread_start:
                    # must be at the start of the first thread, which is open.
                    if self.thread_matched:
                        mark.to_mark(self.thread_matched)
                return ret
            else:
                # move one thread
                if forward:
                    ret = focus.call("doc:step-thread", focus, mark, forward, move)
                    if (self.thread_matched and
                        self.thread_start and mark == self.thread_start):
                        mark.to_mark(self.thread_matched)
                else:
                    # make sure we are at the start of the thread
                    self.parent.call("doc:step-thread", focus, mark, forward, 1)
                    ret = self.parent.call("doc:char", focus, mark,
                                           -1 if move else 0, 0 if move else -1)
                    if move and ret != edlib.WEOF:
                        focus.call("doc:step-thread", focus, mark, forward, move)
                return ret

    def handle_get_attr(self, key, focus, mark, num, num2, str, comm2, **a):
        "handle:doc:get-attr"
        if mark is None:
            mark = focus.call("doc:point", ret='mark')
        if not mark or not mark.pos:
            return edlib.Efallthrough
        if self.whole_thread and mark >= self.thread_end:
            # no attributes after end
            return 1
        attr = str
        if attr == "BG":
            (tid,mid) = mark.pos
            if tid == self.selected and comm2:
                if mid == self.selmsg:
                    comm2("cb", focus, "bg:magenta+60", mark, attr)
                elif self.whole_thread:
                    # only set background for matched messages
                    mt = self.call("doc:get-attr", mark, "matched", ret='str')
                    if mt and mt == "True":
                        comm2("cb", focus, "bg:yellow+60", mark, attr)
                else:
                    comm2("cb", focus, "bg:yellow+60", mark, attr)
            return 1
        if attr[:3] == "TM-":
            if self.thread_start and mark < self.thread_end and mark >= self.thread_start:
                return self.parent.call("doc:get-attr", focus, num, num2, mark, "M-" + str[3:], comm2)
            else:
                return self.parent.call("doc:get-attr", focus, num, num2, mark, "T-" + str[3:], comm2)
        if attr == "message-id":
            # high message-id when thread isn't open
            if not self.selected or not mark.pos or mark.pos[0] != self.selected:
                return 1

        return edlib.Efallthrough

    def handle_Z(self, key, focus, **a):
        "handle:doc:char-Z"
        if not self.thread_start:
            return 1
        if self.whole_thread:
            # all non-match messages in this thread are about to
            # disappear, we need to clip them.
            if self.thread_matched:
                mk = self.thread_start.dup()
                mt = self.thread_matched.dup()
                while mk < self.thread_end:
                    if mk < mt:
                        focus.call("Notify:clip", mk, mt)
                    mk.to_mark(mt)
                    self.parent.call("doc:step-matched", mt, 1, 1)
                    self.parent.next(mk)
            else:
                focus.call("Notify:clip", self.thread_start, self.thread_end)
            # everything after to EOF moves to thread_end.
            eof = edlib.Mark(self)
            self.leaf.call("doc:set-ref", eof, 0)
            eof.step(1)
            eof.offset = 1 # make sure all eof marks are different
            self.leaf.call("Notify:clip", self.thread_end, eof, 1)
            eof.offset = 0

            self['doc-status'] = "Query: %s" % self['qname']
        else:
            # everything before the thread, and after the thread disappears
            m = edlib.Mark(self)
            focus.call("Notify:clip", m, self.thread_start)
            focus.call("doc:set-ref", m, 0)
            focus.call("Notify:clip", self.thread_end, m)
            self['doc-status'] = "Query: %s - single-thread" % self['qname']
        self.whole_thread = not self.whole_thread
        # notify that everything is changed, don't worry about details.
        focus.call("view:changed")
        return 1

    def handle_update(self, key, focus, **a):
        "handle:doc:char-="
        if self.selected:
            self.updating = True
            focus.call("doc:notmuch:load-thread", self.thread_start)
        focus.call("doc:notmuch:query:reload")
        return 1

    def handle_close_whole_thread(self, key, focus, **a):
        "handle:notmuch:close-whole-thread"
        # 'q' is requesting that we close thread if it is open
        if not self.whole_thread:
            return edlib.Efalse
        self.handle_Z(key, focus)
        return 1

    def handle_close_thread(self, key, focus, **a):
        "handle:notmuch:close-thread"
        if self.close_thread():
            return 1
        return edlib.Efalse

    def handle_select(self, key, focus, mark, num, num2, str1, **a):
        "handle:notmuch:select"
        # num = 0 - open thread but don't show message
        # num > 0 - open thread and do show message
        # num < 0 - open thread, go to last message, and show
        # if 'str1' and that thread exists, go there instead of mark
        if not mark:
            return edlib.Efail
        if str1:
            m = mark.dup()
            if self.call("notmuch:find-message", m, str1) > 0:
                mark.to_mark(m)

        s = focus.call("doc:get-attr", "thread-id", mark, ret='str')
        if s and s != self.selected:
            self.close_thread()

            ret = focus.call("doc:notmuch:load-thread", mark)
            if ret == 2:
                focus.call("Message", "Cannot load thread %s" % s)
            if ret == 1:
                self.selected = s
                if mark:
                    self.thread_start = mark.dup()
                else:
                    self.thread_start = focus.call("doc:dup-point", 0,
                                                   edlib.MARK_UNGROUPED, ret='mark')
                focus.call("doc:step-thread", 0, 1, self.thread_start)
                self.thread_end = self.thread_start.dup()
                focus.call("doc:step-thread", 1, 1, self.thread_end)
                self.thread_matched = self.thread_start.dup()
                matched = focus.call("doc:get-attr", self.thread_matched, "matched", ret="str")
                if matched != "True":
                    focus.call("doc:step-matched", 1, 1, self.thread_matched)
                focus.call("view:changed", self.thread_start, self.thread_end)
                self.thread_start.step(0)
                if num < 0:
                    # we moved backward to land here, so go to last message
                    m = self.thread_end.dup()
                    focus.call("doc:step-matched", 0, 1, m)
                else:
                    # choose first new, unread or thread_matched
                    new = None; unread = None
                    m = self.thread_matched.dup()
                    while focus.call("doc:get-attr", m, "thread-id",
                                     ret='str') == s:
                        tg = focus.call("doc:get-attr", m, "tags", ret='str')
                        tl = tg.split(',')
                        if "unread" in tg:
                            if not unread:
                                unread = m.dup()
                            if "new" in tg and not new:
                                new = m.dup()
                        focus.call("doc:step-matched", 1, 1, m)
                    if new:
                        m = new
                    elif unread:
                        m = unread
                    else:
                        m = self.thread_matched
                # all marks on this thread get moved to chosen start
                focus.call("Notify:clip", self.thread_start, m)
                if mark:
                    mark.clip(self.thread_start, m)
                pnt = self.call("doc:point", ret='mark')
                if pnt:
                    pnt['notmuch:selected'] = s
        if num != 0:
            # thread-id shouldn't have changed, but it some corner-cases
            # it can, so get both ids before loading the message.
            s = focus.call("doc:get-attr", "thread-id", mark, ret='str')
            s2 = focus.call("doc:get-attr", "message-id", mark, ret='str')
            if s and s2:
                focus.call("notmuch:select-message", s2, s)
                self.selmsg = s2
                self.call("view:changed")
        self.take_focus()
        return 1

    def handle_reposition(self, key, focus, mark, mark2, **a):
        "handle:render:reposition"
        # some messages have been displayed, from mark to mark2
        # collect threadids and message ids
        if not mark or not mark2:
            return edlib.Efallthrough
        m = mark.dup()

        while m < mark2:
            tg = focus.call("doc:get-attr", "tags", m, ret='str')
            if tg and 'new' in tg.split(','):
                i1 = focus.call("doc:get-attr", "thread-id", m, ret='str')
                i2 = focus.call("doc:get-attr", "message-id", m, ret='str')
                if i1 and not i2 and i1 not in self.seen_threads:
                    self.seen_threads[i1] = True
                if i1 and i2:
                    if i1 in self.seen_threads:
                        del self.seen_threads[i1]
                    if i2 not in self.seen_msgs:
                        self.seen_msgs[i2] = i1
            if self.next(m) is None:
                break
        return edlib.Efallthrough

    def handle_mark_seen(self, key, focus, **a):
        "handle:notmuch:mark-seen"
        for id in self.seen_threads:
            notmuch_set_tags(thread=id, remove=['new'])
            self.notify("Notify:Tag", id)

        for id in self.seen_msgs:
            notmuch_set_tags(msg=id, remove=['new'])
            self.notify("Notify:Tag", self.seen_msgs[id], id)

        self.seen_threads =  {}
        self.seen_msgs = {}
        return 1

class notmuch_message_view(edlib.Pane):
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus)
        # Need to set default visibility on each part.
        # step forward with doc:step-part and for any 'odd' part,
        # which is a spacer, we look at email:path and email:content-type.
        # If alternative:[1-9] is found, or type isn't "text*", make it
        # invisible.
        self['notmuch:pane'] = 'message'
        p = 0
        focus.call("doc:notmuch:request:Notify:Tag", self)
        self.do_handle_notify_tag()

        # a 'view' for recording where quoted sections are
        self.qview = focus.call("doc:add-view", self) - 1

        self.extra_headers = False
        self.point = focus.call("doc:point", ret='mark')
        self.prev_point = None
        self.have_prev = False
        self.call("doc:request:mark:moving")
        self.menu = None
        self.addr = None

        self['word-wrap'] = '1' # Should this be different in different parts?

        choose = {}
        m = edlib.Mark(focus)
        while True:
            self.call("doc:step-part", m, 1)
            which = focus.call("doc:get-attr", "multipart-this:email:which",
                               m, ret='str')
            if not which:
                break
            if which != "spacer":
                continue
            path = focus.call("doc:get-attr",
                              "multipart-prev:email:path", m, ret='str')
            type = focus.call("doc:get-attr",
                              "multipart-prev:email:content-type", m, ret='str')
            disp = focus.call("doc:get-attr",
                              "multipart-prev:email:content-disposition", m, ret='str')
            fname = focus.call("doc:get-attr",
                               "multipart-prev:email:filename", m, ret='str')
            ext = None
            if fname and '/' in fname:
                fname = os.path.basename(prefix)
            prefix = fname
            if fname and '.' in fname:
                d = fname.rindex('.')
                ext = fname[d:]
                prefix = fname[:d]
            if not ext and type:
                ext = mimetypes.guess_extension(type)
            if ext:
                # if there is an extension, we can pass to xdg-open, so maybe
                # there is an external viewer
                focus.call("doc:set-attr", "multipart-prev:email:ext", m, ext)
                focus.call("doc:set-attr", "multipart-prev:email:prefix", m, prefix)
                focus.call("doc:set-attr", "multipart-prev:email:actions", m,
                           "hide:save:external view");

            if (type.startswith("text/") and
                (not disp or "attachment" not in disp)):
                # mark up URLs and quotes in any text part.
                # The part needs to be visible while we do this.
                # Examine at most 50000 chars from the start.
                self.set_vis(focus, m, True)
                start = m.dup()
                self.prev(start)
                self.call("doc:step-part", start, 0)
                end = start.dup()
                self.call("doc:char", end, 10000, m)

                self.call("url:mark-up", start, end)
                self.mark_quotes(start, end)

            # When presented with alternatives we are supposed to show
            # the last alternative that we understand.  However html is
            # only partly understood, so I only want to show that if
            # there is no other option.... but now I have w3m let's try
            # prefering html...
            # An alternate may contain many parts and we need to make
            # everything invisible within an invisible part - and we only
            # know which is invisible when we get to the end.
            # So record the visibility of each group of alternatives
            # now, and the walk through again setting visibility.
            # An alternative itself may be multi-part, typically
            # multipart/related.  In this case we only look at
            # whether we can handle the type of the first part.
            p = path.split(',')
            i = len(p)-1
            while (i > 0 and p[i].endswith(":0") and
                   not p[i].startswith("alternative:")):
                # Might be the first part of a multi-path alternative,
                # look earlier in the path
                i -= 1
            if p[i].startswith("alternative:"):
                # this is one of several - can we handle it?
                group = ','.join(p[:i])
                this = p[i][12:]
                if type in ['text/plain', 'text/calendar', 'text/rfc822-headers',
                            'message/rfc822']:
                    choose[group] = this
                if type.startswith('image/'):
                    choose[group] = this
                if type == 'text/html': # and group not in choose:
                    choose[group] = this

        # Now go through and set visibility for alternates.
        m = edlib.Mark(focus)
        while True:
            self.call("doc:step-part", m, 1)
            which = focus.call("doc:get-attr", "multipart-this:email:which",
                               m, ret='str')
            if not which:
                break
            if which != "spacer":
                continue
            path = focus.call("doc:get-attr", "multipart-prev:email:path",
                              m, ret='str')
            type = focus.call("doc:get-attr", "multipart-prev:email:content-type",
                              m, ret='str')
            disp = focus.call("doc:get-attr", "multipart-prev:email:content-disposition",
                              m, ret='str')

            vis = False

            # Is this allowed to be visible by default?
            if (not type or
                type.startswith("text/") or
                type.startswith("image/")):
                vis = True

            if disp and "attachment" in disp:
                # Attachments are never visible - even text.
                vis = False

            # Is this in a non-selected alternative?
            p = []
            for el in path.split(','):
                p.append(el)
                if el.startswith("alternative:"):
                    group = ','.join(p[:-1])
                    this = el[12:]
                    if choose[group] != this:
                        vis = False

            self.set_vis(focus, m, vis)

    def set_vis(self, focus, m, vis):
        if vis:
            it = focus.call("doc:get-attr", "multipart-prev:email:is_transformed", m, ret='str')
            if it and it == "yes":
                focus.call("doc:set-attr", "email:visible", m, "transformed")
            else:
                focus.call("doc:set-attr", "email:visible", m, "orig")
        else:
            focus.call("doc:set-attr", "email:visible", m, "none")


    def mark_quotes(self, ms, me):
        # if we find more than 7 quoted lines in a row, we add the
        # 4th and 4th-last to the qview with the first of these
        # having a 'quote-length' attr with number of lines
        ms = ms.dup()
        while ms < me:
            try:
                self.call("text-search", "^>", ms, me)
            except:
                return
            self.prev(ms)
            start = ms.dup()
            cnt = 1
            while (cnt <= 7 and self.call("doc:EOL", 1, 1, ms) > 0 and
                   self.following(ms) == '>'):
                cnt += 1
            if cnt > 7:
                try:
                    self.call("text-search", "^[^>]", ms, me)
                    self.prev(ms)
                except:
                    ms.to_mark(me)
                self.mark_one_quote(start, ms.dup())

    def mark_one_quote(self, ms, me):
        self.call("doc:EOL", 3, 1, ms)  # Start of 3rd line
        self.call("doc:EOL", -4, 1, me) # End of 4th last line
        if me <= ms:
            return
        st = edlib.Mark(self, self.qview)
        st.to_mark(ms)
        lines = 0
        while ms < me:
            self.call("doc:EOL", 1, 1, ms)
            lines += 1
        st['quote-length'] = "%d" % lines
        st['quote-hidden'] = "yes"
        ed = edlib.Mark(orig=st)
        ed.to_mark(me)

    def do_handle_notify_tag(self):
        # tags might have changed.
        tg = self.call("doc:notmuch:byid:tags", self['notmuch:id'], ret='str')
        if tg:
            self['doc-status'] = "Tags:" + tg
        else:
            self['doc-status'] = "No Tags"
        return 1
    def handle_notify_tag(self, key, str1, str2, **a):
        "handle:Notify:Tag"
        if str1 != self['notmuch:tid']:
            # not my thread
            return
        if str2 and str2 != self['notmuch:id']:
            # not my message
            return
        return self.do_handle_notify_tag()

    def handle_close(self, key, **a):
        "handle:Close"
        self.call("notmuch-close-message")
        return 1

    def handle_clone(self, key, focus, **a):
        "handle:Clone"
        p = notmuch_message_view(focus)
        self.clone_children(focus.focus)
        return 1

    def handle_replace(self, key, **a):
        "handle:Replace"
        return 1

    def handle_slash(self, key, focus, mark, **a):
        "handle:doc:char-/"
        s = focus.call("doc:get-attr", mark, "email:visible", ret='str')
        if not s:
            return 1
        self.set_vis(focus, mark, s == "none")
        return 1

    def handle_space(self, key, focus, mark, **a):
        "handle:doc:char- "
        if focus.call("K:Next", 1, mark) == 2:
            focus.call("doc:char-n", mark)
        return 1

    def handle_backspace(self, key, focus, mark, **a):
        "handle:K:Backspace"
        if focus.call("K:Prior", 1, mark) == 2:
            focus.call("doc:char-p", mark)
        return 1

    def handle_return(self, key, focus, mark, **a):
        "handle:K:Enter"
        m = self.vmark_at_or_before(self.qview, mark)
        if m and not m['quote-length'] and m == mark:
            # at end of last line, wan't previous mark
            m = m.prev()
        if m and m['quote-length']:
            if m['quote-hidden'] == 'yes':
                m['quote-hidden'] = "no"
            else:
                m['quote-hidden'] = "yes"
            self.leaf.call("view:changed", m, m.next())
            return 1

    def handle_vis(self, focus, mark, which):
        v = focus.call("doc:get-attr", mark, "email:visible", ret='str')
        self.parent.call("email:select:" + which, focus, mark)
        v2 = focus.call("doc:get-attr", mark, "email:visible", ret='str')
        if v != v2 or which == "extras":
            # when visibility changes, move point to start.
            focus.call("doc:email-step-part", mark, -1)
            pt = focus.call("doc:point", ret='mark');
            pt.to_mark(mark)
        return 1

    def handle_toggle_hide(self, key, focus, mark, **a):
        "handle-list/email-hide/email:select:hide"
        return self.handle_vis(focus, mark, "hide")

    def handle_toggle_full(self, key, focus, mark, **a):
        "handle-list/email-full/email:select:full"
        return self.handle_vis(focus, mark, "full")

    def handle_toggle_extras(self, key, focus, mark, **a):
        "handle-list/email-extras/email:select:extras/doc:char-X"
        if not mark:
            # a mark at the first "sep" part will identify the headers
            mark = edlib.Mark(focus)
            focus.call("doc:email-step-part", mark, 1)
        self.handle_vis(focus, mark, "extras")
        if self.extra_headers:
            return 1
        self.extra_headers = 1
        hdrdoc = focus.call("doc:multipart:get-part", 1, ret='pane')
        point = hdrdoc.call("doc:vmark-new", edlib.MARK_POINT, ret='mark')
        hdrdoc.call("doc:set-ref", point)
        for i in range(10):
            f = self['notmuch:fn-%d' % i]
            if not f:
                break
            if f == self['filename']:
                continue
            hdr = "Filename-%d: " % i
            hdrdoc.call("doc:replace", 1, point, point, hdr,
                        ",render:rfc822header=%d" % (len(hdr)-1))
            hdrdoc.call("doc:replace", 1, point, point, f + '\n')
        hdrdoc.call("doc:replace", 1, point, point, "Thread-id: ",
                    ",render:rfc822header=10")
        hdrdoc.call("doc:replace", 1, point, point, self['notmuch:tid'] + '\n')
        try:
            ts = self['notmuch:timestamp']
            ts = int(ts)
        except:
            ts = 0
        if ts > 0:
            tm = time.strftime("%a, %d %b %Y %H:%M:%S", time.localtime(ts))
            hdrdoc.call("doc:replace", 1, point, point, "Local-Time: ",
                    ",render:rfc822header=11")
            hdrdoc.call("doc:replace", 1, point, point, tm + '\n')
        return 1

    def handle_save(self, key, focus, mark, **a):
        "handle-list/email-save/email:select:save"

        file = focus.call("doc:get-attr", "multipart-prev:email:filename", mark, ret='str')
        if not file:
            file = "edlib-saved-file"
        p = file.split('/')
        b = p[-1]
        part = focus.call("doc:get-attr", mark, "multipart:part-num", ret='str')
        part = int(part)-2
        fn = "/tmp/" + b
        f = open(fn, "w")
        content = focus.call("doc:multipart-%d-doc:get-bytes" % part, ret = 'bytes')
        f.buffer.write(content)
        f.close()
        focus.call("Message", "Content saved as %s" % fn)
        return 1

    def handle_external(self, key, focus, mark, **a):
        "handle-list/email-external view/email:select:external view"
        type = focus.call("doc:get-attr", "multipart-prev:email:content-type", mark, ret='str')
        prefix = focus.call("doc:get-attr", "multipart-prev:email:prefix", mark, ret='str')
        ext = focus.call("doc:get-attr", "multipart-prev:email:ext", mark, ret='str')
        if not ext:
            ext = ""
        if not prefix:
            prefix = "tempfile"

        part = focus.call("doc:get-attr", mark, "multipart:part-num", ret='str')
        part = int(part)-2

        content = focus.call("doc:multipart-%d-doc:get-bytes" % part, ret = 'bytes')
        fd, path = tempfile.mkstemp(ext, prefix)
        os.write(fd, content)
        os.close(fd)
        focus.call("Display:external-viewer", path, prefix+"XXXXX"+ext)
        return 1

    def handle_map_attr(self, key, focus, mark, str, str2, comm2, **a):
        "handle:map-attr"
        if str == "render:rfc822header":
            comm2("attr:callback", focus, int(str2), mark, "fg:#6495ed,nobold", 121)
            comm2("attr:callback", focus, 0, mark, "wrap-tail: ,wrap-head:    ",
                  121)
            return 1
        if str == "render:rfc822header-addr":
            w=str2.split(",")
            if "From" in w:
                comm2("attr:callback", focus, int(w[0]), mark,
                      "underline,action-menu:notmuch-addr-menu,addr-tag:"+w[1],
                      200)
            return 1
        if str == "render:rfc822header-wrap":
            comm2("attr:callback", focus, int(str2), mark, "wrap", 120)
            return 1
        if str == "render:rfc822header:subject":
            comm2("attr:callback", focus, 10000, mark, "fg:blue,bold", 120)
            return 1
        if str == "render:rfc822header:to":
            comm2("attr:callback", focus, 10000, mark, "word-wrap:0,fg:blue,bold", 120)
            return 1
        if str == "render:rfc822header:cc":
            comm2("attr:callback", focus, 10000, mark, "word-wrap:0", 120)
            return 1
        if str == "render:hide":
            comm2("attr:callback", focus, 100000 if str2 == "1" else -1,
                  mark, "hide", 100000)
        if str == "render:bold":
            comm2("attr:callback", focus, 100000 if str2 == "1" else -1,
                  mark, "bold", 120)
        if str == "render:internal":
            comm2("attr:callback", focus, 100000 if str2 == "1" else -1,
                  mark, "hide", 120)
        if str == "render:imgalt":
            comm2("attr:callback", focus, 100000 if str2 == "1" else -1,
                  mark, "fg:green-60", 120)
        if str == "render:char":
            w = str2.split(':')
            attr = None
            if w[1] and  w[1][0] == '!' and w[1] != '!':
                # not recognised, so highlight the name
                attr = "fg:magenta-60,bold"
            comm2("attr:callback", focus, int(w[0]), mark,
                  attr, 120, str2=w[1])
            # Don't show the html entity description, just the rendering.
            comm2("attr:callback", focus, int(w[0]), mark,
                  "hide", 60000)
        if str == 'start-of-line':
            m = self.vmark_at_or_before(self.qview, mark)
            bg = None
            if m and m['quote-length']:
                bg = "white-95"
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
                comm2("cb", focus, mark, 0, "fg:"+colours[cnt-1], 102)
                if bg:
                    comm2("cb", focus, mark, 0, "bg:"+bg, 102)
            return edlib.Efallthrough

    def handle_menu(self, key, focus, mark, xy, str1, **a):
        "handle:notmuch-addr-menu"
        if self.menu:
            self.menu.call("Cancel")
        for at in str1.split(','):
            if at.startswith("addr-tag:"):
                t = at[9:]
                addr = focus.call("doc:get-attr", 0, mark, "addr-"+t, ret='str')
        if not addr:
            return 1
        ad = email.utils.getaddresses([addr])
        if ad and ad[0] and len(ad[0]) == 2:
            addr = ad[0][1]
        focus.call("Message", "Menu for address %s" % addr)
        mp = self.call("attach-menu", "", "notmuch-addr-choice", xy, ret='pane')
        mp.call("menu-add", "C", "Compose")
        q = focus.call("doc:notmuch:get-query", "from-list", ret='str')
        if q:
            for t in q.split():
                if t.startswith("query:"):
                    t = t[6:]
                    qq = focus.call("doc:notmuch:get-query", t, ret='str')
                    if qq and ("from:"+addr) in qq:
                        mp.call("menu-add", "-" + t, 'Already in "%s"' % t)
                    else:
                        mp.call("menu-add", t, 'Add to "%s"' % t)
        mp.call("doc:file", -1)
        self.menu = mp
        self.addr = addr
        self.add_notify(mp, "Notify:Close")
        return 1

    def handle_notify_close(self, key, focus, **a):
        "handle:Notify:Close"
        if focus == self.menu:
            self.menu = None
            return 1
        return edlib.Efallthrough

    def handle_addr_choice(self, key, focus, mark, str1, **a):
        "handle:notmuch-addr-choice"
        if not str1 or not self.addr:
            return None
        if str1.startswith('-'):
            # already in this query
            return None
        q = focus.call("doc:notmuch:get-query", str1, ret='str')
        if type(q) == str:
            q = q + " from:" + self.addr
            if focus.call("doc:notmuch:set-query", str1, q) > 0:
                focus.call("Message",
                           "Updated query.%s with %s" % (str1, self.addr))
            else:
                focus.call("Message",
                           "Update for query.%s failed." % str1)
        return 1

    def handle_render_line(self, key, focus, num, mark, mark2, comm2, **a):
        "handle:doc:render-line"
        # If between active quote marks, render a simple marker
        p = self.vmark_at_or_before(self.qview, mark)
        if p and not p['quote-length'] and p == mark:
            # at end of last line
            p = p.prev()
        cursor_at_end = mark2 and mark2 > mark

        if not(p and p['quote-length'] and p['quote-hidden'] == 'yes'):
            return edlib.Efallthrough
        if num < 0:
            # render full line
            mark.to_mark(p.next())
            self.next(mark)
            eol="\n"
        if num >= 0:
            # don't move mark from start of line
            # So 'click' always places at start of line.
            eol = ""

        if comm2:
            line = "<fg:yellow,bg:blue+30>%d quoted lines</>%s" % (int(p['quote-length']), eol)
            if cursor_at_end:
                cpos = len(line)-1
            else:
                cpos = 0

            return comm2("cb", focus, cpos, line)
        return 1

    def handle_render_line_prev(self, key, focus, num, mark, comm2, **a):
        "handle:doc:render-line-prev"
        # If between active quote marks, move to start first
        p = self.vmark_at_or_before(self.qview, mark)
        if not(p and p['quote-length'] and p['quote-active'] == 'yes'):
            return edlib.Efallthrough
        mark.to_mark(p)
        return edlib.Efallthrough

    def handle_moving(self, key, focus, mark, mark2, **a):
        "handle:mark:moving"
        if mark == self.point and not self.have_prev:
            # We cannot dup because that triggers a recursive notification
            #self.prev_point = mark.dup()
            self.prev_point = self.vmark_at_or_before(self.qview, mark)
            self.have_prev = True
            self.damaged(edlib.DAMAGED_VIEW)
        return 1

    def handle_review(self, key, focus, **a):
        "handle:Refresh:view"
        # if point is in a "quoted line" section that is hidden,
        # Move it to start or end opposite prev_point
        if not self.have_prev:
            return 1
        m = self.vmark_at_or_before(self.qview, self.point)
        if m and m != self.point and m['quote-length'] and m['quote-hidden'] == "yes":
            if not self.prev_point or self.prev_point < self.point:
                # moving toward end of file
                m = m.next()
            if self.point != m:
                self.point.to_mark(m)
        self.prev_point = None
        self.have_prev = False
        return 1

def notmuch_doc(key, home, focus, comm2, **a):
    # Create the root notmuch document
    nm = notmuch_main(home)
    nm['render-default'] = "notmuch:master-view"
    nm.call("doc:set-name", "*Notmuch*")
    nm.call("global-multicall-doc:appeared-")
    nm.call("doc:notmuch:update")
    if comm2 is not None:
        comm2("callback", focus, nm)
    return 1

def render_query_attach(key, focus, comm2, **a):
    p = notmuch_query_view(focus)
    p["format:no-linecount"] = "1"
    p = p.call("attach-render-format", ret='pane')
    p['render-wrap'] = 'yes'
    if comm2:
        comm2("callback", p)
    return 1

def render_message_attach(key, focus, comm2, **a):
    p = focus.call("attach-email-view", ret='pane')
    p = notmuch_message_view(p)
    if p:
        p2 = p.call("attach-render-url-view", ret='pane')
        if p2:
            p = p2
    if comm2:
        comm2("callback", p)
    return 1

def render_master_view_attach(key, focus, comm2, **a):
    # The master view for the '*Notmuch*' document uses multiple tiles
    # to display the available searches, the current search results, and the
    # current message, though each of these is optional.
    # The tile which displays the search list does not have a document, as it
    # refers down the main document.  So it doesn't automatically get borders
    # from a 'view', so we must add one explicitly.
    # We assume 'focus' is a 'view' pane on a "doc" pane for the notmuch primary doc,
    # which is (probably) on a "tile" pane.
    # The master view needs to be below the "doc" pane, above the tile.
    # We attach it on focus and use Pane.reparent to rotate
    # from:  master_view -> view -> doc -> tile
    #   to:  view -> doc -> master_view -> tile

    doc = focus.parent
    main = notmuch_master_view(focus)
    doc.reparent(main)
    p = main.call("attach-tile", "notmuch", "main", ret='pane')
    # Now we have tile(main) -> view -> doc -> master_view -> tile
    # and want tile(main) above doc
    doc.reparent(p)
    # Now 'view' doesn't have a child -we give it 'list_view'
    p = notmuch_list_view(focus)
    p = p.call("attach-render-format", ret='pane')
    main.list_pane = p
    p.take_focus()
    comm2("callback", p)
    return 1

def notmuch_pane(focus):
    p0 = focus.call("ThisPane", ret='pane')
    try:
        p1 = focus.call("docs:byname", "*Notmuch*", ret='pane')
    except edlib.commandfailed:
        p1 = focus.call("attach-doc-notmuch", ret='pane')
    if not p1:
        return None
    return p1.call("doc:attach-view", p0, ret='pane')

def notmuch_mode(key, focus, **a):
    if notmuch_pane(focus):
        return 1
    return edlib.Efail

def notmuch_compose(key, focus, **a):
    choice = []
    def choose(choice, a):
        focus = a['focus']
        if focus['email-sent'] == 'no':
            choice.append(focus)
            return 1
        return 0
    focus.call("docs:byeach", lambda key,**a:choose(choice, a))
    if len(choice):
        par = focus.call("ThisPane", ret='pane')
        if par:
            par = choice[0].call("doc:attach-view", par, 1, ret='pane')
            par.take_focus()
    else:
        try:
            db = focus.call("docs:byname", "*Notmuch*", ret='pane')
        except edlib.commandfailed:
            db = focus.call("attach-doc-notmuch", ret='pane')
        v = make_composition(db, focus, "ThisPane")
        if v:
            v.call("compose-email:empty-headers")
    return 1

def notmuch_search(key, focus, **a):
    p = notmuch_pane(focus)
    if p:
        p.call("doc:char-s")
    return 1

edlib.editor.call("global-set-command", "attach-doc-notmuch", notmuch_doc)
edlib.editor.call("global-set-command", "attach-render-notmuch:master-view",
                  render_master_view_attach)
edlib.editor.call("global-set-command", "attach-render-notmuch:threads",
                  render_query_attach)
edlib.editor.call("global-set-command", "attach-render-notmuch:message",
                  render_message_attach)
edlib.editor.call("global-set-command", "interactive-cmd-nm", notmuch_mode)
edlib.editor.call("global-set-command", "interactive-cmd-nmc", notmuch_compose)
edlib.editor.call("global-set-command", "interactive-cmd-nms", notmuch_search)
