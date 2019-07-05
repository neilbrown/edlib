<meta HTTP-EQUIV="Content-Type" CONTENT="text/html; charset=utf-8">

<!--
# Copyright Neil Brown ©2015-2016 <neil@brown.name>
# May be distrubuted under terms of GPLv2 - see file:COPYING
-->

Edlib - a library for building a document editor
==============================================

edlib is an extensible document editor.  It is inspired in part by
emacs, both by its strengths and its weaknesses.

emacs provides a programming language — E-lisp — for configuring and
extending the editor.  edlib doesn't.  It allows various pre-existing
languages to be used to configure and extend the editor.  It does
this by providing a library of core editing tools and providing
bindings to various languages.

At least, that is the plan.  At time if writing, only provides
bindings for C and Python, and these are incomplete.  Other languages
should be fairly easy.

The particular value-add of edlib over emacs (apart from the obvious
“NIH” issues) is that both document storage and document rendering are
fully extensible.  A document is not always a text buffer, it could
also be a mem-mapped files, a directory, or an internal data
structure.
Any document can have multiple views, and each view can
show very different things: scriptable code is run whenever
rendering is required.  This should make implementing documents with
non-trivial structures a lot easier.

edlib is designed to have well defined abstractions that can be
exported to various languages for them to manipulate.  They include
primarily commands, panes and marks, and also
attributes, documents, displays, events, and keymaps.

Commands
--------

Commands are the single mechanism by which control is transferred from
one part of the editor implementation to another.  They provide the
only way for code in one implementation language to invoke code
written in another language, and they are the preferred way to
interact between different modules even in the same language.

All commands receive the same types of arguments and produce an integer
result.  The arguments include two panes, two marks, three
strings, two numbers, two co-ordinate pairs and one command.  Extra
result values can be effected by passing them to a call to the
“command” argument - i.e the command can be used as a call-back.

Commands can be used internally to a module and externally for
communication between modules.  Externally visible commands appear
primarily in two sorts of places.  Each “pane” has a command which
performs all the actions required of, or provided by, that pane.  A
pane may also maintain a set of commands with assigned names that
are provided to it from elsewhere.  In particular the root pane
maintains a set of global named commands.

Two of the arguments provided to a command have very special
meanings.  One of the strings, known as “key”, identifies what action
should be performed.  For commands that have been assigned a name, the
“key” string passed will always be exactly the name by which the
command was found.  For commands which are assigned to a pane, the
“key” could be anything.  The command must determine if it understands
that key.  If it does it should perform the action (possibly
calling some other command internally).  If not it should return 0.
This return code usually causes the caller to look elsewhere for a
command to perform the desired action.

The other special argument is one of the panes, called “home”.  This
always identifies the pane in which the command was found, either as a
dedicated “pane” command or as a named command.  One of the primary
uses of “home” is to access “home->data” which a private data
structure owned by the pane.  The command associated with a
particular pane is typically the only code which can understand the
data.

Apart from the return value of zero which indicates “command not
understood”, a command can return a positive result on success or a
negative result indicating lack of success.  In particular, “-1” is a
generic failure return code, and “-2” indicates that the command will
complete asynchronously.  Commands only return this if they are
specifically documented to do say.  It is usually only relevant if a
callback was passed as the “command” argument and it indicates that
the callback has not yet been called, but otherwise no error has been
detected. (Actually, async commands are purely theoretical, so details
are likely to change).

Panes
-----

A pane combines an optional rectangular area of display with some
functionality.  As such it can receive mouse and keyboard events,
can draw on a display, and can send commands to other panes.  As
previously mentioned, panes can also store module-specific data.

All panes are arranged as a tree with all but the root having a parent
and many having siblings and children.  When a pane represents a
rectangle of display all children are restricted to just that
rectangle or less.  Often a child will cover exactly the same areas as
its parent.  In other cases several children will share out the area.

Events are often generated at a leaf of the tree of panes (i.e. a pane
with no children).  They travel up the tree towards the root until
they find a pane which can handle them.  That pane might handle the
event bey generating other events.  They will typically start looking
for handler at the same leaf.  For this reason branches of the pane
tree usually have more generic panes closer to the root and more
special-purpose panes near the leaves.

