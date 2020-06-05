# -*- coding: utf-8 -*-
# Copyright Neil Brown ©2015-2020 <neil@brown.name>
# May be distributed under terms of GPLv2 - see file:COPYING

# edlib display module using pygtk to create a window, handle drawing, and
# receive mouse/keyboard events.
# provides eventloop function using gtk.main.

import os
import gi
import time
import cairo

gi.require_version('Gtk', '3.0')
gi.require_version('PangoCairo', '1.0')
gi.require_foreign("cairo")
from gi.repository import Gtk, Gdk, Pango, PangoCairo, GdkPixbuf

class EdDisplay(edlib.Pane):
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus, z=1)
        self.win = Gtk.Window()
        # panes[] is a mapping from edlib.Pane objects to cairo surface objects.
        # Where a pane has the same size as its parent, only the parent can have
        # a surface.
        self.panes = {}
        self.win.set_title("EDLIB")
        self.win.connect('destroy', self.close_win)
        self.create_ui()
        self["scale:M"] = "%dx%d" % (self.charwidth, self.lineheight)
        self.w = int(self.charwidth * 80.0)
        self.h = int(self.lineheight * 24.0)
        self.call("editor:request:all-displays")
        self.noclose = None
        self.last_event = 0
        self.win.show()


    def handle_notify_displays(self, key, comm2, **a):
        "handle:all-displays"
        comm2("callback:display", self, self.last_event)
        return 0

    def handle_postorder(self, key, **a):
        "handle:Refresh:postorder"
        self.text.queue_draw()
        return 1

    def handle_close_window(self, key, focus, **a):
        "handle:Display:close"
        if self.noclose:
            focus.call("Message", self.noclose)
            return 1
        x = []
        focus.call("editor:notify:all-displays", lambda key,**a:x.append(1))
        if len(x) > 1:
            self.close_win()
        else:
            focus.call("Message", "Cannot close only window.")
        return 1

    def handle_set_noclose(self, key, str, **a):
        "handle:Display:set-noclose"
        self.noclose = str
        return 1

    def handle_fullscreen(self, key, num, **a):
        "handle:Display:fullscreen"
        if num > 0:
            self.win.fullscreen()
        else:
            self.win.unfullscreen()
        return 1

    def handle_new(self, key, home, **a):
        "handle:Display:new"
        global editor
        p = editor.call("attach-input", ret='focus')
        p['DISPLAY'] = self['DISPLAY']
        newdisp = EdDisplay(p)
        home.clone_children(newdisp);
        return 1

    def handle_close(self, key, **a):
        "handle:Close"
        self.win.destroy()
        return True

    def handle_clear(self, key, focus, str, str2, **a):
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
        self.damaged(edlib.DAMAGED_POSTORDER)
        return True

    def handle_text_size(self, key, num, num2, focus, str, str2, comm2, **a):
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
        ink,l = layout.get_pixel_extents()
        ascent = metric.get_ascent() / Pango.SCALE
        if num >= 0:
            if l.width <= num:
                max_bytes = len(str.encode("utf-8"))
            else:
                inside, max_chars,extra = layout.xy_to_index(Pango.SCALE*num,
                                                     metric.get_ascent())
                max_bytes = len(str[:max_chars].encode("utf-8"))
        else:
            max_bytes = 0
        return comm2("callback:size", focus, max_bytes, int(ascent),
                     (l.width, l.height))

    def handle_draw_text(self, key, num, num2, focus, str, str2, xy, **a):
        "handle:Draw:text"
        self.damaged(edlib.DAMAGED_POSTORDER)

        (x,y) = xy
        attr=""
        if str2 is not None:
            attr = str2
        if num2 is not None:
            scale = num2 * 10 / self.charwidth
        else:
            scale = 1000
        fd = self.extract_font(attr, scale)

        fg, bg = self.get_colours(attr)

        pm, xo, yo = self.find_pixmap(focus)
        x += xo; y += yo
        cr = cairo.Context(pm)
        pl = PangoCairo.create_layout(cr)
        pl.set_text(str)
        pl.set_font_description(fd)

        fontmap = PangoCairo.font_map_get_default()
        font = fontmap.load_font(fontmap.create_context(), fd)
        metric = font.get_metrics()
        ascent = metric.get_ascent() / Pango.SCALE
        ink, log = pl.get_pixel_extents()
        lx = log.x; ly = log.y
        width = log.width; height = log.height

        if bg:
            cr.set_source_rgb(bg.red, bg.green, bg.blue)
            cr.rectangle(x+lx, y-ascent+ly, width, height)
            cr.fill()

        cr.set_source_rgb(fg.red, fg.green, fg.blue)
        cr.move_to(x, y-ascent)
        PangoCairo.show_layout(cr, pl)
        cr.stroke()

        if num >= 0:
            # draw a cursor - outline box if not in-focus,
            # inverse-video if it is.
            c = pl.index_to_pos(num)
            cx = c.x; cy=c.y; cw=c.width; ch=c.height
            if cw <= 0:
                cw = metric.get_approximate_char_width()
            cx /= Pango.SCALE
            cy /= Pango.SCALE
            cw /= Pango.SCALE
            ch /= Pango.SCALE

            cr.rectangle(x+cx, y-ascent+cy, cw-1, ch-1)
            cr.stroke

            in_focus = self.in_focus
            while (in_focus and focus.parent.parent != focus and
                   focus.parent != self):
                if focus.parent.focus != focus and focus.z >= 0:
                    in_focus = False
                focus = focus.parent
            if in_focus:
                if fg:
                    cr.set_source_rgb(fg.red, fg.green, fg.blue)

                cr.rectangle(x+cx, y-ascent+cy, cw, ch)
                cr.fill()
                if num < len(str):
                    pass
                    #l2 = Pango.Layout(ctx)
                    #l2.set_font_description(fd)
                    #l2.set_text(str[num])
                    #fg, bg = self.get_colours(attr+",inverse")
                    #pm.draw_layout(self.gc, x+cx, y-ascent+cy, l2, fg, bg)

        return True

    def handle_image(self, key, num, num2, focus, str, str2, **a):
        "handle:Draw:image"
        self.damaged(edlib.DAMAGED_POSTORDER)
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
            pb = GdkPixbuf.Pixbuf.new_from_file(str)
        except:
            # create a red error image
            pb = Gdk.Pixbuf(Gdk.COLORSPACE_RGB, False, 8, w, h)
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
        scale = pb.scale_simple(w, h, GdkPixbuf.InterpType.HYPER)
        pm, xo, yo = self.find_pixmap(focus)
        cr = cairo.Context(pm)
        Gdk.cairo_set_source_pixbuf(cr, scale, x + xo, y + yo)
        cr.paint()
        return True

    def handle_notify_close(self, key, focus, **a):
        "handle:Notify:Close"
        if focus and focus in self.panes:
            del self.panes[focus]
        return True


    styles=["oblique","italic","bold","small-caps"]

    def extract_font(self, attrs, scale):
        "Return a Pango.FontDescription"
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
        fd = Pango.FontDescription(family+' '+style+' '+str(size))
        if scale != 1000:
            fd.set_size(fd.get_size() * scale / 1000)
        return fd

    def color_parse(self, rgb, c):
        col = self.call("colour:map", c, ret='str')
        if not rgb.parse(col):
            raise ValueError

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
            fgc = Gdk.RGBA()
            try:
                self.color_parse(fgc, fg)
            except:
                fgc.parse('black')
        else:
            fgc = None

        if bg:
            bgc = Gdk.RGBA()
            try:
                self.color_parse(bgc, bg)
            except:
                bgc.parse('white')
        else:
            bgc = None

        return fgc, bgc

    def get_pixmap(self, p):
        # Find or create pixmap for drawing on this pane.
        if p in self.panes:
            pm = self.panes[p]
            w = pm.get_width()
            h = pm.get_height()
            if w == p.w and h == p.h:
                return pm
            del self.panes[p]
        else:
            self.add_notify(p, "Notify:Close")
        self.panes[p] = cairo.ImageSurface(cairo.Format.RGB24, p.w, p.h)
        return self.panes[p]

    def find_pixmap(self, p):
        # Find a pixmap already existing on this pane
        # or an ancestor, and return the pixmap with x and y
        # offset of this pane in the pixmap.

        x = 0; y = 0
        while p.parent != p and p not in self.panes:
            x += p.x
            y += p.y
            p = p.parent
        if p in self.panes:
            return (self.panes[p], x, y)
        # This must not happen. What should I do?

    def close_win(self, *a):
        self.close()

    def create_ui(self):
        text = Gtk.DrawingArea()
        self.text = text
        self.win.add(text)
        text.show()
        self.fd = Pango.FontDescription("mono 10")
        #text.modify_font(self.fd)
        ctx = text.get_pango_context()
        metric = ctx.get_metrics(self.fd)
        self.lineheight = (metric.get_ascent() + metric.get_descent()) / Pango.SCALE
        self.charwidth = metric.get_approximate_char_width() / Pango.SCALE
        self.win.set_default_size(self.charwidth * 80, self.lineheight * 24)

        self.im = Gtk.IMContextSimple()
        self.in_focus = True
        dir(self)
        self.text.connect("draw", self.refresh)
        self.text.connect("focus-in-event", self.focus_in)
        self.text.connect("focus-out-event", self.focus_out)
        self.text.connect("button-press-event", self.press)
        self.text.connect("button-release-event", self.release)
        self.text.connect("scroll-event", self.scroll)
        self.text.connect("key-press-event", self.keystroke)
        self.motion_handler = self.text.connect("motion-notify-event", self.motion)
        self.text.handler_block(self.motion_handler)
        self.motion_blocked = True
        self.im.connect("commit", self.keyinput)
        self.text.connect("configure-event", self.reconfigure)
        self.text.set_events(Gdk.EventMask.EXPOSURE_MASK|
                             Gdk.EventMask.STRUCTURE_MASK|
                             Gdk.EventMask.BUTTON_PRESS_MASK|
                             Gdk.EventMask.BUTTON_RELEASE_MASK|
                             Gdk.EventMask.SCROLL_MASK|
                             Gdk.EventMask.KEY_PRESS_MASK|
                             Gdk.EventMask.KEY_RELEASE_MASK|
                             Gdk.EventMask.POINTER_MOTION_MASK|
                             Gdk.EventMask.POINTER_MOTION_HINT_MASK);
        self.text.set_property("can-focus", True)

    def block_motion(self):
        if self.motion_blocked:
            return
        self.text.handler_block(self.motion_handler)
        self.motion_blocked = True

    def unblock_motion(self):
        if not self.motion_blocked:
            return
        self.text.handler_unblock(self.motion_handler)
        self.motion_blocked = False

    def refresh(self, da, ctx):
        edlib.time_start(edlib.TIME_WINDOW)
        l = list(self.panes)
        l.sort(key=lambda pane: pane.abs_z)
        for p in l:
            pm = self.panes[p]
            rx,ry = self.mapxy(p, 0, 0)
            # FIXME draw on surface or GdkPixbuf
            ctx.set_source_surface(pm, rx, ry)
            ctx.paint()
        edlib.time_stop(edlib.TIME_WINDOW)

    def focus_in(self, *a):
        edlib.time_start(edlib.TIME_WINDOW)
        self.im.focus_in()
        self.in_focus = True
        f = self.leaf
        pt = f.call("doc:point", ret='mark')
        f.call("view:changed", pt)
        self.call("pane:refocus")
        edlib.time_stop(edlib.TIME_WINDOW)

    def focus_out(self, *a):
        edlib.time_start(edlib.TIME_WINDOW)
        self.im.focus_out()
        self.in_focus = False
        f = self.leaf
        pt = f.call("doc:point", ret='mark')
        f.call("view:changed", pt)
        edlib.time_stop(edlib.TIME_WINDOW)

    def reconfigure(self, w, ev):
        edlib.time_start(edlib.TIME_WINDOW)
        alloc = w.get_allocation()
        if self.w == alloc.width and self.h == alloc.height:
            return None
        self.w = alloc.width
        self.h = alloc.height
        edlib.time_stop(edlib.TIME_WINDOW)

    def press(self, c, event):
        if event.type != Gdk.EventType.BUTTON_PRESS:
            #maybe GDK_2BUTTON_PRESS - don't want that.
            return
        edlib.time_start(edlib.TIME_KEY)
        c.grab_focus()
        self.unblock_motion()
        x = int(event.x)
        y = int(event.y)
        s = ":Press-" + ("%d"%event.button)
        mod = ""
        if event.state & Gdk.ModifierType.SHIFT_MASK:
            mod = ":S" + mod
        if event.state & Gdk.ModifierType.CONTROL_MASK:
            mod = ":C" + mod
        if event.state & Gdk.ModifierType.MOD1_MASK:
            mod = ":M" + mod
        self.last_event = int(time.time())
        self.call("Mouse-event", mod+s, mod, (x,y),
                  event.button, 1)
        edlib.time_stop(edlib.TIME_KEY)

    def release(self, c, event):
        edlib.time_start(edlib.TIME_KEY)
        c.grab_focus()
        x = int(event.x)
        y = int(event.y)
        s = ":Release-" + ("%d"%event.button)
        # ignore modifiers for Release
        self.last_event = int(time.time())
        self.call("Mouse-event", s, (x,y), event.button, 2)
        edlib.time_stop(edlib.TIME_KEY)

    def motion(self, c, event):
        edlib.time_start(edlib.TIME_KEY)
        x = int(event.x)
        y = int(event.y)
        ret = self.call("Mouse-event", ":Motion", (x,y), 0, 3)
        if not ret:
            self.block_motion()

    def scroll(self, c, event):
        edlib.time_start(edlib.TIME_KEY)
        c.grab_focus()
        x = int(event.x)
        y = int(event.y)
        if event.direction == Gdk.ScrollDirection.UP:
            s = ":Press-4"
            b = 4
        elif event.direction == Gdk.ScrollDirection.DOWN:
            s = ":Press-5"
            b = 5
        else:
            edlib.time_stop(edlib.TIME_KEY)
            return 0
        if event.state & Gdk.ModifierType.SHIFT_MASK:
            s = ":S" + s;
        if event.state & Gdk.ModifierType.CONTROL_MASK:
            s = ":C" + s;
        if event.state & Gdk.ModifierType.MOD1_MASK:
            s = ":M" + s;
        self.call("Mouse-event", s, (x,y), b, 1)
        edlib.time_stop(edlib.TIME_KEY)

    eventmap = { "Return"	: ":Enter\037:C-M",
                 "Tab"		: ":Tab\037:C-I",
                 "ISO_Left_Tab"	: ":Tab", # :S is added below
                 "Escape"	: ":ESC\037:C-[",
                 "Linefeed"	: ":LF\037:C-J",
                 "Down"		: ":Down",
                 "Up"		: ":Up",
                 "Left"		: ":Left",
                 "Right"	: ":Right",
                 "Home"		: ":Home",
                 "End"		: ":End",
                 "BackSpace"	: ":Backspace\037:C-H",
                 "Delete"	: ":Del",
                 "Insert"	: ":Ins",
                 "Page_Up"	: ":Prior",
                 "Page_Down"	: ":Next",
                 "space"	: "- ",
                 "F1"		: ":F1",
                 "F2"		: ":F2",
                 "F3"		: ":F3",
                 "F4"		: ":F4",
                 "F5"		: ":F5",
                 "F6"		: ":F6",
                 "F7"		: ":F7",
                 "F8"		: ":F8",
                 "F9"		: ":F9",
                 "F10"		: ":F11",
                 "F11"		: ":F11",
                 "F12"		: ":F12",
                 }

    def keyinput(self, c, strng):
        edlib.time_start(edlib.TIME_KEY)
        self.last_event = int(time.time())
        self.call("Keystroke", "-" + strng)
        edlib.time_stop(edlib.TIME_KEY)

    def keystroke(self, c, event):
        edlib.time_start(edlib.TIME_KEY)
        if self.im.filter_keypress(event):
            edlib.time_stop(edlib.TIME_KEY)
            return

        p = ""
        if event.state & Gdk.ModifierType.MOD1_MASK:
            p = ":M"
        kv = Gdk.keyval_name(event.keyval)
        if kv in self.eventmap:
            if event.state & Gdk.ModifierType.CONTROL_MASK:
                p += ":C"
            if event.state & Gdk.ModifierType.SHIFT_MASK:
                p += ":S"
            s = p + self.eventmap[kv]
        else:
            s = event.string
            if len(s) == 0:
                return
            if ord(s[0]) < 32:
                s = p + ":C-" + chr(ord(s[0])+64) + "\037" + p + ":C-" + chr(ord(s[0]) + 96)
            else:
                s = "-" + s
                if event.state & Gdk.ModifierType.CONTROL_MASK:
                    s = ":C" + s;
                s = p + s
        self.last_event = int(time.time())
        self.call("Keystroke", s)
        edlib.time_stop(edlib.TIME_KEY)

    def do_clear(self, pm, colour):
        # FIXME surface
        cr = cairo.Context(pm)
        cr.set_source_rgb(colour.red, colour.green, colour.blue)
        cr.rectangle(0,0,pm.get_width(), pm.get_height())
        cr.fill()

def new_display(key, focus, comm2, **a):
    if not 'DISPLAY' in os.environ:
        return None
    if not os.environ['DISPLAY']:
        return None
    focus.call("attach-glibevents")

    if 'SCALE' in os.environ:
        sc = int(os.environ['SCALE'])
        s = Gtk.settings_get_default()
        s.set_long_property("Gtk-xft-dpi",sc*Pango.SCALE, "code")

    disp = EdDisplay(focus)
    disp['DISPLAY'] = os.environ['DISPLAY']
    p = disp.call("attach-x11selection", ret='focus')
    if not p:
        p = disp
    comm2('callback', p)
    return 1


editor.call("global-set-command", "attach-display-pygtk", new_display);
