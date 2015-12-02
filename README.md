<!--
# Copyright Neil Brown ©2015 <neil@brown.name>
# May be distrubuted under terms of GPLv2 - see file:COPYING
-->

edlib - library for building a document editor
==============================================

edlib is an extensible document editor.  It is inspired in part by
emacs, both by its strengths and its weaknesses.

emacs provides a programming language - E-lisp - for configuring and
extending the editor.  edlib doesn't.  It allows various pre-existing
languages to be used to configure and extend the editor.  It does
this by providing a library of core editing tools and providing
bindings to various languages.

At least, that is the plan.  At time if writing, edlib is just a
demonstration of some of those tools and has no binding yet.

The particular value-add of edlib over emacs (apart from the obvious
"NIH" issues) is that document rendering will be fully extensible.  A
buffer can have multiple views, and each view can show very different
things, scriptable code is run whenever text rendering is required.
This should make implementing documents with non-trivial structures a
lot easier.  I hope.

edlib is designed to have well defined abstractions that can be
exported to various languages for them to manipulate.  They include
attributes, text buffers, marks and points, panes, displays, events,
and keymaps.

attributes
----------

An attribute is a simple name/value pair, both being strings.
Attributes can be associated with various other objects, but
particularly with characters in a text buffer.  Parsing code can
annotate a buffer with attributes, and rendering code can use these
attributes to guide rendering.  e.g. parsing code can attach
"spelling=wrong" or "spelling=doubtful" and rendering can underline in
red or whatever.

text buffer
-----------

A text buffer (sometimes just a "text") stores a string of characters
and an edit history.  The characters can be read from a file, or
manipulated by code.  The edit history allows indefinite undo and
redo.

It is expected that every document will be represented as a text.
Some views of the document would just display the text, others might
display the result of parsing the text, still others might just
display some of the attributes attached to the text.

A text will often be read from and saved to a file in external
storage.  However an extension module might fetch a text over the
network using some protocol, or might generate it entirely
algorithmically.

marks and points
----------------

A "mark" identifies a location in a text buffer.  The location is
between two characters (or at the start or end), and the mark remains
at that location despite any edits that do not affect neighbouring
characters.

A mark can have a 'type'.  All marks of a particular type are linked
together in text-order so moving among marks of a type is easy.

"Points" are a special sort of mark which have linkage to nearby marks
of all different types.  All modification to the text happen at a
point.

When a modification happens, the preceding mark of each type is
"notified".  This allows it (or its owner) to adjust to the change,
such as by reparsing the text.

An example use is to have a type of marks which are used to track line
numbers.  "line-count" marks are places every 500 lines (or so) with
an attribute recording exactly how many lines between this and the
next "line-count" mark.  When a change happens, the recorded line
count is cleared.  When a line count or line number is needed, the
list of "line-count" marks is walked from the start.  If any has its
count cleared, the lines in that section are counted and the record is
updated.  Otherwise all that is required is simply adding up a few
numbers.

Note: the notification of marks on a change is not yet implemented.

Marks could be used by a parser to identify key locations which would
allow a renderer to find the important content quickly if it was only
rendering a partial view - such as the headings in outline mode.

Each mark (and point) has a unique sequence number ordered by position
in the text.  This makes it easy to determine relative order of two marks.

Each view of a buffer will have its own point.

displays
--------

A "display" displays rendered content and accepts keyboard and mouse
input.   Currently only a single 'ncurses' display is supported, but
I intend to support multiple 'ncurses' displays and also X11 displays
and maybe other display/input technologies.

panes
-----

A pane is a rectangular area of a display.  It may receive input from
the display and may render content.  Panes are arranged in an
hierarchy with parents generally responsible for their children.

Each pane can have a 'focus' child.  Keyboard input from the display
goes to the final pane found when following these 'focus' links from the
root. Mouse input goes to the pane under the mouse which has no
children also under the mouse.

Panes can have a 'z' depth - higher 'z' numbers are in front of lower
numbers.  This allows for pop-ups and floating windows.  For example a
list of filename completions might appear in a floating window, which
then disappears when the selection has been made.

Each pane must know how to refresh itself, unless the parent does
that.  Various amounts of 'damage' may have happened which lead to
different amounts of work in refreshing.  The design for this is not
completely clear yet.  It currently understand damage to "Size",
"Content" and "Cursor position".

events
------