It is quite normal for there to be panes in the tree that are not
directly involves in displaying anything - these are just useful
containers for data and functionality.  Documents, described below,
exist as panes that are not directly displayed.  Instead there are
display pane which link to the document and display its content.

As well as a dedicated command (the “handler”) and private data, each
pane has:

- x,y co-ordinates together with width and height.  The co-ordinates
  are relative to the parent, and by recursive addition can be made
  absolute.
- a “z” value which indicates display priority with respect to
  siblings.  When siblings overlap, the sibling with the higher “z”
  value will be draw “over” siblings with a lower “z” value, including
  all the children of that sibling (independent of their z value).
- a selected child referred to as the “focus”. Keyboard input at a
  display is forwarded down the chain of focus links until it reaches
  a leaf pane.  This is where handling of the keystroke starts.
- a set of “damaged” flags which record if any changes have been made
  which might affect the display.
- an arbitrary set of attributes with assigned values.

Each pane may also request notifications from other panes.  These
include, but are not limited to, a notification when the pane is
destroyed and a notification when a document attached to the pane
changes.  This notifications are effected by calling the panes command
with a key like “notify:close” and with the second pane argument
(known as “focus”) set to the pane which is sending the notification.

Documents
---------

A document provides access to whatever data is being edited.  There
can be multiple implementations for documents but they all have a
common interface.

A “document” is assumed to be a linear sequence of elements each of
which presents as a single character and may have some attributes
associated with it.  For a “text” document, the characters are
typically Unicode characters stored as UTF-8 and the attributes, if
any, are hints for parsing or display.
For a “directory” document, the elements are entries in the directory
and the associated character reflects the type of entry.  The
attributes contain the useful information such as file name, size,
modify time etc.

A document is represents by a non-display pane.  These panes are
typically collected together as children of a “document-list” pane
which can be asked to add or find documents.  To display and a
document a document-display pane is normally created.  This contains,
in its private data, a reference to the document pane and a “point”
(see below) indicating where changes will happen.  Events that arrive
at the document-display pane will typically be forwarded to the
document, though they maybe be handled directly by the display pane.

Attributes
----------

An attribute is a simple name/value pair, both being strings.
Attributes can be associated with various other objects, including
marks, panes, and elements in a document.  Parsing code can
annotate a buffer with attributes, and rendering code can use these
attributes to guide rendering.  e.g. parsing code can attach
“spelling=wrong” or “spelling=doubtful” and rendering can underline in
red or whatever.

Currently most attributes have to be stored using a particular
implementation of attribute storage.  I'm not sure I want that in the
longer term.  I'm considering having the pane command support a
“attribute:get” command though there are still unresolved issues with
that idea.

Marks and Points
----------------

A “mark” identifies a location in a document.  The location is between
two elements in the document, or at the start or end, and the mark
remains at that location despite any edits that do not affect
neighbouring elements.

Marks come in three different sorts: ungrouped, grouped, and points.
All of these appear in document-order a singly linked list, all have a
sequence number in the list so ordering-tests are easy, and each can
have a set of attributes attached.

As well as identifying a location in a document, a mark can identify a
location in the display of that document location.  When a single
element in a document is displayed using multiple characters (as for
example a directory entry might be), the “rendering position” or
“rpos” can record where in those multiple characters the mark really
belongs.  I'm not yet sure how useful this is, but it seems like a good
idea.

An ungrouped mark has no property beyond the above.  A grouped marked
is included in a second linked list with all the other marks in the
same group.  This group is owned by a specific pane and keeps
information relevant to the task of that pane.  A pane responsible for
rendering part of a document might have marks identifying the start
and end of the visible portion, and maybe even the start of each line
in the visible portion.  An ungrouped mark also has a reference to an
arbitrary data structure which is understood by the pane which owns
the group.

