<meta HTTP-EQUIV="Content-Type" CONTENT="text/html; charset=utf-8">

<!--
# Copyright Neil Brown ©2015 <neil@brown.name>
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
“command” argument.

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
dedicated 'pane' command or as a named command.  One of the primary
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
detected.

Panes
-----

A pane combines a rectangular area of display with some
functionality.  As such it can receive mouse and keyboard events,
can draw on a display, and can send commands to other panes.  As
previously mentioned, panes can also store module-specific data.

All panes are arranged as a tree with all but the root having a parent
and many having siblings and children.  The “root” pane does not
correspond to any display, but is still implemented as a pane for
consistency.  The children of the root are the different display
windows, each of which can use a distinct display technology such as
“ncurses” or “gtk” or “qt” or “fbdev” or anything else.  The root pane
also has some “virtual display” panes which are used to gather other
special panes - see “Documents” below.

As well as a dedicated command and private data, each pane has:

- x,y co-ordinates together with width and height.  The co-ordinates
  are relative to the parent, and by recursive addition can be made
  absolute.
- a 'z' value which indicates display priority with respect to
  siblings.  When siblings overlap, the sibling with the higher “z”
  value will be draw “over” siblings with a lower “z” value, including
  all the children of that sibling (independent of their z value).
- a selected child referred to as the 'focus'. Keyboard input at a
  display is forwarded down the chain of focus links until it reaches
  a leaf pane.  This is where handling of the keystroke starts.
- a set of  'damaged' flag which record if any changes have been made
  which might affect the display.
- an arbitrary set of attributes with assigned values.

Each pane may also request notifications from other panes.  These
include, but are not limited to, a notification when the pane is
destroyed and a notification when a document attached to the pane
changes.  This notifications are effected by calling the panes command
with a key like “notify:close” and with the second pane argument
(known as 'focus') set to the pane which is sending the notification.


Documents
---------

A document provides access to whatever data is being edited.  There
can be multiple implementations of a document but they call have a
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

Documents cannot be accessed directly, but must be accessed through a
“document access” pane.  There can be several panes which access the
one document though there is always one special pane in a “virtual”
display.  There panes in this virtual display contain all of the
active documents so a list of documents can be obtained by iterating
over the children of this virtual display.  There is a singleton
document types called “*Documents*” which lists all active documents.
By opening a pane on this document, the set of active documents can be
viewed and modified.

It is (will be?) possible for different panes to provide slightly
different access styles to the same document.  For example one pane
might present a file as a sequence of bytes while another presents the
same file as a sequence of unicode characters.  Similarly one
interface may accept arbitrary insert/delete operations while another
converts all changes to over-writes (maybe).


Attributes
----------

An attribute is a simple name/value pair, both being strings.
Attributes can be associated with various other objects, including
mark, panes, and elements in a document.  Parsing code can
annotate a buffer with attributes, and rendering code can use these
attributes to guide rendering.  e.g. parsing code can attach
“spelling=wrong” or “spelling=doubtful” and rendering can underline in
red or whatever.

Currently most attributes have to be stored using a particular
implementation of attribute storage.  I'm not sure I want that in the
longer term.  I'm considering having the pane command support a
"attribute:get" command though there are still unresolved issues with
that idea.

Marks and Points
----------------

A “mark” identifies a location in a document.  The location is between
two elements in the document, or at the start or end, and the mark
remains at that location despite any edits that do not affect
neighbouring element.

Marks come in three different sorts: ungrouped, grouped, and points.
All of these appear in document-order a single linked list, all have a
sequence number in the list so ordering-tests are easy, and each can
have a set of attributes attached.

As well as identifying a location in a document, a mark can identify a
location in the display of that document location.  When a single
element in a document is displayed using multiple characters (as for
example a directory entry might be), the “rendering position” or
“rpos” can record where in those multiple character the mark really
belong.  I'm not yet sure how useful this is, but it seems like a good
idea.

An ungrouped mark has no property beyond the above.  A grouped marked
is included in a second linked list with all the other marks in the
same group.  This group is owned by a specific pane and keeps
information relevant to the task of that pane.  A pane responsible for
rendering part of a document might have marks identifying the start
and end of the visible portion, and maybe even the start of teach line
in the visible portion.  An ungrouped mark also has a reference to an
arbitrary data structure which is understood by the pane which owns
the group.

A “point” is a special grouped-mark which is included in all of the
other lists of grouped marks.  This is achieved by using the external
reference to hold an auxiliary data structure which is linked in to
all of the lists.  Every pane which views a document owns a point.
This point is usually where changes to the document happen.  When the
notification mechanism mentioned earlier tells other panes of a chance
to the document, the point where the change happened is also
reported.  From this point it is easy to find and update nearby marks
of any mark-group.

An example use is to have a group of marks which are used to track line
numbers.  “line-count” marks are placed every 500 lines (or so) with
an attribute recording exactly how many lines between this and the
next “line-count” mark.  When a change happens, the recorded line
count is cleared.  When a line count or line number is needed, the
list of “line-count” marks is walked from the start.  If any has its
count cleared, the lines in that section are counted and the record is
updated.  Otherwise all that is required is simply adding up a few
numbers.

