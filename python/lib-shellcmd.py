# -*- coding: utf-8 -*-
# Copyright Neil Brown ©2016-2023 <neil@brown.name>
# May be distributed under terms of GPLv2 - see file:COPYING
#

import edlib

import subprocess, os, fcntl, signal

class ShellPane(edlib.Pane):
    def __init__(self, focus, reusable, add_footer=True):
        edlib.Pane.__init__(self, focus)
        self.callback = None
        self.cb_pane = None
        self.cb_lines = 0
        self.line = b''
        self.lines = 0
        self.pipe = None
        self.call("doc:request:Abort")
        self.add_footer = add_footer
        if reusable:
            self.call("editor:request:shell-reuse")

    def check_reuse(self, key, focus, comm2, **a):
        "handle:shell-reuse"
        if self.pipe:
            return 0
        if comm2:
            comm2("cb", focus, self["doc-name"])
        self.call("doc:destroy")
        return 1

    def run(self, key, focus, num, num2, str, str2, **a):
        "handle:shell-run"
        cmd = str
        cwd = str2
        header = num != 0
        send_stdin = num2 != 0
        if not cwd:
            cwd=self['dirname']
        if not cwd:
            cwd = '/'
        while cwd and cwd != '/' and cwd[-1] == '/':
            # don't want a trailing slash
            cwd = cwd[:-1]

        if send_stdin:
            input = focus.call("doc:get-str", ret='str')
            focus.call("doc:clear")
            stdin = subprocess.PIPE
        else:
            stdin = subprocess.DEVNULL
        if not os.path.isdir(cwd):
            self.call("doc:replace",
                       "Directory \"%s\" doesn't exist: cannot run shell command\n"
                       % cwd)
            return edlib.Efail
        if header:
            self.call("doc:replace", "Cmd: %s\nCwd: %s\n\n" % (cmd,cwd))
        env = os.environ.copy()
        env['PWD'] = cwd
        askp = self.call("xdg-find-edlib-file",
                         "el-askpass", "bin",  ret='str')
        if askp:
            env['SSH_ASKPASS'] = askp
            env['SSH_ASKPASS_REQUIRE'] = 'force'

        try:
            self.pipe = subprocess.Popen(cmd, shell=True, close_fds=True,
                                         cwd=cwd, env=env,
                                         start_new_session=True,
                                         stdout=subprocess.PIPE,
                                         stderr=subprocess.STDOUT,
                                         stdin =stdin)
        except:
            self.pipe = None
        if not self.pipe:
            return edlib.Efail
        if send_stdin:
            fd = self.pipe.stdin.fileno()
            fl = fcntl.fcntl(fd, fcntl.F_GETFL);
            fcntl.fcntl(fd, fcntl.F_SETFL, fl | os.O_NONBLOCK)
            self.call("event:write", fd, self.write)
            self.input = input
        self.call("doc:set:doc-status", "Running")
        self.call("doc:notify:doc:status-changed")
        fd = self.pipe.stdout.fileno()
        if not edlib.testing:
            fl = fcntl.fcntl(fd, fcntl.F_GETFL)
            fcntl.fcntl(fd, fcntl.F_SETFL, fl | os.O_NONBLOCK)
            self.call("event:read", fd, self.read)
        else:
            # when testing, use blocking IO for predictable results,
            # and schedule them 'soon' so they cannot happen 'immediately'
            # and happen with gaps
            self.call("event:timer", 50, self.read)
        return True

    def read(self, key, **a):
        if not self.pipe:
            return edlib.Efalse
        try:
            if not edlib.testing:
                r = os.read(self.pipe.stdout.fileno(), 4096)
            else:
                r = b''
                b = os.read(self.pipe.stdout.fileno(), 4096)
                while b:
                    r += b
                    b = os.read(self.pipe.stdout.fileno(), 4096)
        except IOError:
            return 1
        if r is None or len(r) == 0:
            (out,err) = self.pipe.communicate()
            ret = self.pipe.poll()
            self.pipe = None
            l = self.line + out
            self.line = b''
            self.call("doc:replace", l.decode("utf-8", 'ignore'))
            if self.cb_pane:
                p = self.cb_pane
                self.cb_pane = None
                self.callback("cb:eof", p, ret, self)
            if not self.add_footer:
                pass
            elif not ret:
                self.call("doc:replace", "\nProcess Finished\n")
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
            if self.cb_pane and self.cb_lines > 0:
                # need to send callback after this many lines
                self.lines += len(l.splitlines())
                if self.lines >= self.cb_lines:
                    p = self.cb_pane
                    self.cb_pane = None
                    self.cb_lines = 0
                    if self.callback("cb:lines", p, self) > 1:
                        # it still wants EOF
                        self.cb_pane = p
            self.call("doc:replace", l[:i+1].decode("utf-8", 'ignore'))
            l = l[i+1:]
        self.line = l
        return 1

    def write(self, key, **a):
        if not self.pipe or not self.pipe.stdin:
            return edlib.Efalse
        if not self.input:
            self.pipe.stdin.close()
            self.pipe.stdin = None
            return edlib.Efalse
        b = self.input[:1024]
        self.input = self.input[1024:]
        os.write(self.pipe.stdin.fileno(), b.encode())
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
            self.call("doc:replace", "\nProcess signalled\n")
        return 1

    def handle_callback(self, key, focus, num, num2, comm2, **a):
        "handle:shellcmd:set-callback"
        # comm2 is recorded as a callback to call on focus after
        # num msecs or num2 lines of output.  Callback is called
        # when command finishes if not before
        if not focus or not comm2:
            return edlib.Einval
        self.callback = comm2
        self.cb_pane = focus
        self.add_notify(focus, "Notify:Close")
        self.cb_lines = num2
        if num > 0:
            self.call("event:timer", num, self.time_cb)
        return 1

    def time_cb(self, key, **a):
        if self.cb_pane:
            p = self.cb_pane
            self.cb_pane = None
            if self.callback("cb:timer", p, self) > 1:
                # still want moer
                self.cb_pane = p
        return edlib.Efalse

    def handle_nofify_close(self, key, focus, **a):
        "handle:Notify:Close"
        if focus == self.cb_pane:
            self.cb_pane = None