A “point” is a special grouped-mark which is included in all of the
other lists of grouped marks.  This is achieved by using the external
reference to hold an auxiliary data structure which is linked in to
all of the lists.  Every document-display pane owns a point.  This
point is usually where changes to the document happen.  When the
notification mechanism mentioned earlier tells other panes of a chance
to the document, the point where the change happened is also reported.
From this point it is easy to find and update nearby marks of any
mark-group.

An example use is to have a group of marks which are used to track
line numbers.  “line-count” marks are placed every 500 lines (or so)
with an attribute recording exactly how many lines between this and
the next “line-count” mark.  When a change happens, the recorded line
count on the preceding mark is cleared.  When a line count or line
number is needed, the list of “line-count” marks is walked from the
start.  If any has its count cleared, the lines in that section are
counted and the record is updated.  Otherwise all that is required is
simply adding up a few numbers.

Marks could be used by a parser to identify key locations which would
allow a renderer to find the important content quickly if it was only
rendering a partial view - such as the headings in outline mode.

Displays
--------

A “display” is just a pane which can create an image somehow, and
responds to commands like “pane-clear”, “text-display”, and
“image-display”.  Displays are typically just below the root of the
“pane” tree, but this is not a requirement.

A display is also expected to call “Keystroke” and “Mouse-event”
commands in response to appropriate events.  There will propagate
towards the root and normally hit an input-management pane which will
find the appropriate target leaf, will convert to a full event,
e.g. adding a repeat count or indication of a prefix key, and will
submit the new event at the target.

Keymaps
-------

A keymap is a mapping from command names to commands.  In many cases a
similar data structure such as a Python “dict” could be used.  The
keymap implemented in edlib has one small advantage in that a range of
strings can be mapped to a command, then exceptions can be recorded.

Keymaps are a bit like attributes in that the concept is valuable but
it isn't yet clear how central a particular implementation should be.

Handling Commands
-----------------

Now that we have plenty of context, it is time to revisit commands to
discuss how they are called.  It is possible to invoke a specific
command directly if you have a reference to it but most often a more
general mechanism is used to find the appropriate command.  The most
common mechanism is to identify a “home” pane and the handler for that
pane and each ancestor will be tried in turn until the handler returns
a non-zero value, or until the root pane has been tried.  Very often
the starting home pane will also be the focus pane so when the two are
the same it is not necessary to specify both.

The other common mechanism is to follow the "notifier" chain from a
pane.  This lists a number of panes which have requested
notifications.  When calling notifiers, all target panes have their
handler called and if any return a non-zero value, the over-all return
value will be non-zero.

Each handler can perform further lookup however it likes.  It may
just compare the “key” against a number of supported keys, or it might
perform a lookup in a key-table.  One particularly useful approach is
to look up all commands with a prefix matching the key and call all of
them in order until one returns a non-zero value.  This can be used to
allow multiple handlers to register for a service where each will
handle different instances.  For example when choosing a document type
to open a given file, all document types will be tried but some would
be expected to return zero.  e.g. if the file is actually a directory,
everything but the directory document type would reject the request.

Another example worth understanding is the document-display pane
type.  When this receives an event it will handle it directly if it
understands the key, otherwise it will pass it to the document pane.
If that doesn't recognize the event it will continue up the tree from
the document-display pane.

Like document-display, other pane types are free to direct events
elsewhere as appropriate.  The “input” handler takes keystroke events
and redirects them to the current focus pane, and take mouse events
and redirects them to the pane with the greatest 'z' depth which cover
the mouse location.


Core Extensions
===============

These are the basic common objects which can (I hope) be used to build
a rich document editor.  There need to be lots of extensions of course
to make them useful.  The current extensions that are available include:

Text Document
-------------

A text document stores text in various linked data structures designed
to make simple edits easy and to support unlimited undo/redo.  There
are a number of allocations, and a list of “chunks” which each
identify a start and end in one of those allocations.  Edits can
add text to the last allocation, can change the endpoints of a chuck,
and can insert new chunks or delete old chunks.

Each chunk has a list of attributes each with an offset into the allocation.

Directory Document
------------------

A directory document contains a list of directory entries from a
directory and provides a variety of attributes for each entry.  The
directory can be re-read at any time with incremental changes made to
the document.

