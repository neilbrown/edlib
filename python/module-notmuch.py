# -*- coding: utf-8 -*-
# Copyright Neil Brown ©2016-2020 <neil@brown.name>
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
# saved search are stored in config file as "saved.foo". Some are special.
# "saved.current" selects messages that have not been archived, and are not spam
# "saved.unread" selects messages that should be highlighted. It is normally "tag:unread"
# "saved.new" selects new messages. Normally "tag:new not tag:unread"
# "saved.current-list" should be a conjunction of "saved:" searches.  They are listed
#  in the "search list" together with a count of 'current' and 'current/new' messages.
# "saved.misc-list" is a subset of current-list for which saved:current should not
# be assumed.

from subprocess import Popen, PIPE
import re
import os
import notmuch
import json
import time
import fcntl

class notmuch_db():
    # This class is designed to be used with "with ... as" and provides
    # locking within the body of the clause.

    def __init__(self):
        self.lock_path = os.environ["HOME"]+"/.notmuch-config"
        self.want_write = False
        self.fd = None
        self.nest = 0

    def get_write(self):
        if self.want_write:
            return self
        if self.nest:
            return None
        self.want_write = True
        return self
    def __enter__(self):
        if self.nest:
            self.nest += 1
            return self.db
        self.fd = open(self.lock_path)
        self.nest = 1
        if self.want_write:
            fcntl.flock(self.fd.fileno(), fcntl.LOCK_EX)
            self.db = notmuch.Database(mode = notmuch.Database.MODE.READ_WRITE)
        else:
            fcntl.flock(self.fd.fileno(), fcntl.LOCK_SH)
            self.db = notmuch.Database()
        return self.db

    def __exit__(self, a,b,c):
        self.nest -= 1
        if self.nest:
            return
        self.db.close()
        self.fd.close()
        self.db = None
        self.fd = None

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
        # /dev/null is to avoid warning when out-of-date data is reported.
        self.p = Popen("/usr/bin/notmuch count --batch", shell=True, stdin=PIPE,
                       stdout = PIPE, stderr = open("/dev/null", 'w'))
        self.p.stdin.write((self.make_search(q) + "\n").encode("utf-8"))
        self.p.stdin.write((self.make_search(q, 'unread') + "\n").encode("utf-8"))
        self.p.stdin.write((self.make_search(q, 'new') + "\n").encode("utf-8"))
        self.p.stdin.close()
        self.pane.call("event:read", self.p.stdout.fileno(), self.ready)
        return True

    def ready(self, key, **a):
        q = self.pending
        count = 0; unread = 0; new = 0
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
        self.cb(q, count, unread, new, more)
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
        self.worker = counter(self.make_search, pane, self.updated)
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
            line = line.decode("utf-8")
            if line[:6] != "saved.":
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
            self.slist["current-list"] = "saved:inbox saved:unread"
            if "current" not in self.slist:
                self.slist["misc-list"] = "saved:inbox saved:unread"
            if "inbox" not in self.slist:
                self.slist["inbox"] = "tag:inbox"
            if "saved:unread" not in self.slist:
                self.slist["unread"] = "tag:inbox AND tag:unread"

        if "misc-list" not in self.slist:
            self.slist["misc-list"] = ""
        if "unread" not in self.slist:
            self.slist["unread"] = "tag:unread"
        if "new" not in self.slist:
            self.slist["new"] = "(tag:new AND tag:unread)"

        self.current = self.searches_from("current-list")
        self.misc = self.searches_from("misc-list")

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
        return self.worker.is_pending(search)

    def update(self):
        for i in self.current:
            self.worker.enqueue(i)
        return self.worker.pending != None

    def update_one(self, search):
        self.worker.enqueue(search, True)

    def updated(self, q, count, unread, new, more):
        self.count[q] = count
        self.unread[q] = unread
        self.new[q] = new
        self.cb(more)

    patn = "\\bsaved:([-_A-Za-z0-9:]*)\\b"
    def map_search(self, query):
        m = re.search(self.patn, query)
        while m:
            s = m.group(1)
            if s in self.slist:
                q = self.slist[s]
                query = re.sub('saved:' + s,
                               '(' + q + ')', query)
            else:
                query = re.sub('saved:' + s,
                               'saved-'+s, query)
            m = re.search(self.patn, query)
        return query

    def make_search(self, name, extra = None):
        s = "saved:" + name
        if name not in self.misc:
            s = s + " saved:current"
        if extra:
            s = s + " AND saved:" + extra
        return self.map_search(s)

    def searches_from(self, n):
        ret = []
        if n in self.slist:
            for s in self.slist[n].split(" "):
                if s[:6] == "saved:":
                    ret.append(s[6:])
        return ret

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
    # FIXME we track "seen_threads" in each thread-list so that we can
    # later mark them as no-longer "new".  Why is this here rather than
    # in the thread-list document?

    def __init__(self, focus):
        edlib.Doc.__init__(self, focus)
        self.searches = searches(self, self.updated)
        self.timer_set = False
        self.updating = None
        self.seen_threads = {}
        self.seen_msgs = {}
        self.container = edlib.Pane(self.root)
        self.db = notmuch_db()

    def handle_close(self, key, **a):
        "handle:Close"
        self.container.close()
        return 1

    def handle_set_ref(self, key, mark, num, **a):
        "handle:doc:set-ref"
        self.to_end(mark, num == 0)
        if num == 1:
            mark.offset = 0
        else:
            mark.offset = len(self.searches.current)
        return 1

    def handle_step(self, key, mark, num, num2, **a):
        "handle:doc:step"
        forward = num
        move = num2
        ret = edlib.WEOF
        target = mark
        if forward and mark.offset < len(self.searches.current):
            ret = '\n'
            if move:
                mark.step(forward)
                mark.offset = mark.offset + 1
        if not forward and mark.offset > 0:
            ret = '\n'
            if move:
                mark.step(forward)
                mark.offset = mark.offset - 1
        return ret

    def handle_doc_get_attr(self, key, focus, mark, str, comm2, **a):
        "handle:doc:get-attr"
        # This must support the line-format used in notmuch_list_view

        attr = str
        o = mark.offset
        val = None
        if o >= 0 and o < len(self.searches.current):
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
                    val = "bg:pink,"+val
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
                else:
                    val = "%5d" % c
            elif attr == 'space':
                p = self.searches.is_pending(s)
                if p == 1:
                    val = '*'
                elif p > 1:
                    val = '?'
                else:
                    val = ' '
        if val:
            comm2("callback", focus, val, mark, str)
            return 1
        return edlib.Efallthrough

    def handle_get_attr(self, key, focus, str, comm2, **a):
        "handle:get-attr"
        if comm2:
            if str == "doc-type":
                comm2("callback", focus, "notmuch")
                return 1
            if str == "notmuch:max-search-len":
                comm2("callback", focus, "%d" % self.searches.maxlen)
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
        with self.db as db:
            tags = db.get_all_tags()
            self.searches.set_tags(tags)
        if self.searches.load(False):
            # there are (possibly) new searches, trigger a refresh
            self.notify("doc:replaced")
        self.updating = "counts"
        if not self.searches.update():
            self.update_next()
        return 1

    def handle_notmuch_update_one(self, key, str, **a):
        "handle:doc:notmuch:update-one"
        self.searches.update_one(str)
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
        if nm and nm.notify("doc:notify-viewers") == 0:
            # no-one is looking, just discard this one
            nm.close()
            nm = None
        if not nm:
            nm = notmuch_query(self, str, q)
            # FIXME This is a a bit ugly.  I should pass self.container
            # as the parent, but notmuch_query needs to stash maindoc
            # Also I should use an edlib call to get notmuch_query
            nm.reparent(self.container)
            nm.call("doc:set-name", str)
        if comm2:
            comm2("callback", focus, nm)
        return 1

    def handle_notmuch_byid(self, key, focus, str, comm2, **a):
        "handle:doc:notmuch:byid"
        # Return a document for the email message.
        # This is a global document.
        with self.db as db:
            m = db.find_message(str)
            fn = m.get_filename() + ""
        doc = focus.call("doc:open", "email:"+fn, -2, ret='focus')
        if doc:
            doc['notmuch:id'] = str
            comm2("callback", doc)
        return 1

    def handle_notmuch_byid_tags(self, key, focus, num2, str, comm2, **a):
        "handle:doc:notmuch:byid:tags"
        # return a string with tags of message
        with self.db as db:
            m = db.find_message(str)
            tags = ",".join(m.get_tags())
        comm2("callback", focus, tags)
        return 1

    def handle_notmuch_bythread_tags(self, key, focus, str, comm2, **a):
        "handle:doc:notmuch:bythread:tags"
        # return a string with tags of all messages in thread
        with self.db as db:
            q = db.create_query("thread:%s" % str)
            tg = []
            for t in q.search_threads():
                ml = t.get_messages()
                for m in ml:
                    tg.extend(m.get_tags())
        tags = ",".join(set(tg))
        comm2("callback", focus, tags)
        return 1

    def handle_notmuch_query_updated(self, key, **a):
        "handle:doc:notmuch:query-updated"
        # A child search document has finished updating.
        self.update_next()
        return 1

    def handle_notmuch_mark_read(self, key, str, str2, **a):
        "handle:doc:notmuch:mark-read"
        with self.db.get_write() as db:
            m = db.find_message(str2)
            if m:
                changed=False
                t = list(m.get_tags())
                if "unread" in t:
                    m.remove_tag("unread")
                    changed=True
                if "new" in t:
                    m.remove_tag("new")
                    changed=True
                if changed:
                    self.notify("Notify:Tag", str, str2)
        return 1

    def handle_notmuch_remove_tag(self, key, str, str2, **a):
        "handle-prefix:doc:notmuch:remove-tag-"
        tag = key[23:]
        with self.db.get_write() as db:
            if str2:
                # remove just from 1 message
                m = db.find_message(str2)
                if m:
                    t = list(m.get_tags())
                    if tag in t:
                        m.remove_tag(tag)
                        self.notify("Notify:Tag", str, str2)
            else:
                # remove from whole thread
                # FIXME This should be the thread as last seen, not as
                # is now in the database - which might be different.
                q = db.create_query("thread:%s" % str)
                changed = False
                for t in q.search_threads():
                    ml = t.get_messages()
                    for m in ml:
                        if tag in m.get_tags():
                            m.remove_tag(tag)
                            changed = True
                if changed:
                    self.notify("Notify:Tag", str)
        return 1

    def handle_notmuch_add_tag(self, key, str, str2, **a):
        "handle-prefix:doc:notmuch:add-tag-"
        tag = key[20:]
        with self.db.get_write() as db:
            if str2:
                # add just to 1 message
                m = db.find_message(str2)
                if m:
                    t = list(m.get_tags())
                    if tag not in t:
                        m.add_tag(tag)
                        self.notify("Notify:Tag", str, str2)
            else:
                # add to whole thread
                # FIXME This should be the thread as last seen, not as
                # is now in the database - which might be different.
                q = db.create_query("thread:%s" % str)
                changed = False
                for t in q.search_threads():
                    ml = t.get_messages()
                    for m in ml:
                        if tag not in m.get_tags():
                            m.add_tag(tag)
                            changed = True
                if changed:
                    self.notify("Notify:Tag", str)
        return 1

    def handle_notmuch_remember_seen_thread(self, key, focus, str, **a):
        "handle:doc:notmuch:remember-seen-thread"
        if str:
            self.seen_threads[str] = focus
            return 1

    def handle_notmuch_remember_seen_msg(self, key, focus, str, **a):
        "handle:doc:notmuch:remember-seen-msg"
        if str:
            self.seen_msgs[str] = focus
            return 1

    def handle_notmuch_mark_seen(self, key, focus, **a):
        "handle:doc:notmuch:mark-seen"
        with self.db.get_write() as db:
            todel = []
            for id in self.seen_msgs:
                if self.seen_msgs[id] == focus:
                    m = db.find_message(id)
                    if m and "new" in m.get_tags():
                        m.remove_tag("new")
                        self.notify("Notify:Tag", m.get_thread_id(), id)
                    todel.append(id)
            for id in todel:
                del self.seen_msgs[id]
            todel = []
            for id in self.seen_threads:
                if self.seen_threads[id] == focus:
                    q = db.create_query("thread:%s" % id)
                    for t in q.search_threads():
                        ml = t.get_messages()
                        for m in ml:
                            if "new" in m.get_tags():
                                m.remove_tag("new")
                        break
                    todel.append(id)
                self.notify("Notify:Tag", id)
            for id in todel:
                del self.seen_threads[id]
        return 1

    def tick(self, key, **a):
        if not self.updating:
            self.updating = "counts"
            self.searches.load(False)
            if not self.searches.update():
                self.update_next()
        return 1

    def updated(self, finished):
        if finished:
            self.update_next()
        self.notify("doc:replaced")

    def update_next(self):
        if self.updating == "counts":
            self.updating = "queries"
            qlist = []
            for c in self.container.children():
                qlist.append(c['query'])
            self.qlist = qlist
        while self.updating == "queries":
            if not self.qlist:
                self.updating = None
            else:
                q = self.qlist.pop(0)
                for c in self.container.children():
                    if c['query'] == q:
                        c("doc:notmuch:query-refresh")
                        return