class ShellViewer(edlib.Pane):
    # This is a simple overlay to follow EOF
    # when point is at EOF.
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus)
        self.call("doc:request:doc:replaced")

    def handle_replace(self, key, mark, mark2, **a):
        "handle:doc:replaced"
        if not mark or not mark2:
            return 1
        p = self.call("doc:point", ret='mark')
        if p and p == mark:
            # point is where we inserted text, so move it to
            # after the insertion point
            p.to_mark(mark2)
        return 1

    def handle_clone(self, key, focus, home, **a):
        "handle:Clone"
        p = ShellViewer(focus)
        home.clone_children(p)
        return 1

def shell_attach(key, focus, comm2, num, str, str2, **a):
    # num: 1 - place Cmd/Cwd at top of doc
    #      2 - reuse doc, don't clear it first
    #      4 - register to be cleaned up by shell-reuse
    #      8 - don't add a footer when command completes.
    #     16 - use content of doc as stdin
    # Clear document - discarding undo history.
    if (num & 2) == 0:
        focus.call("doc:clear")
    p = ShellPane(focus, num & 4, (num & 8) == 0)
    if not p:
        return edlib.Efail
    focus['view-default'] = 'shell-viewer'
    try:
        p.call("shell-run", num&1, num & 16, str, str2)
    except edlib.commandfailed:
        p.close()
        return edlib.Efail
    if comm2:
        comm2("callback", p)
    return 1

def shell_view_attach(key, focus, comm2, **a):
    p = focus.call("attach-viewer", ret='pane')
    p = ShellViewer(p)

    if not p:
        return edlib.Efail
    if comm2:
        comm2("callback", p)
    return 1

edlib.editor.call("global-set-command", "attach-shellcmd", shell_attach)
edlib.editor.call("global-set-command", "attach-shell-viewer", shell_view_attach)