Documents Document
------------------

There is typically one pane of this type and it registers an
“attach-doc” handler with the root pane to get notified when documents
are created.  It will reparent the document that that it becomes a
child of the “Documents” pane.  Then all documents can be found in the
list of children.

The “documents” pane presents as a document which can be viewed and
appears as a list of document names.  Various keystroke events allow
documents to be opened, deleted, etc.

Ncurses Display
---------------

The “ncurses” display can draw text on a terminal window and can set
various attributes such as colour, bold, underline etc.  It also
receives keyboard and mouse input and sends “Mouse-event” or
“Keystroke” command up to the input manage.

Line-Renderer
-------------

The line renderer is designed to work with any document that presents
as a list of lines.  Lines that are wider than the pane can either be
truncated (with side-scrolling) or wrapped.  The line renderer moves
back and forwards from the cursor “point” to determine which lines
should be drawn and sends a “render-line” command to get the
displayed text for those lines.

The “text” document provides direct support for “render-line”, but
needs a better way of provides stable limited-sized lines when at file
doesn't contain as many newline characters as we would like.

Attribute Format Renderer
-------------------------

The attribute formatter is given a format for each line into which it
interpolates attributes from each element in the document.  These
lines are provided in response to “render-line” requests so that if
the line-render and the attribute formatter are both stacked on a
document, then the attributes of elements in the document can be
easily displayed.

The format specification is found by requesting the attribute
“Line-format” from panes starting at the focus and moving towards the root.

Completion Render
-----------------

The “completion” render is a filter.  In response to a “render-line”
call it calls “render-line” on its parent and only returns lines that
start with a given prefix.  It can also add highlights to rendered
text to distinguish the common prefix from the remainder.

A prefix is set by a “set-prefix” command.  The response to this
indicates if the selected lines have a longer common prefix, and if
there is in fact only a single matching line.  This supports the
implementation of filename, document name, command name completion
etc.  To complete from a set of names, you just need to provide a
document which responds to “render-line” with the various options to
choose from.

Hex Render
----------

The HEX renderer provides an alternate “render-line” for a document
which starts each line at a multiple of 16 bytes from the start of the
document, and formats the next 16 bytes as hex and ASCII.  Each
rendered line starts with the byte offset of the start of the line.

Tiler
-----

The “tile” handler takes a pane (typically the root pane of a display)
and divides it up into 1 or more non-overlapping tiles.  Tiles are
grouped in horizonal and vertical stacks.  Tiles can be split, can be
discarded, or can be resized.  Any of these operations may affect other
tiles.

The leaves of the tile tree need to have some other pane attached.  The
tiler doesn't render anything itself, not even borders.  That is left
to the children.

View
----

A “view” draws borders around a pane and provides a child pane which
is slightly smaller to allow for those borders (so it should probably
be called “border”).

The borders can contain scroll-bars, a document name, or other
information provided by child panes.

Popup manager
-------------

The popup manager places a small window with an elevated “z” value
somewhere relevant on the display and can provide a simple text document
for text entry - or can use a provided document.  Various key strokes
are captured to allow the popup to be aborted, or to send the content
of the mini document to the originating pane.

The popup requests notifications from that pane so that if it is
closed, the popup automatically closes too.

Line-Counter
------------

The line-counter uses the model described earlier of placing marks
every few hundred lines in a document and using them to expedite line
counting.  This currently isn't implemented as a pane.  I wonder if it
should be.

Keymap
------

“Keymap” allows both global and local keys (or arbitrary commands) to
be defined.  The global mappings are handled at a pane which must be
stacked before the tiler.  A pane to handle local mappings is added on
demand at the current focus pane.

Search
------

“search” is like line-counter in that it provides a global command
rather than a pane.  This command can perform a reg-ex search through
a document.  Currently it only searches the per-element characters, so
it isn't useful on directory listings.  It should be extended to work
with the results of “render-line”.

Messageline
-----------

“Messageline” trims the bottom line off a pane (providing a pane which is
slightly smaller) and will display messages in this pane until the
next keyboard command.

Input
-----

