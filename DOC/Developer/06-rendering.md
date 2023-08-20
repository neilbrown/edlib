# Drawing the document

edlib allows panes to have substantial control over how the editor
appears: a different display pane can result in a completely different
appearance.  This requires a flexible model for handling display refresh
and detecting changes that need to be reflected in the display.

When the event subsystem finds that there is nothing mode to do, it
returns and the main loop calls "refresh_pane()" on the root pane.  This
triggers all drawing actions.  Each pane have a number of flags to
indicate that they have been "damaged" and so some redraw might be
needed.  The refresh procedure will only call the various handlers on a
pane when it appears to be damaged.  This allows panes that don't need
any change to be bypassed quickly.

## The refresh process

The refresh process happens as a sequence of three steps.  Normally
after all of these steps nothing will be damaged.  There can
occasionally be a genuine need for a later stage to cause damage to
require an earlier stage to be repeated, so the whole sequence might be
repeated.  If it appears to need to be repeated more than 5 times, it is
assumed there is a bug in the code: the process aborts and a warning is
given.

### Refresh size

The first step in the sequence ensure all panes have he right size.  If
one pane changes size for any reason, such as a top level window being
resized, other may need to adjust to this change.  A pane that notices
its size has changed, or might need to change, sets DAMAGED_SIZE with
the pane_damage() interface.

The refresh sequence will send a "Refresh:size" message to any pane with
DAMAGED_SIZE set.  If this message is not handled, all children with a
depth of zero will have their size adjusted to match the parent, which
will cause DAMAGED_SIZE to be set on them.  Any pane with a larger depth
will just have DAMAGED_SIZE set.

When pane is thus requested to handle a resize, the sequence starts
again from the root looking for panes that need to handle a resize.  It
should quickly deal with all pane. It finds that it needs to resize more
than 1000 panes, it assumes that some pane keeps setting DAMAGED_SIZE on
itself, and it aborts the loop.

### Refresh view

Once all panes are the right size, we need to ensure they are looking at
the appropriate part of their document.  If a pane does change size, it
might need to contract or expand the region of the document that it is
interested in.  Other actions within the pane might also affect which
parts of the document should appear.

If a pane determines that something substantial might need to change, it
sets DAMAGED_VIEW using pane_damage() and it will then have the
"Refresh:view" message sent to it during the second phase of the refresh
process.

### Refresh display

The final stage in the refresh process involves updating the display.
By this time it is known how big the pane is and which part of the
document should appear.  It only remains to do the drawing.  Panes can
register a need for this phase by setting DAMAGED_REFRESH and this will
cause them to receive the "Refresh" message at an appropriate time.

Typically only one pane in the stack will perform this stage of the
refresh process.  Several other pane may respond to "Refresh:view" to
update their own summaries or make other adjustments.  Only the display
pane will do the drawing.

This stage will send "Draw:" message to clear a second of the display,
draw some text, or draw some images.

### Refresh finalisation

Once all of the SIZE, VIEW, and REFRESH damage has been handled as above
there is one more stage this just happens once - there is no loop around
it.  This stage finds all panes with DAMAGED_POSTORDER set and send the
message "Refresh:postorder" to it.  Typically this will be used by a
display pane to update the visible image, copying all internal content
out and ensuring that where panes overlap, the pane with the greatest
depth is most visible.

## Drawing on the display

Every top level display window, such as an X11 window or an ncurses
terminal, will have a display pane associated with it.  This pane
accepts drawing commands to create the displayed image.  Display panes
are explained in more detail later, but having a bit on understand will
be helpful here.

Any pane that calls "Draw:clear" to clear a pane will be seen by the
display pane as the "focus", and that will result in an internal image
being created for that pane.  Drawing operations act on this internal
image and then when "Refresh:postorder" is received the internal content
is copies out to the visible display.  When a drawing command comes from
a pane that did not issue "Draw:clear", the drawing happen on the image
for the closest ancestor which did.

