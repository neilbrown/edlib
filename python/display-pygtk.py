# -*- coding: utf-8 -*-
# Copyright Neil Brown ©2015-2019 <neil@brown.name>
# May be distributed under terms of GPLv2 - see file:COPYING

# edlib display module using pygtk to create a window, handle drawing, and
# receive mouse/keyboard events.
# provides eventloop function using gtk.main.

import sys
import os
import signal
import pygtk
import gtk
import pango
import thread
import gobject
import glib

class EdDisplay(gtk.Window):
    def __init__(self, focus):
        events_activate(focus)
        gtk.Window.__init__(self)
        self.pane = edlib.Pane(focus, self)
        # panes is a mapping from edlib.Pane objects to gtk.gdk.Pixmap objects.
        # While a pane has the same size as its parent, only the parent can have
        # a Pixmap
        self.panes = {}
        self.set_title("EDLIB")
        self.connect('destroy', self.close_win)
        self.create_ui()
        self.need_refresh = True
        self.pane["scale:M"] = "%dx%d" % (self.charwidth, self.lineheight)
        self.pane.w = self.charwidth * 80
        self.pane.h = self.lineheight * 24
        self.pane.call("Request:Notify:global-displays")
        self.primary_cb = gtk.Clipboard(selection="PRIMARY")
        self.clipboard_cb = gtk.Clipboard(selection="CLIPBOARD")
        self.targets = [ (gtk.gdk.SELECTION_TYPE_STRING, 0, 0) ]
        self.have_primary = False
        self.have_clipboard = False
        self.show()

    def claim_primary(self):
        if self.have_primary:
            return
        self.primary_cb.set_with_data(self.targets,
                                     self.request_clip,
                                     self.lost_clip, "PRIMARY")
        self.have_primary = True

    def claim_both(self):
        self.claim_primary()
        if self.have_clipboard:
            return
        self.clipboard_cb.set_with_data(self.targets,
                                        self.request_clip,
                                        self.lost_clip, "CLIPBOARD")
        self.have_clipboard = True

    def request_clip(self, sel, seldata, info, data):
        s = self.pane.call("copy:get", 0, ret='str')
        if not s:
            s = ""
        seldata.set_text(s)

    def lost_clip(self, cb, data):
        if data == "PRIMARY":
            self.have_primary = False
        if data == "CLIPBOARD":
            self.have_clipboard = False

    def copy_save(self, key, focus, num2, **a):
        "handle:copy:save"
        if num2:
            # mouse-only
            self.claim_primary()
        else:
            self.claim_both()
        return 0

    def copy_get(self, key, focus, num, comm2, **a):
        "handle:copy:get"
        if not self.have_primary:
            if num == 0:
                s = self.primary_cb.wait_for_text()
                if s is not None:
                    if comm2:
                        comm2("cb", focus, s)
                    return 1
            else:
                if self.primary_cb.wait_is_text_available():
                    num -= 1
        if not self.have_clipboard:
            if num == 0:
                s = self.clipboard_cb.wait_for_text()
                if s is not None:
                    if comm2:
                        comm2("cb", focus, s)
                    return 1
            else:
                if self.clipboard_cb.wait_is_text_available():
                    num -= 1
        return self.pane.parent.call(key, focus, num, comm2)

    def handle_notify_displays(self, key, focus, comm2, **a):
        "handle:Notify:global-displays"
        comm2("callback:display", self.pane)
        return 0

    def handle_postorder(self, key, num, num2, home, focus, str, str2, comm2, xy, **a):
        "handle:Refresh:postorder"
        self.text.queue_draw()
        return 1

    def handle_close_window(self, key, home, focus, **a):
        "handle:Display:close"
        x = []
        focus.call("Call:Notify:global-displays", lambda key,**a:x.append(1))
        if len(x) > 1:
            self.close_win()
        else:
            focus.call("Message", "Cannot close only window.")
        return 1

    def handle_fullscreen(self, key, num, num2, home, focus, str, str2, comm2, xy, **a):
        "handle:Display:fullscreen"
        if num > 0:
            self.fullscreen()
        else:
            self.unfullscreen()
        return 1

    def handle_new(self, key, num, num2, home, focus, str, str2, comm2, xy, **a):
        "handle:Display:new"
        newdisp = EdDisplay(home.parent)
        home.clone_children(newdisp.pane);
        return 1

    def handle_close(self, key, num, num2, home, focus, str, str2, comm2, xy, **a):
        "handle:Close"
        self.pane.close()
        # FIXME close the window??
        return True

    def handle_clear(self, key, num, num2, home, focus, str, str2, comm2, xy, **a):
        "handle:pane-clear"
        attr = str2
        if attr is None:
            attr = str
        if attr is not None:
            fg, bg = self.get_colours(attr)
        else:
            fg, bg = self.get_colours("bg:white")
        pm = self.get_pixmap(focus)
        self.do_clear(pm, bg)
        self.pane.damaged(edlib.DAMAGED_POSTORDER)
        return True

    def handle_text_size(self, key, num, num2, home, focus, str, str2, comm2, xy, **a):
        "handle:text-size"
        attr=""
        if str2 is not None:
            attr = str2
        if num2 is not None:
            scale = num2 * 10 / self.charwidth
        else:
            scale = 1000
        fd = self.extract_font(attr, scale)
        layout = self.text.create_pango_layout(str)
        layout.set_font_description(fd)
        ctx = layout.get_context()
        metric = ctx.get_metrics(fd)
        ink,(x,y,width,height) = layout.get_pixel_extents()
        ascent = metric.get_ascent() / pango.SCALE
        if num >= 0:
            if width <= num:
                max_bytes = len(str.encode("utf-8"))
            else:
                max_chars,extra = layout.xy_to_index(pango.SCALE*num,
                                                     metric.get_ascent())
                max_bytes = len(str[:max_chars].encode("utf-8"))
        else:
            max_bytes = 0
        return comm2("callback:size", focus, max_bytes, ascent, (width, height))

    def handle_draw_text(self, key, num, num2, home, focus, str, str2, comm2, xy, **a):
        "handle:Draw:text"
        self.pane.damaged(edlib.DAMAGED_POSTORDER)
        if not self.gc or not self.bg:
            fg, bg = self.get_colours("fg:blue,bg:white")
            t = self.text
            if not self.gc:
                self.gc = t.window.new_gc()
                self.gc.set_foreground(fg)
            if not self.bg:
                self.bg = t.window.new_gc()
                self.bg.set_foreground(bg)

        (x,y) = xy
        attr=""
        if str2 is not None:
            attr = str2
        if num2 is not None:
            scale = num2 * 10 / self.charwidth
        else:
            scale = 1000
        fd = self.extract_font(attr, scale)
        layout = self.text.create_pango_layout(str)
        layout.set_font_description(fd)
        ctx = layout.get_context()
        fg, bg = self.get_colours(attr)
        pm = self.get_pixmap(focus)
        metric = ctx.get_metrics(fd)
        ascent = metric.get_ascent() / pango.SCALE
        ink,(lx,ly,width,height) = layout.get_pixel_extents()
        if bg:
            self.bg.set_foreground(bg)
            pm.draw_rectangle(self.bg, True, x+lx, y-ascent+ly, width, height)
        pm.draw_layout(self.gc, x, y-ascent, layout, fg, bg)
        if num >= 0:
            # draw a cursor - outline box if not in-focus,
            # inverse-video if it is.
            cx,cy,cw,ch = layout.index_to_pos(num)
            if cw <= 0:
                cw = metric.get_approximate_char_width()
            cx /= pango.SCALE
            cy /= pango.SCALE
            cw /= pango.SCALE
            ch /= pango.SCALE
            pm.draw_rectangle(self.gc, False, x+cx, y-ascent+cy,
                              cw-1, ch-1);
            in_focus = self.in_focus
            while in_focus and focus.parent and focus.parent.parent and focus.parent != self.pane:
                if focus.parent.focus != focus:
                    in_focus = False
                focus = focus.parent
            if in_focus:
                if fg:
                    self.gc.set_foreground(fg)
                pm.draw_rectangle(self.gc, True, x+cx, y-ascent+cy,
                                  cw, ch);
                if num < len(str):
                    l2 = pango.Layout(ctx)
                    l2.set_font_description(fd)
                    l2.set_text(str[num])
                    fg, bg = self.get_colours(attr+",inverse")
                    pm.draw_layout(self.gc, x+cx, y-ascent+cy, l2, fg, bg)

        return True

    def handle_image(self, key, num, num2, home, focus, str, str2, comm2, xy, **a):
        "handle:Draw:image"
        self.pane.damaged(edlib.DAMAGED_POSTORDER)
        # 'str' is the file name of an image
        # 'num' is '1' if image should be stretched to fill pane
        # if 'num is '0', then 'num2' is 'or' of
        #   0,1,2 for left/middle/right in x direction
        #   0,4,8 for top/middle/bottom in y direction
        # only one of these can be used as image will fill pane in other direction.
        stretch = num
        pos = num2
        w, h = focus.w, focus.h
        x, y = 0, 0
        try:
            pb = gtk.gdk.pixbuf_new_from_file(str)
        except:
            # create a red error image
            pb = gtk.gdk.Pixbuf(gtk.gdk.COLORSPACE_RGB, False, 8, w, h)
            pb.fill(0xff000000)
        if not stretch:
            if pb.get_width() * h > pb.get_height() * w:
                # image is wider than space, reduce height
                h2 = pb.get_height() * w / pb.get_width()
                if pos & 12 == 4:
                    y = (h - h2) / 2
                if pos & 12 == 8:
                    y = h - h2
                h = h2
            else:
                # image is too tall, reduce width
                w2 = pb.get_width() * h / pb.get_height()
                if pos & 3 == 1:
                    x = (w - w2) / 2
                if pos & 3 == 2:
                    x = w - w2
                w = w2
        scale = pb.scale_simple(w, h, gtk.gdk.INTERP_HYPER)
        # I'm not sure I'm completely happy with this, but when
        # not stretching and when z == 0, draw on a parent pane unless
        # a pixmap has already been allocated.  This allows
        # a temp pane to be created to draw an image, then it can
        # be discarded and the image remains
        while focus.z == 0 and not stretch and focus not in self.panes and focus.parent:
            x += focus.x
            y += focus.y
            focus = focus.parent
        pm = self.get_pixmap(focus)
        pm.draw_pixbuf(self.gc, scale, 0, 0, x, y)
        return True

    def handle_notify_close(self, key, num, num2, home, focus, str, str2, comm2, xy, **a):
        "handle:Notify:Close"
        if focus and focus in self.panes:
            del self.panes[focus]
        return True


    styles=["oblique","italic","bold","small-caps"]

    def extract_font(self, attrs, scale):
        "Return a pango.FontDescription"
        family="mono"
        style=""
        size=10
        if scale <= 10:
            scale = 1000
        for word in attrs.split(','):
            if word in self.styles:
                style += " " + word
            elif len(word) and word[0].isdigit():
                try:
                    size = float(word)
                except:
                    size = 10
            elif word == "large":
                size = 14
            elif word == "small":
                size = 9
            elif word[0:7] == "family:":
                family = word[7:]
        fd = pango.FontDescription(family+' '+style+' '+str(size))
        if scale != 1000:
            fd.set_size(fd.get_size() * scale / 1000)
        return fd

    def get_colours(self, attrs):
        "Return a foreground and a background colour - background might be None"
        fg = None
        bg = None
        inv = False
        for word in attrs.split(','):
            if word[0:3] == "fg:":
                fg = word[3:]
            if word[0:3] == "bg:":
                bg = word[3:]
            if word == "inverse":
                inv = True
        cmap = self.text.get_colormap()
        if inv:
            fg,bg = bg,fg
            if fg is None:
                fg = "white"
            if bg is None:
                bg = "black"
        else:
            if fg is None:
                fg = "black"

        if fg:
            try:
                c = gtk.gdk.color_parse(fg)
            except:
                c = gtk.gdk.color_parse("black")
            fgc = cmap.alloc_color(c)
        else:
            fgc = None

        if bg:
            try:
                c = gtk.gdk.color_parse(bg)
            except:
                c = gtk.gdk.color_parse("white")
            bgc = cmap.alloc_color(c)
        else:
            bgc = None

        return fgc, bgc

    def get_pixmap(self, p):
        # find pixmap attached to root-most pane with
        # same size as this, with no x,y,z offset
        while p.parent and p.w == p.parent.w and p.h == p.parent.h and \
              p.x == 0 and p.y == 0 and p.z == 0:
            if p in self.panes:
                del self.panes[p]
            p = p.parent

        if p in self.panes:
            pm = self.panes[p]
            (w,h) = pm.get_size()
            if w == p.w and h == p.h:
                return pm
            del self.panes[p]
        else:
            self.pane.add_notify(p, "Notify:Close")

        self.panes[p] = gtk.gdk.Pixmap(self.window, p.w, p.h)
        return self.panes[p]

    def close_win(self, *a):
        self.pane.close()
        self.destroy()

    def create_ui(self):
        text = gtk.DrawingArea()
        self.text = text
        self.add(text)
        text.show()
        self.fd = pango.FontDescription("mono 10")
        text.modify_font(self.fd)
        ctx = text.get_pango_context()
        metric = ctx.get_metrics(self.fd)
        self.lineheight = (metric.get_ascent() + metric.get_descent()) / pango.SCALE
        self.charwidth = metric.get_approximate_char_width() / pango.SCALE
        self.set_default_size(self.charwidth * 80, self.lineheight * 24)
        self.gc = None
        self.bg = None

        self.im = gtk.IMContextSimple()
        self.in_focus = True
        self.im.set_client_window(self.window)
        self.text.connect("expose-event", self.refresh)
        self.text.connect("focus-in-event", self.focus_in)
        self.text.connect("focus-out-event", self.focus_out)
        self.text.connect("button-press-event", self.press)
        self.text.connect("scroll-event", self.scroll)
        self.text.connect("key-press-event", self.keystroke)
        self.im.connect("commit", self.keyinput)
        self.text.connect("configure-event", self.reconfigure)
        self.text.set_events(gtk.gdk.EXPOSURE_MASK|
                             gtk.gdk.STRUCTURE_MASK|
                             gtk.gdk.BUTTON_PRESS_MASK|
                             gtk.gdk.BUTTON_RELEASE_MASK|
                             gtk.gdk.KEY_PRESS_MASK|
                             gtk.gdk.KEY_RELEASE_MASK);
        self.text.set_property("can-focus", True)

    def refresh(self, *a):
        edlib.time_start(edlib.TIME_WINDOW)
        l = self.panes.keys()
        l.sort(key=lambda pane: pane.abs_z)
        for p in l:
            pm = self.panes[p]
            (rx,ry,rw,rh) = p.abs(0,0,p.w,p.h)
            self.text.window.draw_drawable(self.bg, pm, 0, 0,
                                           rx, ry,
                                           rw, rh)
        edlib.time_stop(edlib.TIME_WINDOW)

    def focus_in(self, *a):
        edlib.time_start(edlib.TIME_WINDOW)
        self.im.focus_in()
        self.in_focus = True
        self.pane.damaged(edlib.DAMAGED_CURSOR)
        self.pane.call("pane:refocus")
        edlib.time_stop(edlib.TIME_WINDOW)

    def focus_out(self, *a):
        edlib.time_start(edlib.TIME_WINDOW)
        self.im.focus_out()
        self.in_focus = False
        self.pane.damaged(edlib.DAMAGED_CURSOR)
        edlib.time_stop(edlib.TIME_WINDOW)

    def reconfigure(self, w, ev):
        edlib.time_start(edlib.TIME_WINDOW)
        alloc = w.get_allocation()
        if self.pane.w == alloc.width and self.pane.h == alloc.height:
            return None
        self.pane.w = alloc.width
        self.pane.h = alloc.height
        self.need_refresh = True
        self.text.queue_draw()
        edlib.time_stop(edlib.TIME_WINDOW)

    def press(self, c, event):
        edlib.time_start(edlib.TIME_KEY)
        c.grab_focus()
        x = int(event.x)
        y = int(event.y)
        s = "Click-" + ("%d"%event.button)
        if event.state & gtk.gdk.SHIFT_MASK:
            s = "S-" + s;
        if event.state & gtk.gdk.CONTROL_MASK:
            s = "C-" + s;
        if event.state & gtk.gdk.MOD1_MASK:
            s = "M-" + s;
        self.pane.call("Mouse-event", s, self.pane, (x,y))
        edlib.time_stop(edlib.TIME_KEY)

    def scroll(self, c, event):
        edlib.time_start(edlib.TIME_KEY)
        c.grab_focus()
        x = int(event.x)
        y = int(event.y)
        if event.direction == gtk.gdk.SCROLL_UP:
            s = "Press-4"
        else:
            s = "Press-5"
        if event.state & gtk.gdk.SHIFT_MASK:
            s = "S-" + s;
        if event.state & gtk.gdk.CONTROL_MASK:
            s = "C-" + s;
        if event.state & gtk.gdk.MOD1_MASK:
            s = "M-" + s;
        self.pane.call("Mouse-event", s, self.pane, (x,y))
        edlib.time_stop(edlib.TIME_KEY)

    eventmap = { "Return" : "Enter",
                 "Tab" : "Tab",
                 "Escape" : "ESC",
                 "Linefeed" : "LF",
                 "Down" : "Down",
                 "Up" : "Up",
                 "Left" : "Left",
                 "Right" : "Right",
                 "Home" : "Home",
                 "End" : "End",
                 "BackSpace" : "Backspace",
                 "Delete" : "Del",
                 "Insert" : "Ins",
                 "Page_Up" : "Prior",
                 "Page_Down" : "Next",
                 "space" : "Chr- ",
                 }

    def keyinput(self, c, strng):
        edlib.time_start(edlib.TIME_KEY)
        self.pane.call("Keystroke", "Chr-" + strng)
        edlib.time_stop(edlib.TIME_KEY)

    def keystroke(self, c, event):
        edlib.time_start(edlib.TIME_KEY)
        if self.im.filter_keypress(event):
            edlib.time_stop(edlib.TIME_KEY)
            return

        kv = gtk.gdk.keyval_name(event.keyval)
        if kv in self.eventmap:
            s = self.eventmap[kv]
            if event.state & gtk.gdk.SHIFT_MASK:
                s = "S-" + s;
            if event.state & gtk.gdk.CONTROL_MASK:
                s = "C-" + s;
        else:
            s = event.string
            if len(s) == 0:
                return
            if ord(s[0]) < 32:
                s = "C-Chr-" + chr(ord(s[0])+64) + "\037C-Chr-" + chr(ord(s[0]) + 96)
            else:
                s = "Chr-" + s
                if event.state & gtk.gdk.CONTROL_MASK:
                    s = "C-" + s;
        if event.state & gtk.gdk.MOD1_MASK:
            s = "M-" + s;
        self.pane.call("Keystroke", self.pane, s)
        edlib.time_stop(edlib.TIME_KEY)

    def do_clear(self, pm, colour):

        t = self.text
        if not self.bg:
            self.bg = t.window.new_gc()
        self.bg.set_foreground(colour)
        (w,h) = pm.get_size()
        pm.draw_rectangle(self.bg, True, 0, 0, w, h)

