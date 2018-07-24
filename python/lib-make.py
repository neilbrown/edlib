# -*- coding: utf-8 -*-
# Copyright Neil Brown ©2016 <neil@brown.name>
# May be distributed under terms of GPLv2 - see file:COPYING
#

import subprocess, os, fcntl

class MakePane(edlib.Pane):
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus)

    def run(self, cmd, cwd):
        FNULL = open(os.devnull, 'r')
        self.call("doc:replace", "Cmd: %s\nCwd: %s\n\n" % (cmd,cwd))
        self.pipe = subprocess.Popen(cmd, shell=True, close_fds=True,
                                     cwd=cwd,
                                     stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT,
                                     stdin = FNULL)
        FNULL.close()
        if not self.pipe:
            return False
        fd = self.pipe.stdout.fileno()
        fl = fcntl.fcntl(fd, fcntl.F_GETFL)
        fcntl.fcntl(fd, fcntl.F_SETFL, fl | os.O_NONBLOCK)
        self.call("event:read", fd, self.read)
        return True

    def read(self, key, **a):
        if not self.pipe:
            return -1
        try:
            r = os.read(self.pipe.stdout.fileno(), 1024)
        except IOError:
            return 1
        if r is None or len(r) == 0:
            self.pipe.communicate()
            self.pipe = None
            self.call("doc:replace", "\nProcess Finished\n");
            return -1
        self.call("doc:replace", r);
        return 1

    def handle_close(self, key, **a):
        "handle:Close"
        if self.pipe is not None:
            p = self.pipe
            self.pipe = None
            p.terminate()
            try:
                p.communicate()
            except IOError:
                pass
        return 1

    def handle_abort(self, key, **a):
        "handle:Abort"
        if self.pipe is not None:
            self.pipe.terminate()
            self.pipe.communicate()
            self.pipe = None
            self.call("doc:replace", "\nProcess Aborted\n");
        return 1

def make_attach(key, focus, comm2, str, str2, **a):
    m = edlib.Mark(focus)
    focus.call("doc:replace", m)
    p = MakePane(focus)
    if not p:
        return -1;
    if not p.run(str, str2):
        p.close()
        return -1;
    if comm2:
        comm2("callback", p)
    return 1

class makeprompt(edlib.Pane):
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus)

    def enter(self, key, focus, **a):
        "handle:Enter"
        str = focus.call("doc:get-str", ret="str")
        return focus.call("popup:close", str)

def make_request(key, focus, str, **a):
    if key[-4:] == "grep":
        dflt = "grep -nH "
        cmd = "grep"
        docname = "*grep output*"
    else:
        dflt = "make -k"
        cmd = "make"
        docname = "*Compile Output*"

    if str is not None:
        # pop-up has completed
        print "Run", cmd, "as:", str
        doc = focus.call("doc:byname", docname, ret='focus')
        if not doc:
            doc = focus.call("doc:from-text", docname, "", ret='focus')
        if not doc:
            return -1
        path = focus["dirname"]
        doc['dirname'] = path
        p = focus.call("OtherPane", doc, 4, ret='focus')
        if not p:
            return -1
        focus.call("global-set-attr", "make-target-doc", docname)
        d = p.call("doc:attach", ret='focus')
        d.call("doc:assign", doc, "", 1)
        p = d.call("attach-makecmd", str, path, ret='focus')
        p = p.call("attach-viewer", ret='focus')
        return 1
    # Create a popup to ask for make command
    p = focus.call("PopupTile", "D2", dflt, ret="focus")
    if not p:
        return 0
    p["prompt"] = "%s Command" % cmd
    p["done-key"] = key
    p.call("doc:set-name", "%s Command" % cmd)
    makeprompt(p)
    return 1

editor.call("global-set-command", "attach-makecmd", make_attach)
editor.call("global-set-command", "interactive-cmd-make", make_request)
editor.call("global-set-command", "interactive-cmd-grep", make_request)