As well as messages for drawing content there are messages for measuring
content to determine how much space in the display will be used.  As the
display window is the only part of the editor which knows about fonts or
can decode compressed images, only it can know about sizes.

So "Draw:image-size" will report the size of an image, and
"Draw:text-size" will report the size of some text, in a given font and
given font size.

Once sizes are determined, "Draw:image" and "Draw:text" can be used to
place content on the pane.

Normally it wouldn't make sense to clear both a pane and an ancestor of
that pane, as only the leaf-most pane would be seen.  However it can be
useful when you want a group of panes to share a background image.  The
common ancestor can be cleared and have the image drawn on it.  Then the
child panes can be clear, but not given any attributes to specify a
colour.  In this case the closest parent with a image content is found
and the relevant section is copies up.  This effectively gives the
child pane a transparent background.

## Some common panes for drawing

While it would be possible to write a dedicate rendering pane for every
different document type, that would result in pointless duplication of
effort.  Instead edlib provides a few panes that can work together to
present lines of text in a highly configurable way.  They also allow
images in place of lines.

If you wanted something quite different like a two-dimensional table, or
a document that scrolls left to right, then you would certainly need
something different. But for lots of use cases, these solve most of your
needs.

### render-lines

The "render-lines" module understands that many documents can be display
as a vertical list of lines.  When such a document cannot be completely
display it is appropriate to take section around the current focus of
interest, displaying some lines before and some lines after, but leaving
many lines completely invisible.  So there is a "top" and "bottom" of
the section to be displayed.

"render-lines" itself doesn't know much about the structure of lines or
how to display them.  It primarily knows that each line will consume a
rectangle within the pane, using the full width of the main pane, but
typically not the full height, so several of these rectangles can be
stacked vertically.  This is what "render-lines" does.

"render-lines" sends a message "doc:render-line-prev" to find the start
of the current line, or of the previous line, and with this starting
point will call "doc:render-line".  This must move to the end of the
line and return a string with the text to put in the line, possibly with
markup to provide font size and colour etc.

"render-lines" also understands that there is a need to map between
location in the document and locations on the display.
"doc:render-line" can be asked to track when conversion reaches some
mark, and to report the offset in the rendered string corresponding to
this point.  This can be used to determine where in the markup the
cursor should appear.

As "render-lines" collects these marked-up strings, it creates sub-panes
using "renderline" as described below to draw the marked-up string onto
the display.  This module has interfaces to measure the markup and to
locate and x,y co-ordinate in the markup and "render-lines" uses this to
map mouse clicks to locations in the document.

When "render-lines" determines the range of lines that will fit on the
display is sends a "render:reposition" message to the whole stack with
marks for the start and end of the visible range.  The number of rows
and columns displays are also reported as num and num2.  These can be
less than the height and width of the pane, but not more.  The are
calculated at a different time to the start and end marks and so can
result in a separate "render:reposition" message which doesn't contain
the marks.

This message is normally only sent when there is a change to report.
However other panes can request the information by sending
"render:request:reposition" which will cause the "render:reposition"
message to be send on the next refresh.

"render-lines" listens for "doc:replaced" notification from the document
and will update the display of anything that has changed.  It also
responds to "view:changed" messages which get sent to the stack.

"render-lines" responds to a number of "Move-" message to either update
the displayed range, or update the cursor based on the display of the
document, rather than on its content.

- "Move-View" will move the view by a multiple of its current size.
  The num1 argument is 1/1000ths of the height of the display, so
  a value of -500 will move the display backwards so what was in the
  middle is now at the bottom.  A value of 900 will move 90% of a page
  down so the few lines at the bottom will now be the few lines at the
  top.  After this message "render-lines" will stop ensuring that the
  point is displayed until something happens to make the point important
  again.

- "Move-View-Pos" will move the view if necessary to make sure that the
  mark passed as an argument is visible.

