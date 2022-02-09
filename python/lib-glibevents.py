# -*- coding: utf-8 -*-
# Copyright Neil Brown ©2015-2021 <neil@brown.name>
# May be distributed under terms of GPLv2 - see file:COPYING

# edlib module for getting events from GLib

import signal
import gi
import os

gi.require_version('Gtk', '3.0')
from gi.repository import GLib, Gtk

class events(edlib.Pane):
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus)
        self.active = True
        self.events = {}
        self.sigs = {}
        self.poll_list = []
        self.ev_num = 0
        self.dont_block = False
        self.maxloops = 10
        if 'EDLIB_TESTING' in os.environ:
            self.maxloops = -1

    def handle_close(self, key, focus, **a):
        "handle:Notify:Close"
        self.free("free", focus, None)
        return 1

    def add_ev(self, focus, comm, event, num):
        self.add_notify(focus, "Notify:Close")
        ev = self.ev_num
        self.events[ev] = [focus, comm, event, num]
        self.ev_num += 1
        return ev

    def read(self, key, focus, comm2, num, **a):
        ev = self.add_ev(focus, comm2, 'event:read', num)
        gev = GLib.io_add_watch(num, GLib.IO_IN | GLib.IO_HUP,
                                self.doreadwrite, comm2, focus, num, ev)
        self.events[ev].append(gev)
        return 1

    def doreadwrite(self, evfd, condition, comm2, focus, fd, ev):
        if ev not in self.events:
            return False
        edlib.time_start(edlib.TIME_READ)
        try:
            rv = comm2("callback", focus, fd)
            ret = rv >= 0
        except edlib.commandfailed:
            ret = False
        edlib.time_stop(edlib.TIME_READ)
        if not ret and ev in self.events:
            del self.events[ev]
        return ret

    def write(self, key, focus, comm2, num, **a):
        ev = self.add_ev(focus, comm2, 'event:write', num)
        gev = GLib.io_add_watch(num, GLib.IO_OUT,
                                self.doreadwrite, comm2, focus, num, ev)
        self.events[ev].append(gev)
        return 1

    def signal(self, key, focus, comm2, num, **a):
        ev = self.add_ev(focus, comm2, 'event:signal', num)
        self.sigs[num] = (focus, comm2, ev)
        signal.signal(num, self.sighan)
        return 1

    def sighan(self, sig, frame):
        (focus, comm2, ev) = self.sigs[sig]
        GLib.idle_add(self.dosig, comm2, focus, sig, ev)
        return 1

    def dosig(self, comm, focus, sig, ev):
        if ev not in self.events:
            return False
        edlib.time_start(edlib.TIME_SIG)
        try:
            rv = comm("callback", focus, sig)
            ret = rv >= 0
        except edlib.commandfailed:
            ret = False
        edlib.time_stop(edlib.TIME_SIG)
        if not ret:
            del self.events[ev]
            signal.signal(sig, signal.SIG_DFL)
        return False

    def timer(self, key, focus, comm2, num, **a):
        ev = self.add_ev(focus, comm2, 'event:timer', num)
        gev = GLib.timeout_add(num, self.dotimeout, comm2, focus, ev)
        self.events[ev].append(gev)
        return 1

    def poll(self, key, focus, comm2, **a):
        ev = self.add_ev(focus, comm2, 'event:poll', -2)
        self.poll_list.append(ev)

    def dotimeout(self, comm2, focus, ev):
        if ev not in self.events:
            return False
        edlib.time_start(edlib.TIME_TIMER)
        try:
            rv = comm2("callback", focus)
            ret = rv >= 0
        except edlib.commandfailed:
            ret = False
        edlib.time_stop(edlib.TIME_TIMER)
        if not ret:
            del self.events[ev]
        return ret

    def nonblock(self, key, **a):
        self.dont_block = True

    def run(self, key, **a):
        if self.active:
            dont_block = self.dont_block
            self.dont_block = False
            for s in self.poll_list:
                f,c,e,n = self.events[s]
                if c("callback:poll", f, -1) > 0:
                    dont_block = True
            if not dont_block:
                # Disable any alarm set by python (or other interpreter)
                signal.alarm(0)
                Gtk.main_iteration_do(True)
            events = 0
            while self.active and events != self.maxloops and Gtk.events_pending():
                signal.alarm(0)
                Gtk.main_iteration_do(False)
                events += 1
        if self.active:
            return 1
        else:
            return edlib.Efalse

    def deactivate(self, key, **a):
        self.active = False
        global ev
        ev = None
        return 1

    def is_active(self, key, **a):
        if self.active:
            return 1
        return None

    def free(self, key, focus, comm2, **a):
        try_again = True
        while try_again:
            try_again = False
            for source in self.events:
                e = self.events[source]
                if e[0] != focus:
                    continue
                if comm2 and e[1] != comm2:
                    continue
                del self.events[source]
                if len(e) == 5:
                    try:
                        GLib.source_remove(e[4])
                    except:
                        # must be already gone
                        pass
                if source in self.poll_list:
                    self.poll_list.remove(source)
                try_again = True
                break

        return 1
    def refresh(self, key, focus, **a):
        # all active events are re-enabled.  This will presumably send them
        # to the new primary event handler
        k = self.events.keys()
        for e in k:
            (focus, comm, event, num) = self.events[e][:4]
            if event != "event:signal" and len(self.events[e]) == 5:
                try:
                    GLib.source_remove(self.events[4])
                except:
                    pass
            del self.events[e]
            focus.call(event, num, comm)
        # allow other event handlers to do likewise
        return edlib.Efallthrough

ev = None
def events_activate(focus):
    global ev
    if ev:
        return 1
    ev = events(focus)
    focus.call("global-set-command", "event:read-python", ev.read)
    focus.call("global-set-command", "event:write-python", ev.write)
    focus.call("global-set-command", "event:signal-python", ev.signal)
    focus.call("global-set-command", "event:timer-python", ev.timer)
    focus.call("global-set-command", "event:poll-python", ev.poll)
    focus.call("global-set-command", "event:run-python", ev.run)
    focus.call("global-set-command", "event:deactivate-python", ev.deactivate)
    focus.call("global-set-command", "event:free-python", ev.free)
    focus.call("global-set-command", "event:refresh-python", ev.refresh)
    focus.call("global-set-command", "event:noblock-python", ev.nonblock)
    focus.call("global-set-command", "glibevents:active", ev.is_active)
    focus.call("event:refresh")
    return 1

def register_events(key, focus, comm2, **a):
    if focus.call("glibevents:active"):
        # already active
        return 1
    events_activate(focus)
    return 1

editor.call("global-set-command", "attach-glibevents", register_events)
