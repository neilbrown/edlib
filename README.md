<meta HTTP-EQUIV="Content-Type" CONTENT="text/html; charset=utf-8">

<!--
# Copyright Neil Brown ©2015-2019 <neil@brown.name>
# May be distributed under terms of GPLv2 - see file:COPYING
-->

Edlib - a library for building a document editor
==============================================

Edlib is an extensible document editor.  It is inspired in part by
emacs, both by its strengths and its weaknesses.

emacs provides a programming language — E-lisp — for configuring and
extending the editor.  Edlib doesn't.  It allows various pre-existing
languages to be used to configure and extend the editor.  It does
this by providing a library of core editing tools and providing
bindings to various languages.

At least, that is the plan.  At time if writing, edlib only provides
bindings for C and Python.  Other languages should be fairly easy.

The particular value-add of edlib over emacs (apart from the obvious
“NIH” issues) is that both document storage and document rendering are
fully extensible.  A document is not always a text buffer, it could
also be a mem-mapped files, a directory, or an internal data
structure.
Any document can have multiple views, and each view can
show very different things: scriptable code is run whenever
rendering is required.  This should make implementing documents with
non-trivial structures a lot easier.

Edlib is designed to have well defined abstractions that can be
exported to various languages for them to manipulate.  They include
primarily commands, panes, and marks, and also attributes, documents,
displays, events, and keymaps.

Commands
--------

Commands are the single mechanism by which control is transferred from
one part of the editor implementation to another.  They provide the
only way for code in one implementation language to invoke code
written in another language, and they are the preferred way to
interact between different modules even in the same language.

All commands receive the same types of arguments and produce an integer
result.  The arguments include two panes (“home” and “focus”),
two marks (“mark” and “mark2”),
three strings (“key”, “str”, “str2”),
two numbers (“num” and “num2”),
a co-ordinate pair (“x”, “y”) and two commands (“comm1” and “comm2”).
Extra result values can be effected by passing them to a call to
the “comm2” argument - i.e the command can be used as a call-back.

Each “pane” has a dedicate command which handles messages sent to
the pane, as will be described later.  Commands can also be passed
to other commands, which can either call them directly (like the
call-back mentioned above) or store them for later use, or
both.

Three of the arguments provided to a command have very special
meanings.  One of the strings, known as “key”, identifies what action
should be performed.  The pane handler will normally use this key to
select some other command to actually handle the message.  Other
commands may ignore the key, or use it however they please.

When a message is sent to a pane and the handler command is called,
the “home” argument is set to the pane that owns the handle, so it acts
a bit like the “self” argument in some object-oriented languages.
One of the primary uses of “home” is to access “home->data” which is a
private data structure owned by the pane.  The command associated with
a particular pane is typically the only code which can understand the data.

Finally the “comm1” argument passed to a command always identifies
exactly the command that is being run.  “comm1” is a pointer to a
structure containing a pointer to the function being called.  This
structure can be embedded is some other data structure which contains
context for the command.  This context might be read-only, to refine
the behaviour of the command, or read/write to provide storage for the
command to use.  For example, the call-back commands described earlier
are normally embedded in a data structure that can store the extra
values to return.

Apart from the return value of zero which indicates “command not
understood”, a command can return a positive result on success or a
negative result indicating lack of success.  Known error codes include:

- Enoarg : missing argument
- Einval : something is wrong with the context of the request
- Efail : request makes sense, but didn't work
- Enosup: request makes sense, but isn't allowed for some reason
- Efalse: Not really an error, just a Boolean status

Panes
-----

A pane combines an optional rectangular area of display with some
data storage and some functionality.  As such it can receive mouse and
keyboard events, can draw on a display, and can send commands to other
panes.

All panes are arranged as a tree with all but the root having a parent
and many having siblings and children.  When a pane represents a
rectangle of display all children are restricted to just that
rectangle or less.  Often a child will cover exactly the same area as
its parent.  In other cases several children will share out the area.

An “event” is a set of arguments to a command which is being sent to
a pane.  Events are often generated at a leaf of the tree of panes
(i.e. a pane with no children).  They travel up the tree towards the
root until they find a pane which can handle them.  That pane (or its
handler command) might handle the event by generating other events.
They will typically start looking for a handler at the same leaf which
is available as the “focus” argument.  For this reason branches of the
pane tree usually have more generic panes closer to the root and more
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
  value will be drawn “over” siblings with a lower “z” value, including
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
changes.  These notifications are effected by calling the pane's command
with a key like “Notify:Close” and with the second pane argument
(the “focus”) set to the pane which is sending the notification.

Documents
---------

A document provides access to whatever data is being edited, or just
being displayed.  There can be multiple implementations for documents
but they all have a common interface.

A “document” is assumed to be a linear sequence of elements each of
which presents as a single character and may have some attributes
associated with it.  For a “text” document, the characters are
typically Unicode characters stored as UTF-8 and the attributes, if
any, are hints for parsing or display.
For a “directory” document, the elements are entries in the directory
and the associated character reflects the type of entry.  The
attributes contain other information such as file name, size,
modify time etc.

