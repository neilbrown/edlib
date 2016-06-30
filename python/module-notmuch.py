
# edlib module for working with "notmuch" email.
#
# Two document types:
# - search list: list of saved searches with count of 'current', 'unread',
#   and 'new' messages
# - message list: provided by notmuch-search, though probably with enhanced threads
#
# Messages and composition is handled by a separte 'email' module.  This module
# will send messages to that module and provide services for composing and delivery.
#
# These can be all in with one pane, with sub-panes, or can sometimes have a pane to
# themselves.
#
# saved search are stored in config file as "saved.foo". Some are special.
# "saved.current" selects messages that have not been archived, and are not spam
# "saved.unread" selects messages that should be highlighted. It is normally "tag:unread"
# "saved.new" selects new messages. Normally "tag:new not tag:unread"
# "saved.current-list" should be a conjunction of "saved:" searches.  They are listed
#  in the "search list" together with a count of 'current' and 'current/new' messages.
# "saved.misc-list" is a subsect of current-list for which saved:current should not
# be assumed.

from subprocess import Popen, PIPE
import re
import os
#import notmuch

class searches:
    # Manage the saved searches
    # We read all searches from the config file and periodically
    # update some stored counts.
    #
    # This is used to present the search-list document.
    def __init__(self):
        self.slist = {}
        self.current = []
        self.misc = []
        self.count = {}
        self.unread = {}
        self.new = {}

        if 'NOTMUCH_CONFIG' in os.environ:
            self.path = os.environ['NOTMUCH_CONFIG']
        elif 'HOME' in os.environ:
            self.path = os.environ['HOME'] + "/.notmuch-config"
        else:
            self.path = ".notmuch-config"
        self.mtime = 0

    def load(self, reload = False):
        try:
            stat = os.stat(self.path)
            mtime = stat.st_mtime
        except OSError:
            mtime = 0
        if reload and mtime <= self.mtime:
            return False

        p = Popen("notmuch config list", shell=True, stdout=PIPE)
        if not p:
            return False
        self.slist = {}
        for line in p.stdout:
            if line[:6] != "saved.":
                continue
            w = line[6:].strip().split("=", 1)
            self.slist[w[0]] = w[1]
        try:
            p.communicate()
        except IOError:
            pass
        if "current-list" not in self.slist:
            self.slist["current-list"] = "saved:inbox saved:unread"
            if "inbox" not in self.slist:
                self.slist["inbox"] = "tag:inbox"
            if "saved:unread" not in self.slist:
                self.slist["unread"] = "tag:unread"

        if "misc-list" not in self.slist:
            self.slist["misc-list"] = ""
        if "unread" not in self.slist:
            self.slist["unread"] = "tag:unread"
        if "new" not in self.slist:
            self.slist["new"] = "(tag:new AND tag:unread)"

        self.current = self.searches_from("current-list")
        self.misc = self.searches_from("misc-list")
        for i in self.current:
            if i not in self.count:
                self.count[i] = None
                self.unread[i] = None
                self.new[i] = None
        self.mtime = mtime
        return True

    def old_update(self):
        for i in self.current:
            q = notmuch.Query(self.db, self.make_search(i, False))
            self.count[i] = q.count_messages()
            q = notmuch.Query(self.db, self.make_search(i, 'unread'))
            self.unread[i] = q.count_messages()
            q = notmuch.Query(self.db, self.make_search(i, 'new'))
            self.new[i] = q.count_messages()

    def update(self, pane, cb):
        self.todo = [] + self.current
        self.pane = pane
        self.cb = cb
        self.update_one()

    def update_one(self):
        if not self.todo:
            self.pane = None
            self.cb = None
            return
        n = self.todo[0]
        self.p = Popen("/usr/bin/notmuch count --batch", shell=True, stdin=PIPE,
                       stdout = PIPE)
        self.p.stdin.write(self.make_search(n, False) + "\n")
        self.p.stdin.write(self.make_search(n, 'unread') + "\n")
        self.p.stdin.write(self.make_search(n, 'new') + "\n")
        self.p.stdin.close()
        self.pane.call("event:read", self.p.stdout.fileno(), self.cb)

    def updated(self, *a):
        n = self.todo.pop(0)
        c = self.p.stdout.readline()
        u = self.p.stdout.readline()
        nw = self.p.stdout.readline()
        p = self.p
        self.p = None
        self.count[n] = int(c)
        self.unread[n] = int(u)
        self.new[n] = int(nw)
        self.update_one()
        p.wait()

    patn = "\\bsaved:([-_A-Za-z0-9]*)\\b"
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

    def make_search(self, name, extra):
        s = "saved:" + name
        if name not in self.misc:
            s = s + " saved:current"
        if extra:
            s = s + " saved:" + extra
        return self.map_search(s)

    def searches_from(self, n):
        ret = []
        if n in self.slist:
            for s in self.slist[n].split(" "):
                if s[:6] == "saved:":
                    ret.append(s[6:])
        return ret