def new_display(key, focus, comm2, **a):
    if 'SCALE' in os.environ:
        sc = int(os.environ['SCALE'])
        s = gtk.settings_get_default()
        s.set_long_property("gtk-xft-dpi",sc*pango.SCALE, "code")
    disp = EdDisplay(focus)
    comm2('callback', disp.pane)
    return 1

class events(edlib.Pane):
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus)
        self.active = True
        self.events = {}
        self.sigs = {}
        self.ev_num = 0

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
        self.active = True
        ev = self.add_ev(focus, comm2, 'event:read', num)
        gev = gobject.io_add_watch(num, gobject.IO_IN | gobject.IO_HUP,
                                  self.doread, comm2, focus, num, ev)
        self.events[ev].append(gev)
        return 1

    def doread(self, evfd, condition, comm2, focus, fd, ev):
        if ev not in self.events:
            return False
        try:
            edlib.time_start(edlib.TIME_READ)
            comm2("callback", focus, fd)
            edlib.time_stop(edlib.TIME_READ)
            return True
        except edlib.commandfailed:
            del self.events[ev]
            return False

    def signal(self, key, focus, comm2, num, **a):
        ev = self.add_ev(focus, comm2, 'event:signal', num)
        self.sigs[num] = (focus, comm2, ev)
        signal.signal(num, self.sighan)
        return 1

    def sighan(self, sig, frame):
        (focus, comm2, ev) = self.sigs[sig]
        gobject.idle_add(self.dosig, comm2, focus, sig, ev)
        return 1

    def dosig(self, comm, focus, sig, ev):
        if ev not in self.events:
            return False
        try:
            edlib.time_start(edlib.TIME_SIG)
            comm("callback", focus, sig)
            edlib.time_stop(edlib.TIME_SIG)
            return False
        except edlib.commandfailed:
            del self.events[ev]
            signal.signal(sig, signal.SIG_DFL)
            return False

    def timer(self, key, focus, comm2, num, **a):
        self.active = True
        ev = self.add_ev(focus, comm2, 'event:timer', num)
        gev = gobject.timeout_add(num*1000, self.dotimeout, comm2, focus, ev)
        self.events[ev].append(gev)
        return 1

    def dotimeout(self, comm2, focus, ev):
        if ev not in self.events:
            return False
        try:
            edlib.time_start(edlib.TIME_TIMER)
            comm2("callback", focus);
            edlib.time_stop(edlib.TIME_TIMER)
            return True
        except edlib.commandfailed:
            del self.events[ev]
            return False

    def run(self, key, **a):
        if self.active:
            gtk.main_iteration(True)
            while self.active and gtk.events_pending():
                gtk.main_iteration(False)
        if self.active:
            return 1
        else:
            return edlib.Efalse

    def deactivate(self, key, **a):
        self.active = False
        global ev
        ev = None
        return 1

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
                        gobject.source_remove(e[4])
                    except:
                        # must be already gone
                        pass
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
                    gobject.source_remove(self.events[4])
                except:
                    pass
            del self.events[e]
            focus.call(event, num, comm)
        # allow other event handlers to do likewise
        return 0

ev = None
def events_activate(focus):
    global ev
    if ev:
        return 1
    ev = events(focus)
    focus.call("global-set-command", "event:read-python", ev.read)
    focus.call("global-set-command", "event:signal-python", ev.signal)
    focus.call("global-set-command", "event:timer-python", ev.timer)
    focus.call("global-set-command", "event:run-python", ev.run)
    focus.call("global-set-command", "event:deactivate-python", ev.deactivate)
    focus.call("global-set-command", "event:free-python", ev.free)
    focus.call("global-set-command", "event:refresh-python", ev.refresh)
    focus.call("event:refresh");

    return 1

editor.call("global-set-command", "attach-display-pygtk", new_display);