A document is represented by a non-display pane.  These panes are
typically collected together as children of a “document-list” pane
which can be asked to add or find documents.  To display a
document, a document-display pane is normally created.  This contains,
in its private data, a reference to the document pane and a “point”
(see below) indicating where changes will happen.  Events that arrive
at the document-display pane will typically be forwarded to the
document, though they maybe be handled directly by the display pane.

Attributes
----------

An attribute is a simple name=value pair, both being strings.
Attributes can be associated with various other objects, including
marks, panes, and elements in a document.  Parsing code can
annotate a buffer with attributes, and rendering code can use these
attributes to guide rendering.  e.g. parsing code can attach
“spelling=wrong” or “spelling=doubtful” and rendering can underline in
red or whatever is appropriate.

Marks and Points
----------------

A “mark” identifies a location in a document.  The location is between
two elements in the document, or at the start or end, and the mark
remains at that location despite any edits that do not affect
neighbouring elements.

Marks come in three different sorts: ungrouped, grouped, and points.
All of these appear in document-order in a linked list, all have a
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
in the visible portion.  A grouped mark also has a reference to an
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
“image-display”.  Displays are typically quite close to the root of the
“pane” tree, but this is not a requirement.

A display is also expected to call “Keystroke” and “Mouse-event”
commands in response to appropriate events.  These will propagate
towards the root and normally hit an input-management pane which will
find the appropriate target leaf, will convert to a full event,
e.g. adding a repeat count or indication of a prefix key, and will
submit the new event at the target.

Keymaps
-------

A keymap is a mapping from command names to commands.  While a
pane hander could use any mapping it likes, the keymap implemented in
edlib has one small advantage in that a range of strings can be mapped to
a command, then exceptions can be recorded.

The handler for a pain typically looks up the passed “key” in a
keymap, locates the target command, and passes control to that command.

Handling Commands
-----------------

Now that we have plenty of context, it is time to revisit commands to
discuss how they are called.  It is possible to invoke a specific
command directly if you have a reference to it but most often a more
general mechanism is used to find the appropriate command.  The most
common mechanism is to pass and "Event" (i.e. a set of arguments) to a
“home” pane.  The handler for that pane and each ancestor will be tried
in turn until a handler returns a non-zero value (i.e. it accepts the event),
or until the root pane has been tried.  Very often the starting home pane
will also be the focus pane so when the two are the same it is not necessary
to specify both.

The other common mechanism is a “notification event” which follows the
"notifier" chain from a pane.  This chain lists a number of panes which
have requested notifications.  When calling notifiers, all target panes have their
handler called and if any return a non-zero value, the over-all return
value will be non-zero.  More precisely it will be the value returned
which has the largest absolute value.

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
understands the key, otherwise if it starts with “doc:” it will pass it
to the document pane.  If that doesn't recognise the event it will
continue up the tree from the document-display pane.

Like document-display, other pane types are free to direct events
elsewhere as appropriate.  The “input” handler takes keystroke events
and redirects them to the current focus pane, and take mouse events
and redirects them to the pane with the greatest 'z' depth which covers
the mouse location.


Core Extensions
===============

These are the basic common objects which can (I hope) be used to build
a rich document editor.  There needs to be lots of extensions of course
to make them useful.  The current extensions that are available include:


Text Document
-------------

A text document stores text in various linked data structures designed
to make simple edits easy and to support unlimited undo/redo.  There
are a number of allocations, and a list of “chunks” which each
identify a start and end in one of those allocations.  Edits can
add text to the last allocation, can change the endpoints of a chuck,
and can insert new chunks or delete old chunks.

Each chunk has a list of attributes each with an offset into the allocation,
so they each apply to a single character, though are often interpreted
to apply to follow characters as well.

Directory Document
------------------

A directory document contains a list of directory entries from a
directory and provides a variety of attributes for each entry.  The
directory can be re-read at any time with incremental changes made to
the document.

Documents Document
------------------

There is typically one pane of this type and it registers an
“doc:appeared-” handler with the root pane to get notified when documents
are created.  It will reparent the document so that it becomes a
child of a separate “collection” pane.  Then all documents can be found in the
list of children.

The “documents” pane presents as a document which can be viewed and
appears as a list of document names.  Various keystroke events allow
documents to be opened, deleted, etc.

Rendering virtual document
--------------------------

Working directory with the "directory" or "documents" document is sometimes
a bit awkward as a mark can only point to a while directory entry or
document.  When these are displayed one-per-line it isn't possible to move 
the cursor within that line, which can feel strange, particularly if the line
is wider than that display pane.  Also selecting  an copying
text cannot select part of a line.

To overcome these problems, a "rendering" document can be layered over
the main document.  It presents a virtual document which contains
all the character that are use to display (to render) the underlying
document.  This then "feels" more like a regular document and can respond
to select and copy just like a text document.

Multipart document, and "crop" filter
-------------------------------------