- "Move-View-Line" adjusts the view to ensure that passed mark is one
  the line number given by num1.  A negative line number is counted up
  from the end of the display, a positive number is counted forward from
  the start.  A line number of zero is the centre of the display.

- "Move-CursorXY" does not adjust the display, but moves the point as
  close as possible to the x,y position passed in the message.
  The str1 argument can indicate that a context-dependant action, if
  any,  could be performed.  If the content at the target location has
  a tag named "action-" followed by the value in str1, then the value of
  that tag is used as a command name and is sent to the focus with
  all attributes at that location passed as "str1".  "mark" will be the
  location that the cursor is about to move to, and "mark2" will be the
  mark that will be moved.

  Note that while Move-CursorXY will move the mark to a "near by"
  location if the is no character at that exact locate, the action will
  only be performed if there is a suitable character exactly where the
  x,y co-ords are.

- "Move-Line" moves the cursor (point) forward or backward some number
  of lines based on the num1 argument.  "render-lines" attempts to keep
  the cursor in the same column as it started in.  This column number is
  remembered even if it cannot be precisely achieved so a sequence of
  "Move-Line" messages will place the cursor in the starting column on
  any line where this is possible.

"render-lines" normally ensure that the location of the point for this
display stack is displayed.  If any of the "Move-View-" message above
are handled, "render-lines" will switch to ignoring the point, so the
cursor may not be displayed.  If it isn't, the cursor (inverted
rectangle) will appear at the bottom-right of the pane.  If anything
causes the point to move, or if an "Abort" message is received,
"render-lines" will stop ignoring the point and will reposition the
display to ensure it is visible.

"render-lines" will modify its behaviour based on some attributes
appearing in the pane stack:

- "hide-cursor": if "yes", no cursor is drawn
- "background": how to draw the background. Can be one of:
    + "call:KEY" - A message with the KEY is sent so some other pane can
       perform detailed drawing of the background
    + "color:COLOUR" - fills the background with the given colour.
    + "image:IMAGE" - the image is drawn to fill the pane
- "render-wrap" - if set to any value other than "yes", text is not
   wrapped.
- "render-vmargin" - if set to a number of lines, "render-lines" will
   attempt to keep the cursor at least this far from the top or bottom
   of the display.
- "heading" - if given, the marked-up string is drawn at the top of the
   display, and it stay there as content scrolls beneath it.

- "scale" - should be a pair of values in format WIDTHxHEIGHT.  If
   given, content will be scaled so that this many "points" will fit
   within the window.  A "point" is one tenth of the size of an "M"
   which is stored (in the same WxH format) in the "scale:M" attrbiute
   which is set by the display pane.
   So if "scale" is "800x250" then text is scales so that least 80 M's
   fit across and 25 M's fit down.


### lib-renderline

A "renderline" pane parses the marked-up string provided by
"doc:render-line" and issues the required drawing commands to make the
content appear in a window.  "render-lines" attaches several of these as
children of its own pane and feed the appropriated marked-up string.
The "renderline" panes have a stacking depth of "-1" which has a special
meaning causing it to be completely ignored by the pane refresh process.
"render-lines" *is* involved in this process, and asks "renderline" to
redraw as needed.

"renderline" responds to these messages:

- "render-line:set" - the string is the marked-up text to display.
- "render-line:draw" - parse the marked up text and draw it to the pane.

- "render-line:measure" - parse the marked up text without drawing it.
   If num1 is non-negative then measuring will stop after that many
   characters into the text, and cx,cy will be set on the pane recording
   where the next character would be drawn if there was one.
   If num1 is negative, then measuring continue to the end of the text,
   and the width and height of the pane are set to match the size of the
   rendered image.

- "render-line:findxy"

The pane's behaviour can be modified by several attributes that can be
found in the stack:

