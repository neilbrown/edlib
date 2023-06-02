# -*- coding: utf-8 -*-
# Copyright Neil Brown (c)2020-2023 <neil@brown.name>
# May be distributed under terms of GPLv2 - see file:COPYING
#
# This module supports keyboard-macros, both recording and replaying.
# To start recording, we request Keystroke-notify from the display and record
# keystrokes until asked to stop, and which point we discard the last couple
# as advised.  The set of keystrokes, as a comma-separated string, is added
# to *Macro History*
# Individual macros in this history can be named with attributes at the start
# of the line.  A macro can then be accessed by index from end, or by name.
# A macro can be replayed by sending keystrokes until one returns an error.
#
# While capturing keystrokes we register a pane to receive the notifications.
# This is a child of the root.
# Other functionality is available simply through global commands.
#

import edlib

docname = "*Macro History*"

# A macro is a list of keystroke separated by commas
# A keystroke can contain a comma, but only preceeded by a '-'.

def macro_join(l):
    return ','.join(l)

def macro_split(mac):
    l = []
    st = 0
    while mac:
        c = mac.find(',', st)
        if c < 0:
            l.append(mac)
            mac = ""
        elif c == 0:
            mac = mac[1:]
        elif mac[c-1] == '-':
            st = c+1
        else:
            l.append(mac[:c])
            mac = mac[c+1:]
            st = 0
    return l

class CapturePane(edlib.Pane):
    def __init__(self, focus):
        root = focus
        while root.parent != root:
            root = root.parent
        edlib.Pane.__init__(self, root)
        focus.call("window:request:Keystroke-notify", self)
        focus.call("window:request:macro:capture-active", self)
        focus.call("window:request:macro:capture-done", self)
        focus.call("Mode:set-mode", str2 = "()")
        self.line = []

    def capture_active(self, key, focus, **a):
        "handle:macro:capture-active"
        return 1

    def capture_key(self, key, focus, str, **a):
        "handle:Keystroke-notify"
        self.line.append(str)
        return 1

    def capture_done(self, key, focus, num, **a):
        "handle:macro:capture-done"
        ret = 0
        if num > 0:
            self.line[-num:] = []
        if self.line:
            ret = self.call("history:add", docname, macro_join(self.line));
            if ret == 0:
                self.call("global-load-module", "lib-history")
                ret = self.call("history:add", docname, macro_join(self.line))
            if ret == 0:
                ret = edlib.Efalse
        focus.call("Mode:set-mode", str2 = "")
        self.close()
        return ret

def start_capture(key, focus, **a):
    if focus.call("window:notify:macro:capture-active") >= 1:
        # capture currently active
        return edlib.Efalse
    CapturePane(focus)
    if focus.call("window:notify:macro:capture-active") >= 1:
        # Good, it is active now
        return 1
    return edlib.Efail

def end_capture(key, focus, num, **a):
    try:
        if focus.call("window:notify:macro:capture-active") <= 0:
            # capture currently active
            return edlib.Efalse
    except edlib.commandfailed:
        return edlib.Efalse
    ret = focus.call("window:notify:macro:capture-done", num)
    if ret == 0:
        return edlib.Efalse
    if ret > 0:
        return 1
    return ret

def play_macro(key, focus, num, str, **a):
    if str:
        mac = focus.call("history:get-last", docname, str, ret='str')
    else:
        mac = focus.call("history:get-last", docname, num, ret='str')
    if not mac:
        return edlib.Efail
    while focus.parent != focus.parent.parent:
        focus = focus.parent
    keys = macro_split(mac)
    error = False
    i = 0
    while not error and i < len(keys):
        try:
            if focus.call("Keystroke", keys[i]) < 0:
                error = True
            else:
                i += 1
        except edlib.commandfailed:
            error = True
    if error:
        return edlib.Efail
    return 1

def name_macro(key, focus, num, str, **a):
    # Give name 'str' to macro numbered 'num'
    if num <= 0:
        num = 1
    focus.call("history:get-last", num, docname, str)

edlib.editor.call("global-set-command", "macro:capture", start_capture)
edlib.editor.call("global-set-command", "macro:finished", end_capture)
edlib.editor.call("global-set-command", "macro:replay", play_macro)
edlib.editor.call("global-set-command", "macro:name", name_macro)
