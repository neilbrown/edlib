
# Documents: marks and the viewing pane

In many ways a document is just another pane: it can store state and has
a keymap which allows it to respond to various messages.  As a document
there are a set of message keys that it should respond to, all of which
start with the prefix "doc:".  To assist with this many of the expected
key are provided default implementation via a predefined keymap which is
chained after the main keymap for the document pane.

A document will often be display in multiple display panes so there
naturally needs to be a way for the one document to be attached to these
displays.  For that purpose there is a document viewing pane which can
stacked on a display pane an which redirects relevant messages to the
document.  This is described in more detail below.

A document is intrinsically a linear object.  Typically it is a linear
sequences of characters and that sort of document is well supported.
But characters being content is only a common convention - linearity is
a fundamental property relating to the use of marks.

## Marks in a document

FIXME talk about each mark having a pointer and a number, and for some
documents that number can be shared with another pane.

Every document must be a linear sequence of something - typically
characters.  While examining or modifying that sequence we must have
some way to refer to a particular location in the sequence and for that
edlib provides marks.  A mark points to a position in the sequence not
to an object in the sequence.  So each mark is either at the start, at
the end, or between two objects.  It can be moved forward or backward
through the sequence and various commands expect one or two marks in the
message they receive so they can report on or modify the content between
or near those marks.

There is a total ordering of all marks for a given document.  Even if
two marks refer to the same location they will be ordered.  Often this
subtlety is irrelevant, but some times it must be carefully understood.

There are three different sorts of marks that serve different purposes
within the editor.  The simplest are "ungrouped" marks.  These are often
used as temporary pointer when working on a document.  Any mark can be
"duplicated" to create an ungrouped mark at the same location.  That
ungrouped mark can then be moved around to explore the document without
disturbing any other marks.

Secondly there are grouped marks.  Any pane which has access to a
document can request that a group (also known as a "view") be allocated,
which is just a small integer.  It can then create new marks in that
group and move them wherever required.  All marks in a group are linked
together so that it is easy to find the previous and next mark in a
group, or the first or last.

The pane that created a mark group is the only one that can create marks
in that group.  When the pane is closed, all the mark groups created by
that pane a destroyed together with all the marks in them.

Groups marks can have arbitrary data associated with them.  For example
and pane might you a group of marks to remember interesting locations
when parsing a document, and might record details of the parse in each
mark.  The pane could record those details as attributes.  Sometimes
that is easier, sometimes having arbitrary data is easiest.  Ungrouped
marks can store arbitrary data too, but this is rarely useful.

Finally there a special marks known as "points".  Points are
typically used to hold the location in a document where editing is
happening.  A pointer is linked in which all the different mark groups
so it is efficient to find nearby marks for any group if you start with
a pointer.  If instead you start with some other mark not in the same
group, then some searching may be required to find a specific nearby
mark.  When you start with a pointer you can immediately find a nearby
mark of any group.  Pointers cannot store extra arbitrary data.

Calling "mark_watch(mark)" on a mark causes it to have a flag set so
that certain movements of marks cause a notification to be sent to the
pane which owns the mark - usually a document.  When a notification is
sent, the "watched" flag is cleared on the mark that was moved (if it
was already set), so the notification handler will need to call
mark_watch(mark) again if it needs to see the next movement.

Whenever a watch mark is moved, whether by a movement command
("doc:char", "doc:set-ref" etc) or by moving it to the location of
another mark with mark_to_mark(), a "mark:moving" notification is sent
to the owner of the mark.  The primary mark in the notification is the
mark that was moved.  The focus is the mark owner.

Whenever a mark is moved with mark_to_mark() to a mark which is watched,
a "mark:arrived" notification is sent to the mark owner.  In this case
the destination mark is passed to the notification as the secondary mark
(mark2).  If mark_to_mark() is called with both marks being watch, both
notifications are sent.

### working with marks

The following ..

- mark_valid(mark)
- mark_dup(mark)
- mark_dup_view(mark)
- mark_free(mark)
- mark_watch(mark)
- mark_next(mark)
- mark_prev(mark)
- mark_reset(pane, mark, int)
- mark_to_end(pane, mark, int)
- mark_to_mark(mark, target_mark)
- mark_same(mark1, mark2)
- mark_attr

- mark_at_point() - should be mark_dup...

- vmark_next, vmark_prev, vmark_matching, vmark_first, vmark_last
  vmark_at_or_before vmark_new


- mark_ordered_or_same() mark_ordered_not_same()

Some messages:

- doc:add-view
- doc:del-view
- doc:vmark-get
- doc:vmark-prev
- doc:vmark-new

## Content in a document

Chars first - then other things behind the chars...

