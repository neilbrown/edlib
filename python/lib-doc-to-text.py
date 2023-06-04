# -*- coding: utf-8 -*-
# Copyright Neil Brown ©2023 <neil@brown.name>
# May be distributed under terms of GPLv2 - see file:COPYING
#
# The doc-to-text function extracts bytes from the given child,
# converts it to text using lowriter.
# Unfortunately lowriter only reads and writes a file, not a pipe..

import edlib

import subprocess
import tempfile
import os, fcntl

class doc_pane(edlib.Pane):
    def __init__(self, focus, path, newpath, delayed):
        edlib.Pane.__init__(self, focus)
        self.doc = focus
        self.path = path
        self.add_notify(focus, "Close")
        self.newpath = newpath
        focus.call("doc:replace", 1, "(Converting content to text...)\n")
        if delayed:
            self.call("doc:request:convert-now")
        else:
            self.handle_visible("key", focus)

    def handle_visible(self, key, focus, **a):
        "handle:convert-now"
        pipe = subprocess.Popen(["/usr/bin/lowriter", "--convert-to", "txt:Text",
                                 self.path], close_fds=True,
                         cwd = os.path.dirname(self.path),
                         stdout=subprocess.PIPE, stderr = subprocess.PIPE,
                         stdin =subprocess.DEVNULL)
        fd = pipe.stdout.fileno()
        self.pipe = pipe
        fcntl.fcntl(fd, fcntl.F_SETFL,
                    fcntl.fcntl(fd, fcntl.F_GETFL) | os.O_NONBLOCK)
        self.call("event:read", pipe.stdout.fileno(), self.read)

    def handle_close(self, key, **a):
        "handle:Close"
        try:
            os.unlink(self.path)
        except FileNotFoundError:
            pass
        try:
            os.unlink(self.newpath)
        except FileNotFoundError:
            pass
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
        if r:
            # Not interesting in any output, just in EOF
            return 1

        out, err = self.pipe.communicate()
        self.pipe = None
        if err:
            edlib.LOG("doc-to-text:", err.decode("utf-8","ignore"))
        try:
            with open(self.newpath, 'rb') as fp:
                out = fp.read()
        except:
            out = (b"DOC coversions failed:\n" + err)

        if self.doc:
            m = edlib.Mark(self.doc)
            m2 = m.dup()
            m.step(1)
            self.doc.call("doc:set-ref", m2)
            self.doc.call("doc:replace", 1, out.decode("utf-8", 'ignore'),
                          m, m2)
            self.doc.call("url:mark-up")
        self.close()
        return edlib.Efalse

def doc_to_text(key, home, focus, num, str1, comm2, **a):

    if not str1 or '.' not in str1:
        # we need a file name to get an extension
        return edlib.Enoarg;

    content = focus.call("doc:get-bytes", ret='bytes')

    ext = str1[str1.rindex('.'):]
    fd, path = tempfile.mkstemp(ext, "edlib-office-temp")
    os.write(fd, content)
    os.close(fd)
    newpath = path[:path.rindex('.')] + ".txt"

    doc = focus.call("doc:from-text", "office-document", "", ret='pane')

    doc_pane(doc, path, newpath, num)

    comm2("cb", doc)
    return 1

edlib.editor.call("global-set-command", "doc-to-text", doc_to_text)