Multipart is another virtual document, which appears to contain
the content of a sequence of other documents.  This allows a sequence
of documents to appear to be combined into one.  This can co-operate
with the "crop" filter which limits access to a given document to the
section between two marks.  By combining multipart and crop,
one document can be divided up and re-assembled in any order, or
parts of multiple documents can be merged.

Email document; base64, qprint, utf8, rfc822header filters
----------------------------------------------------------

The Email document handler uses crop and multipart and other tools to
present an email message as a readable document.  The different
parts of an email message (header, body, attachments) are identified
and cropped out with appropriate filters attached.

Notmuch email reader
--------------------

"notmuch" as a email indexing and management tools.    The notmuch
email reader provides one document which displays various
saved searches with a count of the number of items, and another
document type which can show a summary line for each message
found by a given search.  They work with the email document
pane to allow reading and managing an e-mail mailbox.

Ncurses Display
---------------

The “ncurses” display can draw text on a terminal window and can set
various attributes such as colour, bold, underline etc.  It also
receives keyboard and mouse input and sends “Mouse-event” or
“Keystroke” command up to the input manage.

There can be multiple ncurses displays, each attached to a different terminal.

Pygtk Display
-------------

This is a display module written in python and using pygtk for
drawing.

When a “text” or “clear” request is made on a pane, the module
allocates a pixmap (arranging for it to be destroyed when the pane is
closed) and performs the drawings there.  When a refresh is required,
the various pixmaps are combined and drawn to the target window.

Variable width fonts are supported as are images.  An image is
typically the only thing drawn in a pane, so sub-panes must be used to
draw images within a document.

Lines-Renderer and render-line filter.
--------------------------------------

The lines renderer is designed to work with any document that presents
as a list of lines.  Lines that are wider than the pane can either be
truncated (with side-scrolling) or wrapped.  The lines renderer moves
backwards and forwards from the cursor “point” to determine which lines
should be drawn and sends a “render-line” command to get the
displayed text for those lines.

There is “render-line” pane type which provides the “render-line”
function for a simple text document.  It looks for particular
attributes in the document and on marks in the document and interprets
them to allow highlighting different parts of the text.  It returns a
line with markup which the line renderer understands.

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
start with a given prefix, or contain a given substring.  It can also
add highlights to rendered text to distinguish the common prefix from
the remainder.

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

Presentation renderer
---------------------

The presentation rendering accepts a text document and interprets
it as describing pages of a presentation in a language similar
to MarkDown.  It generates a marked-up rendering the various
lines of text which are given to the lines renderer to produce
a single page of the presentation.

There is a partner "markdown" mode which interacts with a presentation
pane and can ask it to move forward or backward one page, or  to
redraw some other arbitrary page.

Tiler
-----

The “tile” handler takes a pane (typically the root pane of a display)
and divides it up into 1 or more non-overlapping tiles.  Tiles are
grouped in horizontal and vertical stacks.  Tiles can be split, can be
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
counting.  This is implemented as a pane which attaches directly to
the document pane.

Keymap
------

“Keymap” pane allows both global and local keys (or arbitrary
commands) to be defined.  The global mappings are handled at a pane
which must be stacked before the tiler.  A pane to handle local
mappings is added on demand at the current focus pane.

Search
------

“search” provides a global command rather than a pane.  This command
can perform a reg-ex search through a document.

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

Shell-command
-------------

This pane makes it easy to run a command in the background and capture
the output in a text document, which can then be displayed like
any other document.

make/grep
---------

This allows make or grep to be running with the output captured and parsed.
A simple keystroke then causes the editor to open a file mentioned
in the output, and to go to the identified line.

Viewer
------

The viewer pane suppresses any commands that would modify the document,
and repurposes some of them to make it easier to move around a document
being viewed - so 'space' pages forward, and 'backspace' pages backwards.

history
-------

A history pane stores data a bit like a document, but provides
access in a different way.  Individual lines can be recalled and
new lines can be appended.  It makes it easy to provide a history
of lines of text entered for some purpose, such as running a shell
command or an editor command selected by name.

copybuf
-------

Similar in principle to "history", a copybuf pane can store a series
of arbitrary slabs of text.  It is used to provide copy/paste functionality.

server
------

The server pane listens on a socket for request to open a file,
or to create an ncurses pane on the current terminal.
It also reports to the request when the file has been edited,
or when the ncurses pane is closed.

Emacs Mode
----------

This provides a set of named commands which can be given to “keymap”
as a global key map.  In provides a number of emacs-like bindings.


C/Python mode
-------------

This pane capture various editing command and tailors them
to suite editing C or Python code.  It helps with correct
indenting, highlight matching brackets, and will eventually do
a lot more.

Python Interface
----------------

This module allows python code to be run, provides an interface to
panes, marks, and commands, and allows commands to be called on a
given pane.  It also allows commands to be defined in python that can
be called from other modules just like any other command.  It is a
complete two-way interface between python and other languages to
access the core edlib functionality.

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

Next steps
----------

The [TO-DO list](DOC/TODO.md) is now a separate document.


