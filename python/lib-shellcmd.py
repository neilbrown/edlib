# -*- coding: utf-8 -*-
# Copyright Neil Brown ©2016 <neil@brown.name>
# May be distributed under terms of GPLv2 - see file:COPYING
#

import subprocess, os, fcntl

class ShellPane(edlib.Pane):
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus, self.handle)

    def run(self, cmd, cwd):
        FNULL = open(os.devnull, 'r')
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
            self.call("Replace", "\nProcess Finished\n");
            return -1
        self.call("Replace", r);
        return 1

    def handle(self, key, **a):
        if key == "Close":
            if self.pipe is not None:
                p = self.pipe
                self.pipe = None
                p.terminate()
                try:
                    p.communicate()
                except IOError:
                    pass
            self.call("event:free")
            return 1
        if key == "Abort":
            if self.pipe is not None:
                self.pipe.terminate()
                self.pipe.communicate()
                self.pipe = None
                self.call("Replace", "\nProcess Aborted\n");
            return 1

def shell_attach(key, focus, comm2, str, str2, **a):
    m = edlib.Mark(focus)
    focus.call("Move-File", 1)
    focus.call("Replace", m)
    p = ShellPane(focus)
    if not p:
        return -1;
    if not p.run(str, str2):
        p.close()
        return -1;
    comm2("callback", p)
    return 1

editor.call("global-set-command", "attach-shellcmd", shell_attach)