# notmuch_query document
# a mark.pos is a list of thread-id and message-id.  The hash
# self.unique_pos is use to ensure marks with the same pos have literally
# identical " is " values

class notmuch_query(edlib.Doc):
    def __init__(self, focus, qname, query):
        edlib.Doc.__init__(self, focus)
        self.db = notmuch_db()
        self.maindoc = focus
        self.query = query
        self['qname'] = qname
        self['query'] = query
        self.threadids = []
        self.threads = {}
        self.messageids = {}
        self.threadinfo = {}
        self.unique_pos = {}
        self["render-default"] = "notmuch:threads"
        self["line-format"] = "<%BG><%TM-hilite>%TM-date_relative</><tab:130></> <fg:blue>%TM-authors</><tab:350>%TM-threadinfo<tab:450><%TM-hilite>%TM-subject</></>                      "
        self.add_notify(self.maindoc, "Notify:Tag")
        self.add_notify(self.maindoc, "Notify:Close")
        self.load_full()

    def makepos(self, thread, msg = None):
        p = [thread, msg]
        if msg:
            k = thread + msg
        else:
            k = thread
        if k in self.unique_pos:
            p = self.unique_pos[k]
        else:
            self.unique_pos[k] = p
        # FIXME unique_pos never shinks.
        return p

    def setpos(self, mark, thread, msgnum = 0):
        if thread is None:
            mark.pos = None
            return
        if thread in self.messageids:
            msg = self.messageids[thread][msgnum]
        else:
            msg = None
        mark.pos = self.makepos(thread, msg)
        mark.offset = 0

    def load_full(self):
        self.partial = False
        self.age = 1

        # mark all threads inactive, so any that remain that way
        # can be pruned.
        for id in self.threads:
            self.threads[id]['total'] = 0
        self.offset = 0
        self.tindex = 0
        self.pos = edlib.Mark(self)
        self.start_load()

    def load_update(self):
        self.partial = True
        self.age = None

        self.offset = 0
        self.tindex = 0
        self.pos = edlib.Mark(self)
        self.start_load()

    def start_load(self):
        cmd = ["/usr/bin/notmuch", "search", "--output=summary", "--format=json", "--limit=100", "--offset=%d" % self.offset ]
        if self.partial:
            cmd += [ "date:-1day.. AND " ]
        elif self.age:
            cmd += [ "date:-%dmonths.. AND " % self.age]
        cmd += [ "( %s )" % self.query ]
        self.p = Popen(cmd, shell=False, stdout=PIPE)
        self.call("event:read", self.p.stdout.fileno(), self.get_threads)

    def move_marks(self, tid, new):
        # Thread 'tid' is being removed, or relocated.
        # Move all marks that point to it to the next thread, which
        # might be None
        m = self.first_mark()
        while m and m.pos:
            if m.pos[0] == tid:
                self.setpos(m, new, 0)
            m = m.next_any()

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
            if tid in self.threads:
                oj = self.threads[tid]
                if  (oj['timestamp'] != j['timestamp'] or
                     oj['total'] != j['total'] or
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
                self.threadids.insert(self.tindex, tid)
                self.tindex += 1
            if old >= 0 and old == self.tindex - 2:
                # same as immediate last - no need to move marks
                self.tindex -= 1
                self.threadids.pop(old)
            elif old >= 0:
                # move mark to before self.pos
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
                self.pos.step(0)
                while m and m.pos and m.pos[0] == tid:
                    m2 = m.next_any()
                    m.to_mark_noref(self.pos)
                    if m.seq > self.pos.seq:
                        # want m before pos
                        m.to_mark_noref(self.pos)
                    m = m2
                self.notify("notmuch:thread-changed", tid, 1)
            if need_update:
                if self.pos.pos and self.pos.pos[0] == tid:
                    self.load_thread(self.pos)
                else:
                    # might previous thread thread
                    m = self.pos.dup()
                    self.prev(m)
                    self.call("doc:step-thread", m, 0, 1)
                    if m.pos and m.pos[0] == tid:
                        self.load_thread(m)

        tl = None
        if self.p:
            self.p.wait()
        self.p = None
        if was_empty and self.threadids:
            # first insertion, all marks other than self.pos must be at start
            m = self.first_mark()
            while m and m.pos is None:
                m2 = m.next_any()
                if m.seq != self.pos.seq:
                    m.step(0)
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
                del self.threads[tid]
                self.threadids.remove(tid)
                m.step(0)
                while m < m2:
                    m.pos = m2.pos
                    m = m.next_any()
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

    def add_message(self, m, lst, info, depth):
        mid = m.get_message_id()
        lst.append(mid)
        l = list(m.get_replies())
        info[mid] = (m.get_filename(), m.get_date(),
                     m.get_flag(notmuch.Message.FLAG.MATCH),
                     depth + [1 if l else 0],
                     m.get_header("From"), m.get_header("Subject"), list(m.get_tags()))
        if l:
            l.sort(key=lambda m:(m.get_date(), m.get_header("subject")))
            for m in l[:-1]:
                self.add_message(m, lst, info, depth + [1])
            self.add_message(l[-1], lst, info, depth + [0])

    def load_thread(self, mark):
        (tid, mid) = mark.pos
        with self.db as db:
            q = notmuch.Query(db, "thread:%s and (%s)" % (tid, self.query))
            tl = list(q.search_threads())
            if not tl:
                q = notmuch.Query(db, "thread:%s" % (tid))
                tl = list(q.search_threads())
            if not tl:
                return
            thread = tl[0]
            midlist = []
            minfo = {}
            ml = list(thread.get_toplevel_messages())
            ml.sort(key=lambda m:(m.get_date(), m.get_header("subject")))
            for m in list(ml):
                self.add_message(m, midlist, minfo, [2])
            self.messageids[tid] = midlist
            self.threadinfo[tid] = minfo
        if mid is None:
            # need to update all marks at this location to hold mid
            m = mark
            pos = self.makepos(tid, midlist[0])
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
            val = time.strftime("%D %T!", then)
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
            mark.step(1)
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
            mark.step(1)
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
            mark.step(0)

            if j == 0:
                i -= 1
                tid = self.threadids[i]
                if tid in self.messageids:
                    j = len(self.messageids[tid])
                else:
                    j = 1
            j -= 1
            self.setpos(mark, tid, j)
            mark.step(0)

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

    def handle_notify_close(self, **a):
        "handle:Notify:Close"
        # Main doc is closing, so must we
        self.close()
        return 1

    def handle_set_ref(self, key, mark, num, **a):
        "handle:doc:set-ref"
        self.to_end(mark, num == 0)
        mark.pos = None
        if num == 1 and len(self.threadids) > 0:
            self.setpos(mark, self.threadids[0], 0)
        mark.offset = 0
        return 1

    def handle_step(self, key, mark, num, num2, **a):
        "handle:doc:step"
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
                if len(self.threadids) == 0:
                    return edlib.WEOF
                tid = self.threadids[-1]
            else:
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

    def handle_doc_get_attr(self, key, mark, focus, str, comm2, **a):
        "handle:doc:get-attr"
        attr = str
        if mark.pos == None:
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
            m = ("", 0, False, [0,0], "" ,"", [])
        (fn, dt, matched, depth, author, subj, tags) = m
        if attr == "message-id":
            val = mid
        elif attr == "thread-id":
            val = tid
        elif attr == "T-hilite":
            if "inbox" not in t["tags"]:
                # FIXME this test is wrong once we have generic searches
                val = "fg:grey"
            elif "new" in t["tags"] and "unread" in t["tags"]:
                val = "fg:red,bold"
            elif "unread" in t["tags"]:
                val = "fg:blue"
            else:
                val = "fg:black"
        elif attr == "T-date_relative":
            val = self.rel_date(t['timestamp'])
        elif attr == "T-threadinfo":
            val = "[%d/%d]" % (t['matched'],t['total'])
        elif attr[:2] == "T-" and attr[2:] in t:
            val = t[attr[2:]]
            if type(val) == int:
                val = "%d" % val
            else:
                val = t[attr[2:]]
            if attr == "T-authors":
                val = val[:20]

        elif attr == "matched":
            val = "True" if matched else "False"
        elif attr == "M-hilite":
            # FIXME this inbox test is wrong once we allow generic searches
            if not matched or "inbox" not in tags:
                val = "fg:grey"
                if "new" in tags and "unread" in tags:
                    val = "fg:pink"
            elif "new" in tags and "unread" in tags:
                val = "fg:red,bold"
            elif "unread" in tags:
                val = "fg:blue"
            else:
                val = "fg:black"
        elif attr == "M-date_relative":
            val = self.rel_date(dt)
        elif attr == "M-authors":
            val = author[:20]
        elif attr == "M-subject":
            val = subj
        elif attr == "M-threadinfo":
            val = self.cvt_depth(depth)

        if not val is None:
            comm2("callback", focus, val, mark, attr)
        return 1

    def handle_get_attr(self, key, focus, str, comm2, **a):
        "handle:get-attr"
        if str == "doc-type" and comm2:
            comm2("callback", focus, "notmuch-query")
            return 1
        return edlib.Efallthrough

    def handle_maindoc(self, key, **a):
        "handle-prefix:doc:notmuch:"
        # any doc:notmuch calls that we don't handle directly
        # are handled to the maindoc
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
        self.load_thread(mark)
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
        if str not in self.threadinfo:
            edlib.LOG("notmuch:mark-read cannot find %s" % str)
            return edlib.Efail
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
        if not is_unread:
            # thread is no longer 'unread'
            j = self.threads[str]
            t = j["tags"]
            if "unread" in t:
                t.remove("unread")
        self.notify("doc:replaced")
        # Cached info is updates, pass down to
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

#
# There are 4 viewer
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
            tile = self.list_pane.call("ThisPane", "notmuch", ret='focus')
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
            tile = self.query_pane.call("ThisPane", "notmuch", ret='focus')
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
            if str in ["qname","query"] and self.query_pane:
                val = self.query_pane[str]
            if val:
                comm2("callback", focus, val, str)
                return 1
        return edlib.Efallthrough

    def handle_choose(self, key, **a):
        "handle:docs:choose"
        # don't choose anything  FIXME when is this helpful?
        return 1

    def handle_clone(self, key, focus, **a):
        "handle:Clone"
        main = notmuch_master_view(focus)
        p = main.call("attach-tile", "notmuch", "main", ret='focus')
        frm = self.list_pane.call("ThisPane", "notmuch", ret='focus')
        frm.clone_children(p)
        return 1

    recursed = False
    def handle_maindoc(self, key, **a):
        "handle-prefix:doc:notmuch:"
        # any doc:notmuch calls that haven't been handled
        # are handled to the list_pane
        if self.recursed:
            return edlib.Efail
        self.recursed = True
        ret = self.list_pane.call(key, **a)
        self.recursed = False
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

    def handle_space(self, key, **a):
        "handle:doc:char- "
        if self.message_pane:
            self.message_pane.call(key)
        elif self.query_pane:
            self.query_pane.call("K:Enter")
        else:
            self.list_pane.call("K:Enter")
        return 1

    def handle_move(self, key, mark, **a):
        "handle-list/K:A-n/K:A-p/doc:char-n/doc:char-p"
        if key.startswith("K:A-") or not self.query_pane:
            p = self.list_pane
            op = self.query_pane
        else:
            p = self.query_pane
            op = self.message_pane
        if not p:
            return 1
        m = mark
        direction = 1 if key[-1] in "na" else -1
        if op:
            # secondary window exists so move, otherwise just select
            # Need to get point as 'mark' might be in the wrong pane
            p.call("Move-Line", direction)
            m = p.call("doc:dup-point", 0, -2, ret='mark')

        p.call("notmuch:select", m, direction)
        return 1

    def handle_A(self, key, focus, mark, str, **a):
        "handle-list/doc:char-a/doc:char-S/doc:char-N/doc:char-*/doc:char-!/"
        # adjust flags for this message or thread, and move to next
        # a - remove inbox
        # S - add newspam
        # N - remove newspam and add notspam
        # * - add flagged
        # ! - add unread,inbox remove newspam,notspam,flagged
        in_message = False
        in_query = False
        in_main = False
        if self.message_pane and self.mychild(focus) == self.mychild(self.message_pane):
            in_message = True
        elif self.query_pane and self.mychild(focus) == self.mychild(self.query_pane):
            in_query = True
        else:
            in_main = True

        adds = []; removes = []
        if key[-1] == 'a':
            removes = ['inbox']
        if key[-1] == 'S':
            adds = ['newspam']
        if key[-1] == 'N':
            adds = ['notspam']
            removes = ['newspam']
        if key[-1] == '*':
            adds = ['flagged']
        if key[-1] == '!':
            adds = ['unread','inbox']
            removes = ['newspam','notspam','flagged']

        if in_message:
            mp = self.message_pane
            if mp.cmid and mp.ctid:
                self.do_update(mp.ctid, mp.cmid, adds, removes)
            self.call("doc:char-n")
            return 1
        if in_query:
            thid = focus.call("doc:get-attr", "thread-id", mark, ret = 'str')
            msid = focus.call("doc:get-attr", "message-id", mark, ret = 'str')
            self.do_update(thid, msid, adds, removes)
            # Move to next message.
            m = focus.call("doc:dup-point", 0, -2, ret='mark')
            if focus.call("Move-Line", 1, m) == 1:
                focus.call("Move-to", m)
            if self.message_pane:
                # Message was displayed, so display this one
                focus.call("notmuch:select", m, 0)
            return 1
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
                self.list_pane.call("doc:notmuch:add-tag-%s" % t, tid, mid)
            else:
                skipped.append(t)
        for t in removes:
            if self.tag_ok(t):
                self.list_pane.call("doc:notmuch:remove-tag-%s" % t, tid, mid)
            else:
                skipped.append(t)
        if skipped:
            self.list_pane.call("Message", "Skipped illegal tags:" + ','.join(skipped))

    def handle_tags(self, key, focus, mark, **a):
        "handle-list/doc:char--/doc:char-+"
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
            thid = self.message_pane.ctid
            msid = self.message_pane.cmid

        pup = focus.call("PopupTile", "2", key[-1:], ret='focus')
        if not pup:
            return edlib.Fail
        done = "notmuch-do-tags-%s" % thid
        if msid:
            done += " " + msid
        pup['done-key'] = done
        pup['prompt'] = "[+/-]Tags: "
        pup.call("doc:set-name", "Tag changes")
        tag_popup(pup)
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

    def do_tags(self, key, focus, str, **a):
        "handle-prefix:notmuch-do-tags-"
        suffix = key[16:]
        ids = suffix.split(' ', 1)
        thid = ids[0]
        if len(ids) == 2:
            msid = ids[1]
        else:
            msid = None
        t = self.parse_tags(str)
        if t is None:
            focus.call("Message", "Tags list must start with + or -")
        self.do_update(thid, msid, t[0], t[1])
        return 1

    def handle_close_message(self, key, **a):
        "handle:notmuch-close-message"
        self.message_pane = None
        return 1

    def handle_xq(self, key, **a):
        "handle-list/doc:char-x/doc:char-q"
        if self.message_pane:
            if key != "doc:char-x":
                self.mark_read()
            p = self.message_pane
            self.message_pane = None
            p.call("Window:close", "notmuch")
        elif self.query_pane:
            if self.query_pane.call("notmuch:close-thread") == 1:
                return 1
            if key != "doc:char-x":
                self.query_pane.call("doc:notmuch:mark-seen")
            p = self.query_pane
            self.query_pane = None

            s = p.call("get-attr", "qname", 1, ret='str')
            if s:
                p.call("doc:notmuch:update-one", s)

            p.call("Window:close", "notmuch")
        else:
            p = self.call("ThisPane", ret='focus')
            if p and p.focus:
                p.focus.close()
        return 1

    def handle_v(self, key, **a):
        "handle:doc:char-V"
        # View the current message as a raw file
        if not self.message_pane:
            return 1
        p2 = self.call("doc:open", self.message_pane["filename"], -1,
                       ret = 'focus')
        p2.call("doc:set:autoclose", 1)
        p0 = self.call("DocPane", p2, ret='focus')
        if not p0:
            p0 = self.call("OtherPane", ret = 'focus')
        if not p0:
            return 1
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

    def handle_select_query(self, key, num, str, **a):
        "handle:notmuch:select-query"
        # A query was selected, identifed by 'str'.  Close the
        # message window and open a threads window.
        if self.message_pane:
            p = self.message_pane
            self.message_pane = None
            p.call("Window:close", "notmuch")
        if self.query_pane:
            self.query_pane.call("doc:notmuch:mark-seen")
        if self.query_pane:
            # update summaries for the query pane we are replacing.
            s = self.query_pane.call("get-attr", "qname", 1, ret='str')
            if s:
                self.list_pane.call("doc:notmuch:update-one", s)

        p0 = self.list_pane.call("doc:notmuch:query", str, ret='focus')
        p1 = self.list_pane.call("OtherPane", "notmuch", "threads", 15,
                                 ret = 'focus')
        self.query_pane = p0.call("doc:attach-view", p1, ret='focus')
        if num:
            self.query_pane.take_focus()
        self.resize()
        return 1

    def handle_select_message(self, key, focus, num, str, str2, **a):
        "handle:notmuch:select-message"
        # a thread or message was selected. id in 'str'. threadid in str2
        # Find the file and display it in a 'message' pane
        self.mark_read()

        p0 = self.list_pane.call("doc:notmuch:byid", str, ret='focus')
        if not p0:
            focus.call("Message", "Failed to find message")
            return edlib.Efail
        p0['notmuch:tid'] = str2

        p1 = self.query_pane.call("OtherPane", "notmuch", "message", 13,
                                  ret='focus')
        p3 = p0.call("doc:attach-view", p1, ret='focus')
        p3 = p3.call("attach-render-notmuch:message", ret='focus')

        # FIXME This still doesn't work: there are races: attaching a doc to
        # the pane causes the current doc to be closed.  But the new doc
        # hasn't been anchored yet so if they are the same, we lose.
        # Need a better way to anchor a document.
        #p0.call("doc:set:autoclose", 1)
        p = self.message_pane = p3
        p.ctid = str2
        p.cmid = str
        if num:
            self.message_pane.take_focus()
        self.resize()
        return 1

    def mark_read(self):
        p = self.message_pane
        if not p:
            return
        if self.query_pane:
            self.query_pane.call("doc:notmuch:mark-read", p.ctid, p.cmid)

class notmuch_list_view(edlib.Pane):
    # This pane provides view on the search-list document.
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus)
        self['render-wrap'] = 'no'
        self['background'] = 'color:#A0FFFF'
        self['line-format'] = '<%fmt>%count%space%name</>'
        self.call("notmuch:set_list_pane")
        self.call("doc:request:doc:replaced")
        self.selected = None

    def handle_clone(self, key, focus, **a):
        "handle:Clone"
        p = notmuch_list_view(focus)
        self.clone_children(focus.focus)
        return 1

    def handle_notify_replace(self, key, **a):
        "handle:doc:replaced"
        # FIXME do I need to do anything here? - of not, why not
        return 0

    def handle_select(self, key, focus, mark, num, **a):
        "handle:notmuch:select"
        s = focus.call("doc:get-attr", "query", mark, ret='str')
        if s:
            focus.call("notmuch:select-query", s, num)
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

        self['doc-status'] = "query: %s" % self['qname']
        # thread_start and thread_end are marks which deliniate
        # the 'current' thread. thread_end is the start of the next
        # thread (if there is one).
        # thread_matched is the first matched message in the thread.
        self.thread_start = None
        self.thread_end = None
        self.thread_matched = None
        (xs,ys) = self.scale()
        ret = []
        self.call("text-size", "M", -1, ys,
                  lambda key, **a: ret.append(a))
        if ret:
            lh = ret[0]['xy'][1]
        else:
            lh = 1
        # fixme adjust for pane size
        self['render-vmargin'] = "%d" % (4 * lh)
        self.call("doc:request:doc:replaced")
        self.call("doc:request:notmuch:thread-changed")

    def handle_clone(self, key, focus, **a):
        "handle:Clone"
        p = notmuch_query_view(focus)
        self.clone_children(focus.focus)
        return 1

    def handle_notify_replace(self, key, **a):
        "handle:doc:replaced"
        self.leaf.call("view:changed")
        return 1

    def close_thread(self, gone = False):
        if not self.selected:
            return
        # old thread is disappearing.
        self.leaf.call("Notify:clip", self.thread_start, self.thread_end,
                       0 if gone else 1)
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
        return 1

    def handle_set_ref(self, key, mark, num, **a):
        "handle:doc:set-ref"
        start = num
        if start:
            if self.whole_thread:
                mark.to_mark(self.thread_start)
                return 1
            if self.thread_matched and self.parent.prior(mark) is None:
                # first thread is open
                mark.to_mark(self.thread_matched)
                return 1
        # otherwise fall-through to real start or end
        return edlib.Efallthrough

    def handle_step(self, key, focus, mark, num, num2, **a):
        "handle:doc:step"
        forward = num
        move = num2
        if self.whole_thread:
            # move one message, but stop at thread_start/thread_end
            if forward:
                if mark < self.thread_start:
                    mark.to_mark(self.thread_start)
                if mark >= self.thread_end:
                    ret = edlib.WEOF
                else:
                    ret = self.parent.call("doc:step", focus, forward, move, mark)
            else:
                if mark <= self.thread_start:
                    # at start already
                    ret = edlib.WEOF
                else:
                    if mark > self.thread_end:
                        mark.to_mark(self.thread_end)
                    ret = self.parent.call("doc:step", focus, forward, move, mark)
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
                return ret
            else:
                # move one thread
                if forward:
                    ret = focus.call("doc:step-thread", focus, mark, forward, move)
                    if self.thread_start and mark == self.thread_start:
                        mark.to_mark(self.thread_matched)
                else:
                    # make sure we are at the start of the thread
                    self.parent.call("doc:step-thread", focus, mark, forward, 1)
                    ret = self.parent.call("doc:step", focus, mark, forward, move)
                    if move and ret != edlib.WEOF:
                        focus.call("doc:step-thread", focus, mark, forward, move)
                return ret

    def handle_get_attr(self, key, focus, mark, num, num2, str, comm2, **a):
        "handle:doc:get-attr"
        if mark is None:
            mark = focus.call("doc:point", ret='mark')
        if not mark.pos:
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
                else:
                    comm2("cb", focus, "bg:yellow+60", mark, attr)
            return 1
        if attr[:3] == "TM-":
            if self.thread_start and mark < self.thread_end and mark >= self.thread_start:
                return self.parent.call("doc:get-attr", focus, num, num2, mark, "M-" + str[3:], comm2)
            else:
                return self.parent.call("doc:get-attr", focus, num, num2, mark, "T-" + str[3:], comm2)
        return edlib.Efallthrough

    def handle_Z(self, key, focus, **a):
        "handle:doc:char-Z"
        if not self.thread_start:
            return 1
        if self.whole_thread:
            # all non-match messages in this thread are about to
            # disappear, we need to clip them.
            mk = self.thread_start.dup()
            mt = self.thread_matched.dup()
            while mk < self.thread_end:
                if mk < mt:
                    focus.call("Notify:clip", mk, mt)
                mk.to_mark(mt)
                self.parent.call("doc:step-matched", mt, 1, 1)
                self.parent.next(mk)
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
            focus.call("doc:notmuch:load-thread", self.thread_start)
        focus.call("doc:notmuch:query:reload")
        return 1

    def handle_close_thread(self, key, focus, **a):
        "handle:notmuch:close-thread"
        # 'q' is requesting that we close thread if it is open
        if not self.whole_thread:
            return edlib.Efalse
        self.handle_Z(key, focus)
        return 1

    def handle_select(self, key, focus, mark, num, num2, str, **a):
        "handle:notmuch:select"
        s = focus.call("doc:get-attr", "thread-id", mark, ret='str')
        if s and s != self.selected:
            self.close_thread()

            if focus.call("doc:notmuch:load-thread", mark) == 1:
                self.selected = s
                if mark:
                    self.thread_start = mark.dup()
                else:
                    self.thread_start = focus.call("doc:dup-point", 0, -2, ret='mark')
                focus.call("doc:step-thread", 0, 1, self.thread_start)
                self.thread_end = self.thread_start.dup()
                focus.call("doc:step-thread", 1, 1, self.thread_end)
                self.thread_matched = self.thread_start.dup()
                matched = focus.call("doc:get-attr", self.thread_matched, "matched", ret="str")
                if matched != "True":
                    focus.call("doc:step-matched", 1, 1, self.thread_matched)
                focus.call("view:changed", self.thread_start, self.thread_end)
                # all marks on this thread must be moved to thread_matched
                self.thread_start.step(0)
                m = self.thread_matched
                if num < 0:
                    # we moved backward to land here, so go to last message
                    m = self.thread_end.dup()
                    focus.call("doc:step-matched", 0, 1, m)
                focus.call("Notify:clip", self.thread_start, m)
                if mark:
                    mark.clip(self.thread_start, m)
        if num != 0:
            s2 = focus.call("doc:get-attr", "message-id", mark, ret='str')
            if s2:
                focus.call("notmuch:select-message", s2, s, num)
                self.selmsg = s2
        return 1

    def handle_reposition(self, key, focus, mark, mark2, **a):
        "handle:render:reposition"
        # some messages have been displayed, from mark to mark2
        # collect threadids and message ids
        if not mark or not mark2:
            return edlib.Efallthrough
        m = mark.dup()

        while m < mark2:
            i1 = focus.call("doc:get-attr", "thread-id", m, ret='str')
            i2 = focus.call("doc:get-attr", "message-id", m, ret='str')
            if i1 and not i2 and i1 not in self.seen_threads:
                self.seen_threads[i1] = True
            if i1 and i2:
                if i1 in self.seen_threads:
                    del self.seen_threads[i1]
                if i2 not in self.seen_msgs:
                    self.seen_msgs[i2] = True
            if focus.next(m) is None:
                break

    def handle_mark_seen(self, key, focus, mark, mark2, str, **a):
        "handle:doc:notmuch:mark-seen"
        for i in self.seen_threads:
            focus.call("doc:notmuch:remember-seen-thread", i)
        for i in self.seen_msgs:
            focus.call("doc:notmuch:remember-seen-msg", i)
        return edlib.Efallthrough

