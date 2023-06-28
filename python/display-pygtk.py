# -*- coding: utf-8 -*-
# Copyright Neil Brown ©2015-2023 <neil@brown.name>
# May be distributed under terms of GPLv2 - see file:COPYING

# edlib display module using pygtk to create a window, handle drawing, and
# receive mouse/keyboard events.
# provides eventloop function using gtk.main.

import edlib
import os, fcntl
import gi
import time
import cairo
import subprocess

gi.require_version('Gtk', '3.0')
gi.require_version('PangoCairo', '1.0')
gi.require_foreign("cairo")
from gi.repository import Gtk, Gdk, Pango, PangoCairo, GdkPixbuf, Gio

def wait_for(p):
    try:
        r = os.read(p.stdout.fileno(), 4096)
    except IOError:
        # nothing to read yet
        return 1
    if r:
        # not eof yet
        return 1
    p.communicate()
    # stop waiting
    return edlib.Efalse

def wait_on(self, p):
    # wait on pipe for exit
    fd = self.pipe.stdout.fileno()
    fl = fcntl.fcntl(fd, fcntl.F_GETFL);
    fcntl.fcntl(fd, fcntl.F_SETFL, fl | os.O_NONBLOCK)
    self.call("event:read", fd, lambda key, **a: wait_for(p))