Events happen at panes and propagate up parent linkages until a
handler is found.  The handler may invoke further events, which
typically start at the original pane and themselves propagate up.

Events include:

- keystrokes.  These go to the focus pane.  They can include
  modifiers (e.g. C-x or META) which cause subsequent keystrokes
  to be reported differently.
- mouse clicks.  These go to the pane under the mouse.  Mouse
  movement will eventually be included.  Possibly a 'grab' mechanism
  will be needed to direct motion and button release events.
- text-replace.  This requests that a range of text be replaced by
  another range (either can be empty).  When a keystroke implies
  a text change, it is not performed directly but instead causes a
  "text-replace" event to be sent.  This allows a module to easily
  capture and validate all changes to text without having to capture
  all relevant key strokes and mouse actions.
- movement.  This identifies a unit of text (char, word, line, etc)
  and a count and allows each pane to interpret it in a local context.
  This is similar to "text-replace" in that it allows handlers to
  capture functionality rather than just keystrokes.
- search.  I haven't really thought this through, but it seem like
  useful functionality to specifically delegate.


keymaps
-------

A keymap maps keystrokes to commands. It should really be "eventmap"
as all events are included: key, mouse, replace, movement etc.
The full unicode character space can be mapped as well as multiple
mouse clicks and assorted auxiliary functions.

An event in mapped to a "command".   Commands can be provided by
different modules or language bindings.  A command receives the target
pane, the event, and a structure of auxiliary information such as x/y
co-ordinate for mouse events or a text string to insert.


bindings
--------

This is totally unimplemented so the details are probably all wrong,
but it something thing that will be needed.

A "binding" is a connection between the library and some language
and/or runtime.  To load an extension module a binding to the language
will need to be established, and used to load the module.  The binding
will need access to some global objects such as the global keymap and
buffer list.

I'm hoping to support multiple threads so that extension modules
can run in parallel with editing code.  For example an extension
module might start a thread to talk to an IMAP server to keep some
local buffers up-to-date with arriving mail, and to handle slow
requests.   I imaging threading being closely related to bindings, but
maybe not.

The first binding types I hope to implement are "C" - which is really
just loadable shared objects - and Python.


Core Extensions
===============

These are the basic common objects which can (I hope) be used to build
a rich document editor.  There need to be lots of extensions of course
to make them useful.  Some of these "Extensions" are so intrinsic that
they are part of edlib, not attached through any binding.

tile
----

The "tile" handler takes a pane (typically the root pane of a display)
and divides it up into 1 or more non-overlapping tiles.  Tiles are
grouped in horizonal and vertical stacks.  Tiles can be split, can be
discarded or can be resized.  Any of these operations may affect other
tiles.

The leaves of the tile tree need to have other handlers attached.  The
tiler doesn't render anything itself, not even borders.  That is left
to the children.

view
----

A "view" connects a text to a pane.  It holds the text and the point
and draws borders.  A horizontal border at the bottom can report
the name of the text and possibly other information.  A vertical
border acts largely to separate panes but also serves as a scroll
bar.  The text manager can set attributes on the text identifying
scroll-bar position, and the "view" will render them and handle mouse
events to request movement.

text-render
-----------

Basic text rendering is a core extension.  It will probably grow a lot
of optional features such as wrapping or truncation of long lines,
mapping attributes to colours etc though a provided mapping, and maybe
centering or filling based on attributes.

Simple views will probably use text-render.  More complex views won't.


TO-DO
=====

There is so very much to do.  At time of writing most of the core
infrastructure is in place, but it isn't all connected and is not at
all polished. It is certainly possible to start writing simple
commands to test out functionality and explore interfaces.  As
functionality is added I expect lots of details to be refine.

Below is a very rough list, which serves to highlight how little
really works.  Some of these should be easy to write.  Others will
requires careful design though and possibly deep changes.

- movement: char, word, line, vertical, paragraph, page, start/end
- text entry
- delete
- notifications of change to marks
- search
- number prefix
- choose file name - with floating pane
- open file
- save file
- file names in status line
- autosave
- cut/copy
- paste
- undo/redo
- auto-indent
- auto-wrap
- c-mode parsing
- color highlights
- line/word count
- scroll bar
- spell check - async?

Some Ideas
==========

I have big dreams for edlib.  How much will ever be reality is an open
question, but dreams are nice.  Here are some, big an little.

email with notmuch
------------------