class notmuch_message_view(edlib.Pane):
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus)
        # Need to set default visibility on each part.
        # step forward with doc:step-part and for any 'odd' part,
        # which is a spacer, we look at email:path and email:content-type.
        # If alternative:[1-9] is found, or type isn't "text*", make it
        # invisible.
        p = 0
        m = edlib.Mark(focus)
        focus.call("doc:notmuch:request:Notify:Tag", self)
        self.handle_notify_tag("Notify:Tag")
        while True:
            newp = self.call("doc:step-part", m, 1)
            # retval is offset by 1 to avoid zero
            newp -= 1
            if newp <= p:
                # reached the end
                break
            p = newp
            if not (p & 1):
                continue
            if p < 2:
                continue
            path = focus.call("doc:get-attr", "multipart-prev:email:path", m, ret='str')
            type = focus.call("doc:get-attr", "multipart-prev:email:content-type", m, ret='str')
            vis = True
            for  el in path.split('.'):
                if el[:12] == "alternative:" and el != "alternative:0":
                    vis = False
            if type[:4] != "text" and type != "":
                vis = False
            if not vis:
                focus.call("doc:set-attr", "email:visible", m, 0)

    def handle_notify_tag(self, key, **a):
        "handle:Notify:Tag"
        # tags might have changed.
        tg = self.call("doc:notmuch:byid:tags", self['notmuch:id'], ret='str')
        if tg:
            self['doc-status'] = "Tags:" + tg
        else:
            self['doc-status'] = "No Tags"
        return 1

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
        if s == "0":
            focus.call("doc:set-attr", mark, "email:visible", "1")
        else:
            focus.call("doc:set-attr", mark, "email:visible", "0")
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
        focus.call("doc:email:select", mark)
        return 1

    def handle_activate(self, key, focus, mark, **a):
        "handle:Mouse-Activate"
        focus.call("doc:email:select", mark)
        return 1

    def handle_map_attr(self, key, focus, mark, str, str2, comm2, **a):
        "handle:map-attr"
        if str == "render:rfc822header":
            comm2("attr:callback", focus, int(str2), mark, "fg:#6495ed", 21)
            comm2("attr:callback", focus, 10000, mark, "wrap-tail: ,wrap-head:    ", 19)
            return 1
        if str == "render:rfc822header-wrap":
            comm2("attr:callback", focus, int(str2), mark, "wrap", 20)
            return 1
        if str == "render:rfc822header-subject":
            comm2("attr:callback", focus, int(str2), mark, "fg:blue,bold", 20)
            return 1
        if str == "render:rfc822header-to":
            comm2("attr:callback", focus, int(str2), mark, "fg:blue,bold", 20)
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
    p = focus.call("attach-render-format", ret='focus')
    p = notmuch_query_view(p)
    if comm2:
        comm2("callback", p)
    return 1