class notmuch_main(edlib.Doc):
    # This is the document interface for the saved-search list.
    # It contain the searches as item which have attributes
    # providing name, count, unread-count
    # Once activated it auto-updates every 5 minutes
    def __init__(self, focus):
        edlib.Doc.__init__(self, focus, self.handle)
        self.searches = searches()
        self.timer_set = False

    def handle(self, key, **a):
        if key == "doc:set-ref":
            m = a['mark']
            if a['numeric'] == 1:
                m.offset = 0
            else:
                m.offset = len(self.searches.current)
            m.rpos = 0
            return 1

        if key == "doc:mark-same":
            m = a['mark']
            m2= a['mark2']
            return 1 if m.offset == m2.offset else 2

        if key == "doc:step":
            m = a['mark']
            forward = a['numeric']
            move = a['extra']
            ret = edlib.WEOF
            target = m
            if forward and m.offset < len(self.searches.current):
                ret = ' '
                if move:
                    m2 = m.next_any()
                    while m2 and m2.offset <= m.offset + 1:
                        target = m2
                        m2 = m2.next_any()
                    o = m.offset
                    m.to_mark(target)
                    m.offset = o+1
            if not forward and m.offset > 0:
                ret = ' '
                if move:
                    m2 = m.prev_any()
                    while m2 and m2.offset >= m.offset - 1:
                        target = m2
                        m2 = m2.prev_any()
                    o = m.offset
                    m.to_mark(target)
                    m.offset = o - 1
            return ret

        if key == "doc:get-attr":
            m = a['mark']
            attr = a['str']
            forward = a['numeric']
            o = m.offset
            if not forward:
                o -= 1
            val = None
            if o >= 0 and o < len(self.searches.current):
                s = self.searches.current[o]
                if attr == 'namefmt':
                    if self.searches.new[s]:
                        val = "bold,fg:red"
                    elif self.searches.unread[s]:
                        val = "bold"
                    else:
                        val = ""
                elif attr == 'unreadfmt':
                    if self.searches.unread[s]:
                        val = "bold"
                    else:
                        val = ""
                elif attr == 'name':
                        val = "%-12s" % s
                elif attr == 'count':
                    if self.searches.count[s] is None:
                        val = "%6s" % "?"
                    else:
                        val = "%6d" % self.searches.count[s]
                elif attr == 'unread':
                    if self.searches.unread[s] is None:
                        val = "%6s" % "?"
                    else:
                        val = "%6d" % self.searches.unread[s]
                elif attr == 'new':
                    if self.searches.new[s] is None:
                        val = "%6s" % "?"
                    else:
                        val = "%6d" % self.searches.new[s]
            a['comm2']("callback", a['focus'], val)
            return 1

        if key == "notmuch:update":
            if not self.timer_set:
                self.timer_set = True
                self.call("event:timer", 60*5, self.tick)
            self.searches.load(True)
            self.notify("Notify:Replace")
            self.searches.update(self, self.updated)
            return 1

    def tick(self, key, **a):
        if not self.searches.todo:
            self.searches.load(True)
            self.searches.update(self, self.updated)
        return 1

    def updated(self, key, **a):
        self.searches.updated()
        self.notify("Notify:Replace")
        return -1

def notmuch_doc(key, home, **a):
    # Create the root notmuch document
    nm = notmuch_main(home)
    nm['render-default'] = "notmuch:searchlist"
    nm.call("doc:set-name", "*Notmuch*")
    nm.call("global-multicall-doc:appeared-")
    if a['comm2'] is not None:
        cb = a['comm2']
        cb("callback", f, nm)
    return 1

class notmuch_main_view(edlib.Pane):
    # This pane provides view on the search-list document.
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus, self.handle)
        self['render-wrap'] = 'no'
        self['background'] = 'color:#A0FFFF'
        self['line-format'] = '<%namefmt>%+name</> %new <%unreadfmt>%unread</> %count'
        self.call("Request:Notify:Replace")

    def handle(self, key, focus, **a):
        if key == "Clone":
            p = notmuch_main_view(focus)
            self.clone_children(focus.focus)
            return 1
        if key == "Chr-g":
            focus.call("notmuch:update")
            self.damaged(edlib.DAMAGED_CONTENT|edlib.DAMAGED_VIEW)
            return 1
        if key == "Notify:Replace":
            self.damaged(edlib.DAMAGED_CONTENT|edlib.DAMAGED_VIEW)
            return 0

def render_searchlist_attach(key, focus, comm2, **a):
    p = focus.render_attach("format")
    p = p.render_attach("lines")
    p = notmuch_main_view(p)
    comm2("callback", p)
    return 1

if "editor" in globals():
    editor.call("global-set-command", pane, "attach-doc-notmuch", notmuch_doc)
    editor.call("global-set-command", pane, "attach-render-notmuch:searchlist",
                render_searchlist_attach)

    # This should be done by 'M-x notmuch' or similar - eventually
    editor.call("attach-doc-notmuch")
