# -*- coding: utf-8 -*-
# Copyright Neil Brown ©2016-2020 <neil@brown.name>
# May be distributed under terms of GPLv2 - see file:COPYING
#

import subprocess, os, fcntl, signal

class ShellPane(edlib.Pane):
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus)
        self.line = b''
        self.call("doc:request:Abort")
        self.call("editor:request:shell-reuse")

    def check_reuse(self, key, comm2, **a):
        "handle:shell-reuse"
        if self.pipe:
            return 0
        if comm2:
            comm2("cb", self["doc-name"])
        self.call("doc:destroy")
        return 1

    def run(self, cmd, cwd, header=True):
        FNULL = open(os.devnull, 'r')
        if not cwd:
            cwd=self['dirname']
        if not cwd:
            cwd = '/'
        if header:
            self.call("doc:replace", "Cmd: %s\nCwd: %s\n\n" % (cmd,cwd))
        env = os.environ.copy()
        env['PWD'] = cwd
        try:
            self.pipe = subprocess.Popen(cmd, shell=True, close_fds=True,
                                         cwd=cwd, env=env,
                                         start_new_session=True,
                                         stdout=subprocess.PIPE,
                                         stderr=subprocess.STDOUT,
                                         stdin = FNULL)
        except:
            self.pipe = None
        FNULL.close()
        if not self.pipe:
            return False
        self.call("doc:set:doc-status", "Running");
        self.call("doc:notify:doc:status-changed")
        fd = self.pipe.stdout.fileno()
        fl = fcntl.fcntl(fd, fcntl.F_GETFL)
        fcntl.fcntl(fd, fcntl.F_SETFL, fl | os.O_NONBLOCK)
        self.call("event:read", fd, self.read)
        return True

    def read(self, key, **a):
        if not self.pipe:
            return edlib.Efalse
        try:
            r = os.read(self.pipe.stdout.fileno(), 1024)
        except IOError:
            return 1
        if r is None or len(r) == 0:
            (out,err) = self.pipe.communicate()
            ret = self.pipe.poll()
            self.pipe = None
            l = self.line + out
            self.line = b''
            self.call("doc:replace", l.decode("utf-8"))
            if not ret:
                self.call("doc:replace", "\nProcess Finished\n");
            elif ret > 0:
                self.call("doc:replace", "\nProcess Finished (%d)\n" % ret)
            else:
                self.call("doc:replace", "\nProcess Finished (signaled %d)\n" % -ret)
            self.call("doc:set:doc-status", "Complete")
            self.call("doc:notify:doc:status-changed")
            return edlib.Efalse
        l = self.line + r
        i = l.rfind(b'\n')
        if i >= 0:
            self.call("doc:replace", l[:i+1].decode("utf-8"));
            l = l[i+1:]
        self.line = l
        return 1

    def handle_close(self, key, **a):
        "handle:Close"
        if self.pipe is not None:
            p = self.pipe
            self.pipe = None
            os.killpg(p.pid, signal.SIGTERM)
            p.terminate()
            try:
                p.communicate()
            except IOError:
                pass
        return 1

    def handle_abort(self, key, **a):
        "handle:Abort"

        if self.pipe is not None:
            os.killpg(self.pipe.pid, signal.SIGTERM)
            self.call("doc:replace", "\nProcess signalled\n");
        return 1

def shell_attach(key, focus, comm2, num, str, str2, **a):
    # Clear document - discarding undo history.
    if num == 0:
        focus.call("doc:clear")
    p = ShellPane(focus)
    if not p:
        return edlib.Efail;
    if not p.run(str, str2, False):
        p.close()
        return edlib.Efail;
    if comm2:
        comm2("callback", p)
    return 1

editor.call("global-set-command", "attach-shellcmd", shell_attach)