Marks could be used by a parser to identify key locations which would
allow a renderer to find the important content quickly if it was only
rendering a partial view - such as the headings in outline mode.


Displays
--------

A “display” is just a pane which can create an image somehow, and
responds to commands like “draw:clear” and “draw:text”.  Displays are
typically just below the root of the 'pane' tree, but this is not a
requirement.

A display is also expected to call "Keystroke" and "Mouse-event"
commands in response to appropriate events.

Keymaps
-------

A keymap is a mapping from command names to commands.  In many cases a
similar data structure such as a Python “dict” could be used.  The
keymap implemented in edlib has one small advantage in that a range of
strings can be mapped to a command, then exception can be recorded.

Keymaps are a bit like attributes in that the concept is valuable but
it isn't yet clear how central a particular implementation should be.

Handling Commands
-----------------

Now that we have plenty of context, it is time to revisit commands to
discuss how they are called.  It is possible to invoke a specific
command directly but most often a more general mechanism is used to
find the appropriate command.  There are three such mechanisms which
each search the pane tree testing different commands until one accepts
the given “key” name.  They each find a starting pane in a different
way, and then try that pane and then its parent and so on up the tree
until a pane accepts the command (returning non-zero) or until the
root is reached.

There are three ways to choose the starting point, which is included
in the arguments to every command called as the “focus” point.

The first way is for the caller to explicitly set it.  This is not a
very common approach but is needed when a pane acts like a “filter”.
The handler for a particular command might call that command on the
parent, then process the result in some way and return that result to
the caller.  This can also be used when the appropriate “focus” has
already been found.  If the handler for some command wants to call
some other command it will typically pass the “focus” of the first as
the “focus” to start searching for the second.

The second way is to search out to a leaf of the tree following the
“focus” links in each pane.  The search typically starts at a
“Display” pane where a keystroke is generated,

The third way is to search out to a leaf of the tree which contains
some particular x,y co-ordinate.  The pane with the highest 'z' value
which contains the co-ordinate will be chosen.  This is typically used
to find the correct handler for a mouse event.

Core Extensions
===============

These are the basic common objects which can (I hope) be used to build
a rich document editor.  There need to be lots of extensions of course
to make them useful.  The current extensions that are available include:

Text Document
-------------

A text document stores text in various linked data structured designed
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

Ncurses Display
---------------

The 'ncurses' display can draw text on a terminal window and can set
various attributes such as colour, bold, underline etc.  It also
receives keyboard and mouse input and sends 'Mouse-event' or
'Keystroke' commands ... somewhere.

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
rendered line start with the byte offset of the start of the line.

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

The popup manager places a small window with an elevated 'z' value
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
slightly smaller) and will displays messages in this pane until the
next keyboard command.

Input
-----

A pane of this module store some state related to the current input
context, including a modifier prefix and a repeat count.

When a “keystroke” command is received the prefix is added to the key
and this is sent as a command to the focus.  The command includes the
repeat count, which gets cleared.

Commands are provided to set a new prefix or repeat count.  So for
example “Meta-1” might multiply the repeat count in the command by 10,
add 1, and then ask 'input' to set that as the new repeat count for
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

This is an under-development display module written in python and
using pygtk for drawing.

When a “text” or “clear” request is made on a pane, the module
allocates a pixmap (arranging for it to be destroyed when the pane is
closed) and performs the drawings there.  When a refresh is required,
the various pixmaps are combined and drawn to the target window.

There is currently no attempt to handle variable-width fonts.  That
must come later.


TO-DO
=====

There is still so very much to do.  At time of writing a lot of things
work and I can load and save files with filename completion, and I can
browse directories and search in text files.  This is encouraging but
it is barely a start.

This a list of just some of the things I want to work on soon.  You
might noticed that the above texts might suggest that some of them are
done already.  In those cases I was being a little ahead of myself above.

- Generic notifications between panes: currently only a document can
  notify a pane.

- change commands that return a non-integer to do so by calling
  the call-back command.

- The “complete” popup should be positioned above/below the file name,
  not over the top of it.  And typing should increase/decrease the
  prefix.

- render-lines should always re-render the line containing point, so
  the location of 'point' can affect the rendering.

- allow searching in the rendered output as well as in the document

- support case-insensitive search an literal (non-regex) search.

- Create an append-only limited size document for a log of messages
  and a log of keystrokes.

- use above to allow keyboard macros.

- create a 'mmap' document type so I can edit a block device without
  reading it all in.

- create a 'reflection' document so I can view the internal data structures.

- Generalize the “event loop” interface so that glib events can be
  used when gtk is active.

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

- determine how best to support variable-sized fonts, both
  non-constant-width and requested font size changing.

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

I very often use 'bc' for hex/decimal conversion etc.  But it is a
little clumsy.  I want an easy calculator on my desktop with base
conversion.  I'd like to use edlib.  I imagine a small pop-up
appearing which automatically converts whatever I type.

spread sheet
------------

Many years ago I started writing a spreadsheet program in emacs-lisp.
The document was a 'LaTeX' document with specially marked “tabular”
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
 