- "shift_left" - this should be in integer.  If less than zero the line
  will be wrapped if it is too wide to display completely in the pane.
  If it is non-negative then the display will not be wrapped and the
  display will be shifted to the left this far so later parts of the
  line can be seen.

- "prefix" - This marked-up text will be display first.  It will not be
  shifted even when the main text is shifted left.

- "word-wrap" - This can be "0" or non-zero.  If non-zero then text
  is wrapped on white-space (space, tab, any UNICODE space) that is not
  at the start of the line.

The pane also sets some attributes when handling these messages:

- "prefix_len" - the length of the appearance of the prefix
- "curs_width" - the width of the cursor.  This is typically the with of
   the letter "m".
- "xyattr" - when "render-line:findxy" is handled, the attributes that
  were in effect that the found position are made available as "xyattr".
- "line-height" - when measuring text, this is set to the height of the
  line.  When the line is wrapped this will be an integer submultiple of
  the resulting pane height.  When text is not wrapped it will be
  exactly the pane height.

#### renderline markup

The markup language that "renderline" understands uses angle brackets
("<>") to identify attributes as separate from text.  The content
between the angle brackets are a comma-separated list of tags, each of
which may be stand alone or may have a colon and a value.  A set
of attributes remains in effect until a balance "</>" or until the end
of the line is reached.  A less-than size can be included in the text
by doubling it "<<".

Some attributes are understood by renderline, others are passed to the
display to affect drawing of the text.  Attributes understood by
"renderline" are:

- "wrap" - text with this tag is a suitable place to wrap.  If the line
  is too long to draw completely, the last section of text marked "wrap"
  is taken as the wrap point.  Text before appears on one line, text
  after appears on the next.  When the wrap happens, the text marked
  "wrap" is not drawn at all unless the cursor is in that text.
  If no text has the "wrap" attribute, then wrap will happen after the
  last character that fits.

- "wrap-margin" - mark a location in the line where any subsequent wrap
                  will cause a margin to be inserted to.  Only the first
                  location with this tag is remembered.

- "wrap-head:" - The value of this tag is used to start the line after a
                 wrap point.  By default no text is added.  This is added
                 after the wrap-margin.

- "wrap-tail:" - The value of this tag is placed at the end of a line
  just before it is wrapped.  It will be underlined with blue
  foreground.  If no such tag is available a single back-slash is used.

- "center" - the line should be centred in the available space.
- "left:" - specify a left margin in points for the line
- "right:" - specify that line should be right-justified this far from
  right edge of pane.
- "space-above:" - extra space should be inserted above the line.
- "space-below:" - extra space below the line
- "tab:" - move to this position on the line before drawing more text.

- "image:" - This must be the first tag in the set of attributes, and the
  line must continue only of this set of attributes, no text.  The value
  is a "filename" which is passed to "Draw:image".
- "width:" - only valid after "image:" this is the width, scaled so that
  1000 is full size.  See scaling that should be described elsewhere.
- "height:" - only valid after "image:" this is the desired height,
  scaled.
- "noupscale" - normally an image is scaled within the size of the
  parent of the "renderline" pane, defaulting to take up at most half
  the width and half the height, but to be as large as possible.  With
  noupscale this calculation never makes the image larger than the
  native size of the image, only the same size or smaller.
- "hide" - the text affected by this attribute is not rendered at all.
   If any other attribute is active with the same or higher priority,
   "hide" becomes ineffective.

### lib-markup

The "markup" pane handles "doc:render-line" and "doc:render-line-prev"
messages sent by "render-lines".  It assembles a marked-up line from
text in the document together with attributes found in the text or on
marks in the document.  A level of indirection is used to find the tags
to use in the markup.

If a mark is seen in the range of text of interest, and if it has any
attributes that start with "render:", then the message "map-attr" is
sent to the stack passing the mark, the attribute name, and the
attribute value.  Some pane on the stack should recognise this
combination and all the callback command with relevant information as
described below.