A pane of this module stores some state related to the current input
context, including a modifier prefix and a repeat count.

When a “keystroke” command is received the prefix is added to the key
and this is sent as a command to the focus.  The command includes the
repeat count, which gets cleared.

Commands are provided to set a new prefix or repeat count.  So for
example “Meta-1” might multiply the repeat count in the command by 10,
add 1, and then ask “input” to set that as the new repeat count for
the next keystroke.

Emacs Mode
----------

This provides a set of named commands which can be given to “keymap”
as a global key map.  In provides a number of emacs-like bindings.

Python Interface
----------------

This module allows python code to be run, provide an interface to
panes, marks, and commands, and allows commands to be called on a
given pane.  It also allows commands to be defined in python that can
be called from other modules just like any other command.  It is, or
will be, a complete two-way interface between python and other
languages to access the core edlib functionality.


Pygtk Display
-------------

This is a display module written in python and using pygtk for
drawing.

When a “text” or “clear” request is made on a pane, the module
allocates a pixmap (arranging for it to be destroyed when the pane is
closed) and performs the drawings there.  When a refresh is required,
the various pixmaps are combined and drawn to the target window.

Variable with fonts are supported as are images.  An image is
typically the only thing drawn in a pane, so sub-panes must be used to
draw images within a document.

libEvent
--------

edlib needs an event loop to wait for input, capture signals, and run
tasks.  Any module can register an event loop by registering handlers
for various “event:*” events with the root pane.  When pygtk is being
used, the glib event loop must be used.  Otherwise some other event
loop is needed.  To this end, the libevent module registers a
low-priority set of event handler which use libevent.

I'm not entirely happy about this arrangement.  In particular I would
like to be able to have multiple event loops running in separate
threads.  So expect things to change here.


TO-DO
=====

There is still so very much to do.  At time of writing a lot of things
work and I can load and save files with filename completion, and I can
browse directories and search in text files.  This is encouraging but
it is barely a start.

This a list of just some of the things I want to work on soon.  You
might noticed that the above texts might suggest that some of them are
done already.  In those cases I was being a little ahead of myself above.

- The “complete” popup should be positioned above/below the file name,
  not over the top of it.  And typing should increase/decrease the
  prefix.

- render-lines should always re-render the line containing point, so
  the location of “point” can affect the rendering.

- allow searching in the rendered output as well as in the document

- support case-insensitive search and literal (non-regex) searchs.

- Create an append-only limited size document for a log of messages
  and a log of keystrokes.

- use above to allow keyboard macros.

- create a “mmap” document type so I can edit a block device without
  reading it all in.

- create a “reflection” document so I can view the internal data structures.

- create clean threading interfaces so that I can have different event
  loops in different threads and suitable locking so commands can be
  called from any event loop.

- Support write-file (providing a file name) - currently I only save
  to the file I loaded from.

- improve format options for status line.

- autosave

- cut/copy,  paste

- lots of work on pygtk interface

- allow a second (and more) ncurses display to be created.

- improve ncurses code for choosing colours.

- auto-indent
- auto-wrap
- c-mode parsing
- color highlights
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

I only recently discovered “emacsclient -t”.  I think I like it.  I
certainly want edlib to have a “server” mode and to be able to perform
editing in arbitrary terminal windows where I do other work.

calculator in floating pane
---------------------------

I very often use “bc” for hex/decimal conversion etc.  But it is a
little clumsy.  I want an easy calculator on my desktop with base
conversion.  I'd like to use edlib.  I imagine a small pop-up
appearing which automatically converts whatever I type.

spread sheet
------------

Many years ago I started writing a spreadsheet program in emacs-lisp.
The document was a “LaTeX” document with specially marked “tabular”
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
variables as possible.  If I am in an “if” statement in a “for” look,
then the loop header and the if condition would be displayed if at all
possible.

hexedit
-------

This is something that is very clumsy with emacs, and should be very
easy with edlib.  A “hexedit” view shows the hex value of each byte in
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
file into a buffer, which wouldn't be good.  If a “less” command could
give the filename to edlib and let it display, that might be nice.