class EdDisplay(edlib.Pane):
    def __init__(self, focus, display):
        edlib.Pane.__init__(self, focus, z=1)
        self['DISPLAY'] = display
        self.win = Gtk.Window()
        # panes[] is a mapping from edlib.Pane objects to cairo surface objects.
        # A pane only has a surface if it has been explicitly cleared.
        # When a pane is cleared to a colour with nothing drawn, self.bg
        # records the colour for the pane
        self.panes = {}
        self.bg = {}
        self.win.set_title("EDLIB")
        self.win.connect('destroy', self.destroy_win)
        self.win.connect('delete-event', self.close_win)
        self.create_ui()
        # report approximate size of an "M"
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
        return 1

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
            return edlib.Efallthrough
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

    def handle_new(self, key, focus, **a):
        "handle:Display:new"
        newdisp = EdDisplay(edlib.editor, self['DISPLAY'])
        p = newdisp.call("editor:activate-display", ret='pane')
        if p:
            focus.call("doc:attach-view", p, 1)
        return 1

    def handle_external(self, key, str, **a):
        "handle:Display:external-viewer"
        disp = self['DISPLAY']
        if not str or not disp:
            return edlib.Enoarg
        env = os.environ.copy()
        env['DISPLAY'] = disp

        p = subprocess.Popen(["xdg-open", str], env=env,
                         stdin = subprocess.DEVNULL,
                         stdout = subprocess.PIPE,
                         stderr = subprocess.DEVNULL)
        if p:
            wait_on(self, p)
        return 1

    def handle_close(self, key, **a):
        "handle:Close"
        self.win.destroy()
        return True

    def handle_clear(self, key, focus, str1, **a):
        "handle:Draw:clear"
        attr = str1
        if attr is not None:
            fg, bg, ul = self.get_colours(attr)
            if not bg:
                bg = Gdk.RGBA()
                bg.parse("white")
        else:
            bg = None
        src = None
        if not bg:
            src = self.find_pixmap(focus.parent)
            if not src:
                fg, bg, ul = self.get_colours("bg:white")
            else:
                bg = src[3]
        pm = self.get_pixmap(focus)
        cr = cairo.Context(pm)
        if bg:
            cr.set_source_rgb(bg.red, bg.green, bg.blue)
            cr.rectangle(0,0,pm.get_width(), pm.get_height())
            cr.fill()
        else:
            (pm2, x, y, bg) = src
            x += focus.x
            y += focus.y
            cr.set_source_surface(pm2, -x, -y)
            cr.paint()
        self.bg[focus] = bg

        self.damaged(edlib.DAMAGED_POSTORDER)
        return True

    def handle_text_size(self, key, num, num2, focus, str, str2, comm2, **a):
        "handle:Draw:text-size"
        attr=""
        if str2 is not None:
            attr = str2
        if num2 is not None:
            scale = num2 * 10 / self.charwidth
        else:
            scale = 1000
        fd = self.extract_font(attr, scale)
        # If we use an empty string, line height is wrong
        layout = self.text.create_pango_layout(str if str else "M")
        layout.set_font_description(fd)
        ink,l = layout.get_pixel_extents()
        baseline = int(layout.get_baseline() / Pango.SCALE)
        if num >= 0:
            if not str or l.width <= num:
                max_bytes = len(str.encode("utf-8"))
            else:
                inside, max_chars,extra = layout.xy_to_index(Pango.SCALE*num,
                                                             baseline)
                max_bytes = len(str[:max_chars].encode("utf-8"))
        else:
            max_bytes = 0
        return comm2("callback:size", focus, max_bytes, baseline,
                     (l.width if str else 0, l.height))

    def handle_draw_text(self, key, num, num2, focus, str, str2, xy, **a):
        "handle:Draw:text"

        if str is None:
            return edlib.Enoarg

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

        fg, bg, ul = self.get_colours(attr)

        pm = self.find_pixmap(focus, True)
        if pm is None:
            return edlib.Einval
        pm, xo, yo, pbg = pm
        x += xo; y += yo
        cr = cairo.Context(pm)
        pl = PangoCairo.create_layout(cr)
        pl.set_text(str)
        pl.set_font_description(fd)

        fontmap = PangoCairo.font_map_get_default()
        font = fontmap.load_font(fontmap.create_context(), fd)
        metric = font.get_metrics()
        baseline = pl.get_baseline() / Pango.SCALE
        ink, log = pl.get_pixel_extents()
        lx = log.x; ly = log.y
        width = log.width; height = log.height

        if bg:
            cr.set_source_rgb(bg.red, bg.green, bg.blue)
            cr.rectangle(x+lx, y-baseline+ly, width, height)
            cr.fill()
        cr.set_source_rgb(fg.red, fg.green, fg.blue)
        if ul:
            cr.rectangle(x+lx, y+ly+2, width, 1); cr.fill()
        cr.move_to(x, y-baseline)
        PangoCairo.show_layout(cr, pl)
        cr.stroke()

        if num >= 0:
            # draw a cursor - outline box if not in-focus,
            # inverse-video if it is.
            # FIXME num is a byte-offset, not a char offset!!!
            c = pl.index_to_pos(num)
            cx = c.x; cy=c.y; cw=c.width; ch=c.height
            if cw <= 0:
                pl.set_text("M")
                ink,log = pl.get_extents()
                cw = log.width
            cx /= Pango.SCALE
            cy /= Pango.SCALE
            cw /= Pango.SCALE
            ch /= Pango.SCALE

            cr.rectangle(x+cx, y-baseline+cy, cw-1, ch-1)
            cr.set_line_width(1)
            cr.stroke()

            in_focus = self.in_focus
            while (in_focus and focus.parent.parent != focus and
                   focus.parent != self):
                if focus.parent.focus != focus and focus.z >= 0:
                    in_focus = False
                focus = focus.parent
            if in_focus:
                if fg:
                    cr.set_source_rgb(fg.red, fg.green, fg.blue)

                cr.rectangle(x+cx, y-baseline+cy, cw, ch)
                cr.fill()
                if num < len(str):
                    pl2 = PangoCairo.create_layout(cr)
                    pl2.set_font_description(fd)
                    pl2.set_text(str[num])
                    fg, bg, ul = self.get_colours(attr+",inverse")
                    if fg:
                        cr.set_source_rgb(fg.red, fg.green, fg.blue)
                    cr.move_to(x+cx, y-baseline+cy)
                    PangoCairo.show_layout(cr, pl2)
                    cr.stroke()

        return True

    def handle_image(self, key, num, focus, str, str2, xy, **a):
        "handle:Draw:image"
        self.damaged(edlib.DAMAGED_POSTORDER)
        # 'str' identifies the image. Options are:
        #     file:filename  - load file from fs
        #     comm:command   - run command collecting bytes
        # 'num' is '16' if image should be stretched to fill pane
        # otherwise it is 'or' of
        #   0,1,2 for left/middle/right in x direction
        #   0,4,8 for top/middle/bottom in y direction
        # only one of these can be used as image will fill pane
        # in other direction.
        # xy gives a number of rows and cols to overlay on the image
        # for the purpose of cursor positioning.  If these are positive
        # and focus.cx,cy are not negative, draw a cursor at cx,cy
        # highlighting the relevant cell.
        if not str:
            return edlib.Enoarg
        stretch = num & 16
        pos = num
        w, h = focus.w, focus.h
        x, y = 0, 0
        try:
            if str.startswith("file:"):
                pb = GdkPixbuf.Pixbuf.new_from_file(str[5:])
            elif str.startswith("comm:"):
                img = focus.call(str[5:], str2, ret='bytes')
                io = Gio.MemoryInputStream.new_from_data(img)
                pb = GdkPixbuf.Pixbuf.new_from_stream(io)
            else:
                return edlib.Einval
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
        pm, xo, yo, pbg = self.find_pixmap(focus, True)
        cr = cairo.Context(pm)
        Gdk.cairo_set_source_pixbuf(cr, scale, x + xo, y + yo)
        cr.paint()

        (rows,cols) = xy
        if rows > 0 and cols > 0 and focus.cx >= 0:
            cr.rectangle(focus.cx + xo, focus.cy + yo,
                         w/rows, h/cols)
            cr.set_line_width(1)
            cr.set_source_rgb(1,0,0)
            cr.stroke()
        return True

    def handle_image_size(self, key, focus, str1, str2, comm2, **a):
        "handle:Draw:image-size"
        if not str1 or not comm2:
            return edlib.Enoarg
        try:
            if str1.startswith("file:"):
                pb = GdkPixbuf.Pixbuf.new_from_file(str1[5:])
            elif str1.startswith("comm:"):
                img = focus.call(str1[5:], str2, ret='bytes')
                io = Gio.MemoryInputStream.new_from_data(img)
                pb = GdkPixbuf.Pixbuf.new_from_stream(io)
            else:
                return edlib.Einval
        except:
            return edlib.Einval
        comm2("cb:size", focus, (pb.get_width(), pb.get_height()))
        return True

    def handle_notify_close(self, key, focus, **a):
        "handle:Notify:Close"
        if focus and focus in self.panes:
            del self.panes[focus]
            self.damaged(edlib.DAMAGED_POSTORDER)
        if focus and focus in self.bg:
            del self.bg[focus]
        return True

    styles=["oblique","italic","bold","small-caps"]

    def extract_font(self, attrs, scale):
        "Return a Pango.FontDescription"
        family="mono"
        style=""
        size=10
        if scale <= 10:
            scale = 1000
        style = []
        for word in attrs.split(','):
            if word in self.styles and word not in style:
                style.append(word)
            elif word[:2] == 'no' and word[2:] in self.styles and word[2:] in style:
                style.remove(word[2:])
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
        fd = Pango.FontDescription(family+' '+(' '.join(style))+' '+str(size))
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
        underline = False
        for word in attrs.split(','):
            if word[0:3] == "fg:":
                fg = word[3:]
            if word[0:3] == "bg:":
                bg = word[3:]
            if word == "inverse":
                inv = True
            if word == "noinverse":
                inv = False
            if word == "underline":
                underline = True
            if word == "nounderline":
                underline = False
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

        return fgc, bgc, underline

    def get_pixmap(self, p):
        # Find or create pixmap for drawing on this pane.
        if p in self.panes:
            pm = self.panes[p]
            w = pm.get_width()
            h = pm.get_height()
            if w == p.w and h == p.h:
                return pm
            del self.panes[p]
            if p in self.bg:
                del self.bg[p]
        else:
            self.add_notify(p, "Notify:Close")
        self.panes[p] = cairo.ImageSurface(cairo.Format.RGB24, p.w, p.h)
        return self.panes[p]

    def find_pixmap(self, p, clearbg = False):
        # Find a pixmap already existing on this pane
        # or an ancestor, and return the pixmap with x and y
        # offset of this pane in the pixmap.

        x = 0; y = 0
        while p.parent != p and p not in self.panes:
            x += p.x
            y += p.y
            p = p.parent
        if p in self.panes:
            if p not in self.bg:
                return (self.panes[p], x, y, None)
            elif clearbg:
                del self.bg[p]
                return (self.panes[p], x, y, None)
            else:
                return (self.panes[p], x, y, self.bg[p])
        # This must not happen. What should I do?

    def close_win(self, *a):
        self.call("Display:close")
        return True

    def destroy_win(self, *a):
        self.parent("Display:close")
        return False

    def create_ui(self):
        text = Gtk.DrawingArea()
        self.text = text
        self.win.add(text)
        text.show()
        self.fd = Pango.FontDescription("mono 10")
        layout = self.text.create_pango_layout("M")
        layout.set_font_description(self.fd)
        ink, log = layout.get_pixel_extents()
        self.lineheight = log.height
        self.charwidth = log.width
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
                             Gdk.EventMask.POINTER_MOTION_HINT_MASK)
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
        l.sort(key=lambda pane: pane.abs_z * 1000 + pane.z)
        for p in l:
            pm = self.panes[p]
            rx,ry = self.mapxy(p, 0, 0)
            lox,loy = self.clipxy(p, 0, 0)
            hix,hiy = self.clipxy(p, p.w, p.h)
            # FIXME draw on surface or GdkPixbuf
            ctx.save()
            ctx.rectangle(lox, loy, hix-lox, hiy-loy)
            ctx.set_source_surface(pm, rx, ry)
            ctx.fill()
            ctx.restore()
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
            mod = ":A" + mod
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
        try:
            ret = self.call("Mouse-event", ":Motion", (x,y), 0, 3)
        except edlib.commandfailed:
            ret = -1
        if ret <= 0:
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
            s = ":S" + s
        if event.state & Gdk.ModifierType.CONTROL_MASK:
            s = ":C" + s
        if event.state & Gdk.ModifierType.MOD1_MASK:
            s = ":A" + s
        self.call("Mouse-event", s, (x,y), b, 1)
        edlib.time_stop(edlib.TIME_KEY)

    eventmap = { "Return"	: ":Enter",
                 "Tab"		: ":Tab",
                 "ISO_Left_Tab"	: ":Tab", # :S is added below
                 "Escape"	: ":ESC",
                 "Linefeed"	: ":LF",
                 "Down"		: ":Down",
                 "Up"		: ":Up",
                 "Left"		: ":Left",
                 "Right"	: ":Right",
                 "Home"		: ":Home",
                 "End"		: ":End",
                 "BackSpace"	: ":Backspace",
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
            p = ":A"
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
                s = p + ":C-" + chr(ord(s[0])+64)
            else:
                s = "-" + s
                if event.state & Gdk.ModifierType.CONTROL_MASK:
                    s = ":C" + s
                s = p + s
        self.last_event = int(time.time())
        self.call("Keystroke", s)
        edlib.time_stop(edlib.TIME_KEY)

def new_display(key, focus, comm2, str1, **a):
    if not str1:
        return None
    focus.call("attach-glibevents")
    ed = focus.root

    if 'SCALE' in os.environ:
        sc = int(os.environ['SCALE'])
        s = Gtk.settings_get_default()
        s.set_long_property("Gtk-xft-dpi",sc*Pango.SCALE, "code")

    disp = EdDisplay(focus, str1)
    p = disp.call("editor:activate-display", ret='pane')
    if p and focus != ed:
        p = focus.call("doc:attach-view", p, 1)
    comm2('callback', p)
    return 1

def new_display2(key, focus, **a):
    display = focus['DISPLAY']
    if not display:
        return None
    p = focus.root
    p.call("attach-glibevents")

    if 'SCALE' in os.environ:
        sc = int(os.environ['SCALE'])
        s = Gtk.settings_get_default()
        s.set_long_property("Gtk-xft-dpi",sc*Pango.SCALE, "code")

    disp = EdDisplay(p, display)
    p = disp.call("editor:activate-display", ret='pane')
    if p:
        focus.call("doc:attach-view", p, 1);
    return 1

edlib.editor.call("global-set-command", "attach-display-gtk", new_display)
edlib.editor.call("global-set-command", "interactive-cmd-gtkwindow", new_display2)