The sequence of things that comprise a document are nominally UNICODE
characters but this may only be a shallow appearance.  As marks are
moved forwards or backwards around the document, numbers are reported
and these number are either UNICODE characters, optionally with bit 21
set, or 21 1 bits indicating end-of-file (either end).  In any document
the object can have arbitrary attributes attached and these may allow
the objects to present


	key_add(doc_default_cmd, "Notify:Close", &doc_view_close);
	key_add(doc_default_cmd, "get-attr", &doc_get_attr);
	key_add(doc_default_cmd, "doc:set-name", &doc_set_name);
	key_add(doc_default_cmd, "doc:destroy", &doc_do_destroy);
	key_add(doc_default_cmd, "doc:drop-cache", &doc_drop_cache);
	key_add(doc_default_cmd, "doc:closed", &doc_do_closed);
	key_add(doc_default_cmd, "doc:get-str", &doc_get_str);
	key_add(doc_default_cmd, "doc:get-bytes", &doc_get_str);
	key_add(doc_default_cmd, "doc:content", &doc_default_content);
	key_add(doc_default_cmd, "doc:content-bytes", &doc_default_content);
	key_add(doc_default_cmd, "doc:push-point", &doc_push_point);
	key_add(doc_default_cmd, "doc:pop-point", &doc_pop_point);
	key_add(doc_default_cmd, "doc:attach-view", &doc_attach_view);
	key_add(doc_default_cmd, "doc:attach-helper", &doc_attach_helper);
	key_add(doc_default_cmd, "Close", &doc_close_doc);

	key_add(doc_default_cmd, "doc:word", &doc_word);
	key_add(doc_default_cmd, "doc:WORD", &doc_WORD);
	key_add(doc_default_cmd, "doc:EOL", &doc_eol);
	key_add(doc_default_cmd, "doc:file", &doc_file);
	key_add(doc_default_cmd, "doc:expr", &doc_expr);
	key_add(doc_default_cmd, "doc:paragraph", &doc_para);

	key_add_prefix(doc_default_cmd, "doc:char-", &doc_insert_char);
	key_add_prefix(doc_default_cmd, "doc:request:",
		       &doc_request_notify);
	key_add_prefix(doc_default_cmd, "doc:notify:", &doc_notify);
	key_add_prefix(doc_default_cmd, "doc:set:", &doc_set);
	key_add_prefix(doc_default_cmd, "doc:append:", &doc_append);

## The document link pane

As a document might appear in different display windows it must be
possible for a stack of panes responsible for the display to have some
linkage to the document.  It cannot be a simple parent linkage because
the display panes would then need two parents, one being the document
and one being the output device such as an x11 window or ncurses
terminal.  To provide this linkage we have the doc-view pane.

As well as providing the necessary linkage the doc-view pane owns a
point which will normally be used as the editing cursor.  It also
supports a set of 4 general purpose marks that be be used for selecting
regions and similar tasks.  As these must be different for each view,
but are likely to be needed for all views, keeping in the doc-view pane
works well.

The doc-view pane responds to all messages with a "doc:" prefix.  A few
are handled directly and the rest are passed on to the document.  If the
document doesn't handle them, then they are passed to the parent of the
doc-view pane as would normally happen.

When a "doc:" message is passed on to the document, if message doesn't
contains a valid first mark, the point is passed as the first mark.
This ensure documents will always see a first mark for "doc:" messages.

The doc-view pane also redirects "get-attr" messages to the document so
that a the doc-view pane appears to have all the attributes that the
document pane has.

The doc-view pane gets a notification when the document is destroyed and
closes itself in that case, so the whole display stack above it will
close.

Messages that a doc-view pane handles directly include:

 - Move-Char
 - Move-Line
 - Move-View
 - doc:point
 - doc:dup-point
 - Replace
 - Move-to
 - Abort

### attaching a doc-view pane

Attaching a doc-view pane involves sending the message
"doc:attach-view" to the document to be attached, passing the pane to be
the parent as the focus pane.  This message has a default implementation
provided to each document, so every document knows how to attach a
viewer.

The first string argument can specify a "type" which determines how to
display the document, which is important for documents that can be
displayed in different way. If no type is given, "default" is used.

The "type" is used to attach extra panes over the doc-view pane as this
is often wanted.  Firstly a generic viewing pane is attached with the
"attach-view" message.  This typically support borders and scroll bars
around the document view, possibly with a mode line an/or title line.

Next the renders is requested from document attribute "render-TYPE", or
failing that from "render-default".  If that exists then the
"attach-render-RENDERER" message is sent which should attach a pane that
understand the requested style of renering, often "text" for simple text
documents.

Finally, the "view-TYPE" attribute, or "view-default" is requested. This
may be a comma separated list of name and each will be attached by
sending the message "attach-VIEW".  These pane normally make small
modifications to behaviour such as highlighting spelling errors or
syntax components of a parseable document, or enabling paragraph-fill or
auto-indent or similar.

Each of these "attach-" messages are typically handled the root pane on
which various modules can register handlers.  The all expect a callback
command argument and will call it with the newly created pane as the
"focus" argument.  "doc:attach-view" also expects a callback command and
will pass the final leave pane to this command before returning.

As a special case if the type pased to "doc:attach-view" is "invisible"
then none of these extra view pane are attached.  The doc-view pane is
installed and that is passed to the callback.  The caller much then add
anything else it needs.

### special handling for point:

The doc-view pane registers a "mark:moving" handler so that it gets
callback whenever the point moves.  It uses this to arrange that
whenever the display is refreshed after the point has moved, its
"Refresh:view" handler is called.

The Refresh:view handler sends a "view:changed" message to the leaf
where refresh is happening for bother the previous point (which doc-view
takes a copy of) and the new point.  This ensures that the cursor is
redrawn.

If the point has the "selection:active" attribute set to a positive
integer, then a single "view:changed" message is sent for with both the
old and new values of the point.  This ensures that any change in the
selection will be redrawn when the point moves.










