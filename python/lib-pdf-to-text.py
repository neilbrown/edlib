# -*- coding: utf-8 -*-
# Copyright Neil Brown ©2021 <neil@brown.name>
# May be distributed under terms of GPLv2 - see file:COPYING
#
# The pdf-to-text function extracts text from the given child,
# converts it from pdf to text, and creates a text doc with the
# text.

import subprocess

class pdf_pane(edlib.Pane):
    def __init__(self, focus, content, delayed):
        edlib.Pane.__init__(self, focus)
        self.doc = focus
        self.pipe = None
        self.add_notify(focus, "Close")
        self.content = content
        self.have_converting = True
        focus.call("doc:replace", 1, "(Converting content to text...)\n")
        if delayed:
            self.call("doc:request:convert-now")
        else:
            self.handle_visible("key", focus)

    def handle_visible(self, key, focus, **a):
        "handle:convert-now"

        p = subprocess.Popen(["/usr/bin/pdftotext", "-layout", "-", "-"], close_fds=True,
                             stdout=subprocess.PIPE, stderr = subprocess.PIPE,
                             stdin =subprocess.PIPE)

        self.pipe = p
        # FIXME this could block if pipe fills
        os.write(p.stdin.fileno(), self.content)
        p.stdin.close()
        p.stdin = None
        fd = p.stdout.fileno()
        fcntl.fcntl(fd, fcntl.F_SETFL,
                    fcntl.fcntl(fd, fcntl.F_GETFL) | os.O_NONBLOCK)
        self.call("event:read", fd, self.read)

    def handle_close(self, key, **a):
        "handle:Close"

        if self.pipe:
            self.pipe.kill()
            self.pipe.communicate()
        return 1

    def handle_doc_close(self, key, focus, **a):
        "handle:Notify:Close"
        if focus == self.doc:
            self.doc = None
            if self.pipe:
                self.pipe.kill()
        return 1

    def read(self, key, **a):
        if not self.pipe:
            return edlib.Efalse
        try:
            r = os.read(self.pipe.stdout.fileno(), 65536)
        except IOError:
            return 1

        if not self.doc:
            return edlib.Efalse

        if r:
            if self.have_converting:
                m = edlib.Mark(self.doc)
                m2 = m.dup()
                m.step(1)
                self.have_converting = False
            else:
                m = edlib.Mark(self.doc)
                m2 = m
            self.doc.call("doc:set-ref", m2)
            self.doc.call("doc:replace", 1, r.decode('utf-8','ignore'),
                          m, m2)
            return 1
        # EOF
        if not self.pipe:
            return edlib.Efalse
        out, err = self.pipe.communicate()
        self.pipe = None
        if err:
            edlib.LOG("pdf-to-text", err.decode('utf-8','ignore'))
        else:
            self.mark_urls(self.doc)

        self.close()
        return edlib.Efalse

    def mark_urls(self, doc):
        ms = edlib.Mark(doc)
        me = ms.dup()
        doc.call("doc:set-ref", me)
        while ms < me:
            try:
                len = doc.call("text-search",
                                "(http|https|ftp|mail):[^][\\s\";<>]+", ms, me)
                len -= 1
            except:
                return
            # People sometimes put a period or ')' at the end of a URL.
            while doc.prior(ms) in '.)':
                doc.prev(ms)
                len -= 1
            m1 = ms.dup()
            i = 0
            while i < len:
                doc.prev(m1)
                i += 1
            url = doc.call("doc:get-str", m1, ms, ret='str')
            tag = doc['next-url-tag']
            if not tag:
                tag = "1"
            doc.call("doc:set-attr", 1, m1, "render:url", "%d:%s"%(len,tag))
            doc['next-url-tag'] = "%d" % (int(tag) + 1)
            doc["url:" + tag] = url

def pdf_to_text(key, home, focus, num, comm2, **a):
    pdf = focus.call("doc:get-bytes", ret='bytes')

    doc = focus.call("doc:from-text", "pdf-document", "", ret='pane')
    pdf_pane(doc, pdf, num)
    comm2("cb", doc)
    return 1

if "editor" in globals():
    editor.call("global-set-command", "pdf-to-text", pdf_to_text)