If any text is found to have an attribute starting "render:", again
"map-attr" is called with a mark just before that text and with the
attribute name and value.

Finally, at the start of each line, a "map-attr" message is sent with
the attribute name "start-of-line" and not value.

Any handler that recognised the details in a "map-attr" message should
call the callback command with two numbers and a string.  If the first
number if non-negative, then the tags in the string apply to that many
following characters.  The second number is a priority so that tags set
with a larger priority will over-ride tags set with a lower priority.

Priorities 1-99 are for stable properties of the document, like colour
to highlight directories in a file listing.  Properties 100-199 are for
fairly stable results of analysing the document, such as spelling
errors.  Properties 200-299 are for less stable results like search
results of a selection.  Priorities larger than 65534 are all treated
identically, as are priorities less than 1.

Importantly any attribute which affects spacing, like 'tab' or 'centre'
and which cannot be closed and re-opened must have priority of 0.
Conversely the attribute "hide" is implicitly disabled when any other
attribute has a higher priority, and so it should typically have the
largest of the priorities in use.

If 'str2' is provided, it is inserted into the rendering.  Attributes
supplied will be applied to the text.

If the first number passed is negative, then any active tags that match
the string and the given priority will be cancelled, even if the length
count have not yet been consumed.  When this is used, the original
length count is usually given as an extremely large number meaning
"until cancelled".

#### lib-markup messages

 - "doc:render-lines-prev"
     + mark - starting location
     + num - if 0, move mark to start of current line.  Call should not
       fail but will return '1'.
       If 1, move mark back beyond the start of current line, then to
       start of the previous line.  Call can fail if it hits start of
       document.  It will return Efail in that case.

 - "doc:render-line"
     + mark - start of the line to be rendered.  Line continues until
       first '\n' or until 'num2' chars have been examined and no '\n'
       found.  'mark' will be moved to the end of the range that was
       rendered (often to the end of line line / start of next line).
     + mark2 - location of interested to be reported as offset in the
       rendering.  This is typically set to the current "point", and the
       cursor will be drawn where the returned string ends.
     + num - if >= 0, maximum number of result character to return
       before aborting.  'mark' will be moved to the location that would
       draw at or beyond this place on the screen.  This can be used to
       convert a screen location (from a mouse click) back to a document
       location.
       If num < 0 rendering continues to end-of-line.
     + comm2 - a callback used to return the rendered string in "str1",
       and the offset of mark2 (if given) in "num".
       The string will be freed after the callback returns, so it must
       be duplicated if later access is needed.

#### lib-markup attributes.

These attributes take effect is found on a pane in the stack.

 - "render-one-line" causes the whole document to be treated as one
   line, newline characters are treated like any other.
 - "render-hide-CR" causes the carriage-return character to be hidden.
   normally it is drawn as a red "^M" indicating "control-M".

### render-format

The "render-format" works with "lib-markup" to synthesise lines of text
from single objects on the document.  A "line-format" pane attribute
must be available in the stack.  It contains a line if text with markup
and attribute substitutions.  "render-format" breaks this down into text
and attributes and provide "doc:char" and "doc:get-attr" handlers to
make it look like the document actually contains that text, with those
attributes.  "lib-markup" then re-assembles this into a normal markup
line.

The reason for this double conversion is to make it possible to have a
cursor move around the line, even though the line doesn't really exist.

I wonder if there might be an easier way.

The attribute substitutions in the "line-format" are introduced with a
percent sign.  Doubling the sign ("%%") can include a literal percent.
The percent should be followed by an attribute name being a string of
hyphen, underscore, and alphanumerics.  If this is not followed by a
colon, then the value of this attribute on the current object in the
document is interpolated into the format.

If the next character *is* a colon, that should be followed by a number,
possibly with a leading minus sign.  The number is a field width with
padding added on the left or, if the minus sign was given, on the right.


FIXME where to I talk about Notify:clip?
Close:mark