I loved using emacs for reading mail, but it became too clumsy.  I
want to go back to using the one editor for everything.

I imagine using notmuch as an indexing backend and viewing everything
via edlib.  Viewing HTML would certainly be an interesting challenge
but I can probably live without that if everything else works well.

Attached images, including PDF, is important.  I would certainly like
to be able to show images in a pane but have not designed anything for
that yet.

Text mode server
----------------

I only recently discovered "emacsclient -t".  I think I like it.  I
certainly want edlib to have a "server" mode and to be able to perform
editing in arbitrary terminal windows where I do other work.

calculator in floating pane
---------------------------

I very often use 'bc' for hex/decimal conversion etc.  But it is a
little clumsy.  I want an easy calculator on my desktop with base
conversion.  I'd like to use edlib.  I imagine a small pop-up
appearing which automatically converts whatever I type.

spread sheet
------------

Many years ago I started writing a spreadsheet program in emacs-lisp.
The document was a 'LaTeX' document with specially marked "tabular"
sections.  Each cell was on a line by itself.  It consisted of the
current appearance of the text, and then a comment containing the
formula and formatting rules.

The e-lisp code would hide the comment and the newlines so that it
looked like a table.  When the focus entered a cell, it would switch
to displaying the formula, or optionally the formatting.  Any change
to formula would propagate around the table.

It never worked very well.  I felt I spent most of my time fighting
with emacs rather than writing useful code.

If I can make a spreadsheet work at least reasonably well with edlib,
then edlib will be a success.

mark-right
----------

Markdown?  Commonmark?  Some sort of markup is useful and there are
few if any that are both easy to write and highly functional.

I like writing with markdown, but it is far from complete.  So I'd
like to create a mark-down mode that format as I type and which can
render to PDF.  Then I would start extending it.  Some extensions like
table are fairly standard - maybe ASCII-Math too.

Description-lists are an obvious (to me) omission.  Anchors for links,
structure tags (author, title), foot notes, insets for
table/figure/equation etc, with labeling.  Index, contents...

Figure Drawings?  There are lots of drawing languages.  It would be
nice to find a simple but versatile one to embed in markright.


Outline code rendering.
-----------------------

I like the principle of outlines and only showing the heading of
nearby sections, but the detail of the current section.  I've always
found them a bit clumsy to use.  I want the rendering to automatically
do what I want, partly depending on where the cursor is, partly
depending on space.

So when I am editing I want to see a lot of immediately surrounding
text, but I also want to see nearby minor headings and all major
headings.
If headings are short enough and there are enough of them, the having
several to a line would be a good idea - maybe even abbreviated.  If I
click on an abbreviated heading in the middle of a line at the top of
the screen, then that section should open up, and the nearby sections
should get a bit more space.

When searching, I probably don't need as much context of the current
point, so less of the current section would be displayed, more of
surrounding sections.  If those surrounding sections also contained
matches, then that would be the part of those sections that was shown.

I would like to implement this sort of view for markright mode, but
more importantly I want to implement it for C-mode.  When editing C
code (which I do a lot) I want the few lines at the top and bottom of
the view to just list some function names.  Then maybe a few lines
with function signature one the whole line.

Certainly the start of the current function would appear somewhere no
matter where in the function I am editing, and as many of the
variables as possible.  If I am in an 'if' statement in a 'for' look,
then the loop header and the if condition would be displayed if at all
possible.

hexedit
-------

This is something that is very clumsy with emacs, and should be very
easy with edlib.  A 'hexedit' view shows the hex value of each byte in
the file in a nice regular pattern.  For ASCII chars, it also shows
the character separately.

terminal emulator
-----------------

I never quite got into using shell-mode in emacs - it never felt quite
as raw as an xterm.  And the infinite history bothered me - possibly
irrationally.

Still, a shell mode for edlib  might be a useful thing, though having
edlib easily available in any terminal window might be just as good.

If I did a shell mode, I would capture the output of each command into
a separate buffer, and display all those buffers in the one view.
Sufficiently old buffers would be discarded.  Output from any recent
command could easily be saved or piped.  It would be possible to
arrange for some interactive commands to also send output of each
command to a separate buffer.

Auto paging would be disabled (where possible) and edlib would page
output as needed.  This means that `cat bigfile` could move the whole
file into a buffer, which wouldn't be good.  If a 'less' command could
give the filename to edlib and let it display, that might be nice.