def render_message_attach(key, focus, comm2, **a):
    p = focus.call("attach-email-view", ret='focus')
    p = notmuch_message_view(p)
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

    doc = focus.parent
    main = notmuch_master_view()
    doc.reparent(main)
    p = main.call("attach-tile", "notmuch", "main", ret='focus')
    doc.reparent(p)
    p = focus.call("attach-render-format", ret='focus')
    p = notmuch_list_view(p)
    main.list_pane = p
    p.take_focus()
    comm2("callback", p)
    return 1

def notmuch_mode(key, home, focus, **a):
    p0 = focus.call("ThisPane", ret = 'focus')
    try:
        p1 = home.call("docs:byname", "*Notmuch*", ret='focus')
    except edlib.commandfailed:
        p1 = home.call("attach-doc-notmuch", ret='focus')
    if not p1:
        return edlib.Efail
    p1.call("doc:attach-view", p0)
    return 1

if "editor" in globals():
    editor.call("global-set-command", "attach-doc-notmuch", notmuch_doc)
    editor.call("global-set-command", "attach-render-notmuch:master-view",
                render_master_view_attach)
    editor.call("global-set-command", "attach-render-notmuch:threads",
                render_query_attach)
    editor.call("global-set-command", "attach-render-notmuch:message",
                render_message_attach)
    editor.call("global-set-command", "interactive-cmd-nm", notmuch_mode)
