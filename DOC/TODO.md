To-do list for edlib
====================

Current priorities
------------------

- [ ] fix bugs
- [ ] lib-diff improvements
- [ ] lib-mergeview improvements
- [ ] Add menu/menu-bar support
- [ ] use common UI for dynamic abbrev and spell (and more?)
- [ ] Finish render-lines rewrite

- [ ] switch-buffer in pop-up window
- [ ] file in pop-up window in 'view' mode by default
- [X] Cx-44 to open doc/file in pop-up.

Bugs to be fixed
----------------

- [ ] sometimes when press 'enter' at end-of-file, page refreshes to move
      cursor closer to end of pane .... but not always.
- [ ] When cursor is off-screen pygtk cursor gets drawn on background and
      stays there.  I think pygtk needs to know about an off-screen cursor
      and draw that explicitly in 'refresh()'.  For that to work, render-lines
      need to explicitly tell it that this is offscreen, so that it can be ignored
      when not in-focus, or drawn if it is.
- [ ] When select line from grep/make results should replace any current results pane

Requirements for a v1.0 release
-------------------------------

- [ ] efficient refresh using separate lib-renderline for each line
- [ ] efficient X11 drawing using server-side surfaces
- [ ] configuration
- [ ] vi mode
- [ ] office mode
- [ ] nano mode(?)
- [ ] introspection
- [ ] markdown editor with PDF output
- [ ] spell checking
- [ ] non-line-based render, such as a tabular render for spreadsheet.
- [ ] documentation reader
- [ ] block-dev (mmap) doc type, and some hex-mode support
- [ ] user documentation
- [ ] developer documentation
- [ ] notmuch
- [ ] some git support

Core features
-------------

- [ ] Change Efallthough to -1 so I can return '0' meaningfully.
- [X] Make it easier for Move-EOL to move to start of next line
- [ ] key_add_prefix() doesn't work if there is no punctuation.
- [ ] send warning message when recursive notification is prohibited.
- [ ] detect and limit recursion
- [ ] Change naming from "Meta" to "Alt"
- [ ] Make DEF_CB really different from DEF_CMD and ensure it is used properly.
- [ ] is DocLeaf really a good idea?  Maybe panes should have 'leafward'
      pointer separate to 'focus'?  Maybe panes could have optional
      'child' method which returns main child - pane_leaf() calls that.
      Maybe pane_leaf() find a pane with z=0 and matching w,h ??
- [ ] maybe generalize search and select somehow, so a line-based filter can
      detect and highlight the selection, rather than major-mode being fully
      in control  Similarly search might be handled by a render pane.
- [ ] should pane_clone_children() copy attrs too?
- [ ] support text-replace as easy as text-insert (doc:char...)
- [ ] What about :Enter and BS TAB ESC ???
- [ ] For a notify handler, returning non-zero doesn't stop other handlers
      running.  For a call handler it does.  This inconsistency is awkward for
      messageline_msg which wants to allow fallthrough, but needs to acknowledge.
      How can I resolve this?
- [ ] make a doc read-only if dir doesn't exist or isn't writeable
- [ ] account all mem allocation types separately, and (optionally) report
      stats regularly
- [ ] graceful failure when closing doc that still has views.
      Then call doc_free() internally so the module doesn't need to.
      Also if there are still ungrouped marks with ->mdata.  There shouldn't be, but
      coding errors can cause that.
- [ ] marks should not be auto-freed on close as there could still be a pointer
      somewhere from the owner.  Rather they should be disconnected and tracked
      so that a 'free' can work, but nothing else does anything useful.
- [ ] When I cal DocPane I normally doc:attach-view a doc there. But it is
      the same doc, so pointless.  Can I optimise that somehow?
- [ ] document the use of doc:replaced.  What are the two
      marks exactly? start and end of range.  Verify all clients and providers
- [ ] revise and document behaviour of doc:open etc, particularly for
       reloading and filenames etc.

### Longer term

- [ ] Make it possible to unload C modules when refcount on all commands
      reaches zero
- [ ] Make it possible to unload Python modules
- [ ] malloc anti-fail policy.  Small allocations don't fail but use pre-allocated.
      large allocations use different API and can fail.
- [ ] support $SUBST in file-open path names ??
- [ ] Need a debug mode where every mark usage is checked for validity.
      also check the setref sets up all linkages.
- [ ] remove all FIXMEs (there are 55) ... and any HACKs (5).
- [ ] Replace asserts with warnings where possible.
- [ ] hide view-num inside pane so number cannot be misused.
     i.e. each view is owned by a pane and can only be used by that pane.

Module features
---------------

### lib-search

- [ ] make it easy for a make-search command to search backwards

### autosave

- [ ] if multiple files are opened quickly (e.g. by grep), we might get cascading
      autosave prompts.  Introduce a mechanism to queue them and only have one per
      display

### tile


### rexel


### popup

- [ ] I need a way to move the pop-up window to an existing pane.
- [ ] if 'focus' is a temp pane, it might disappear (lib-abbrev) which
      closes the popup.  I need to some how indicate a more stable pane
      for replies to go to
      Maybe lib-abbrev should catch ChildRegistered and call pane_subsume
      to get out of the way ... or similar....
      Or maybe lib-abbrev just stays there ignoring commands until it has
      no children, then it disappears.

### lib-diff

- [X] diff-mode keystrokes for 'next diff', prev, etc
- [X] When only add or only delete, highlight in same way as when there are both.
- [X] allow inversion so 'enter' looks for the '-' not the '+'
- [X] status-line entry to indicate if inverted or not.
- [X] Add code to check alignment and search for nearby match.
- [ ] command to apply a hunk to a given document - or to reverse it.
- [ ] Link wiggle code to find best-match if direct match fails
- [ ] command to find best 'wiggle' match, and another to apply it if no conflicts.
- [ ] command to move to matching place in other branch

### lib-mergeview

- [ ] merge-mode to highlight markers with "space-only" or "no-diff" state
- [ ] merge-mode command to select one of the three "this only".
- [ ] merge-mode automatic detect, enable, goto-first
- [ ] command to cycle through matching places in other 2 branches.

### emacs

- [X] find-file/buffer in popup.  Cx-44??
- [ ] make-directory command
- [ ] semi-auto make-dir on save to nonexistent
- [ ] sort the command names for command-completion?
- [ ] filename completion should ignore uninteresting files like ".o"
      Maybe use .gitignore, or have config module understand that.
- [ ] search highlight doesn't report empty match (eol)...
- [ ] emacs highlight should get close notification from popup,
      instead of catching abort.
- [ ] ask before killing modified buffer - or refuse without numeric prefix
- [ ] maybe meta-, does c-x` if that is the recent search?
- [ ] Support write-file (providing a file name) - currently I only save
      to the file I loaded from.
- [ ] Support include-file (C-x i) to load contents of a file at point.
- [ ] C-uC-xC-v prompts for file name, like C-xC-v in emacs
- [ ] compare two panes somehow
- [ ] pipe doc or selection to a command, optionally capture to replace with output.
- [ ] history for each entry.

##### needs design work

- [ ] invent a way to reserve 'extra' values for command sets
      do I need this across panes ?? probably
- [ ] search/replace should support undo somehow
- [ ] search/replace should make it easy to revisit previous changes.
- [ ] What should be passed to M-x commands?  prefix arg?  selection string?  point?

#### history

- [ ] Make it possible to search through history. Maybe Alt-P only shows
      lines containing current content.

### ncurses

- [ ] add full list of colour names (to lib-colourmap)
- [ ] allow a pane to require 'true-color' and discover number of colours available
      Colour map gets changed when it becomes the focus.
- [ ] merge 'catpic' code to draw low-res images.
- [ ] When only 16 colors, maybe add underline when insufficient contrast available.
- [ ] automatically ensure the fg colour contrasts with bg, unless explicitly disabled.
      If bg is bright, reduce fg brightness.  If bg is dark, reduce saturation.

### pygtk

- [ ] interactive command to open pygtk window even from ncurses.  displayname can be given
- [ ] make sure pixmap handling in optimal - I want the per-pane images to be server-side
      See cairo_xcb_surface_create.
- [ ] allow 'pane-clear' to use content from lower-level image.
- [ ] If a net connection to a display goes away, we can block on IO to that display.
      Particularly an ssh connection to an ncurses display.
      The problem is the x11selection X connection. When it is closed, the
      whole app dies!
      ARRG.  This is a gtk bug that emacs wants fixed too.  I guess maybe
      I need something other than gtk... I wonder if I can tollerate tk??
      or PyFLTK or WxWidgets .... or XCB??
      Or maybe run any gtk code in a separate process...

### render-lines

- [ ] update_line_height should handle TAB (and any control) - cannot expect
      text-size to handle it.
- [ ] adding 10% height at e-o-f doesn't make sense with a one-line display
      and certainly must not push the cursor line of the screen as it currently does.
- [ ] Give lib-renderline a Refresh:view which calls something in the render-line
      pane which does call_render_line().  Use pane_damaged() to mark panes as invalid
      and pane_refresh() to update them.
- [ ] revise render_lines_move
- [ ] revise render_lines_move_line
- [ ] revise render_lines_view_line
- [ ] click in a wrapped line always goes to first line.
- [ ] Replace <attr> text </> in markup with SOH attr STX text ETX
      This also affects lib-markup and others.
- [ ] I regularly hit problems because ->mdata is not up to date and we render
      to find a cursor and compare with ->mdata and get confusion.  How can I avoid this?
- [ ] view:changed shouldn't destroy the view, else Move-CursorXY
      gets confused.
- [ ] make renderlines "refresh everything when point moves" optional.
- [ ] if flush_line from render_line() keeps returning zero, abort
- [ ] render-lines should always re-render the line containing point, so
      the location of “point” can affect the rendering.

### lib-input

- [ ] can we capture the substates of character composition, and give feed-back?

### lib-macro

- [ ] detect errors includng Abort and search failure etc. Abort capture or
      replay on error
- [ ] 'capturing' state should be visible in status line.
- [ ] Possibly wait for a shell-command etc to complete before continuing.

### doc-dir

- [ ] allow setting a pattern, as alternate to substr, for 'complete' viewer.
- [ ] how to change sort order of a directory listing.  I think this requires
      a separate dir document, which borrows state from the main one.
- [ ] chown/chmod/unlink/rename etc

### doc-text

- [ ] use larger buffers for adding text - especialy when filling from pipe.
      e.g. new buffer doubles each time??
- [ ] support disable of undo in text, e.g. for copybuf document.
      I think this is a completely different doc type
- [ ] Possibly move read-only handling to core-doc, once docs/dir
      respond to something other than 'replace' to open files.
- [ ] how to prune old undo history?
- [ ] allow undo across re-read file. Keeping marks in the right place
      will be tricky, but might not be critical.
- [ ] report stats on:
        undo usage, chunk usage
- [ ] if 'find-file' finds same inode/inum, check the name is still valid.
       file might have changed (stg pop/push) underneath us.
- [ ] handle large files better - loading a 42M file took too long.
      Maybe it was the linecount?

### completion

- [ ] When 'delete' and there is only the original
      entry of the prefix stack, just delete one character.
- [ ] mouse selection should work in completion pane
- [ ] filename completion should work for earlier component of path.
- [ ] The “complete” popup should be positioned above/below the file name,
      not over the top of it.

### lib-view

- [ ] easy way for minor-modes to report existance in status bar
- [ ] review use of line-drawing chars for window boarders
- [ ] improve scroll bars
- [ ] make (trailing) space/tab in doc name visible
- [ ] review decision about that to do when high < 3*border-height.
      Current (disabled) code makes a mess when differing scales causes
      borders to be shorter than content.

### doc-rendering

- [ ] create alternative to doc-rendering which *knows* that the int of mark.ref
      is unused and puts a line-offset in there.  Then mark is safe for use in doc.
      Content is extracted (e.g. with lib-format) and doc:step and doc:content are
      implemented in an overlay which detects markup and presents it as attributes:
      render:markup gives the attr string and len:markup gives the length to ETX
      Rather than formatting to a string with markup, we could format to a list
      of attr/text pairs.  lib-format returns these via a callback and we use some
      bits to index the list and some to index a char.

- [ ] doesn't support highlights from marks
- [ ] maybe should highlight whole line that has cursor.

### grep/make

- [ ] When I visit from grep in a popup, I think I want a 'view' at first.
      so 'q' works.
- [ ] Need keystroke to step through different grep/make windows
- [ ] if file isn't already loaded, wait until it is wanted, or something
      else loads it.
- [ ] if file is reloaded, re-place all the marks, don't just ignore them.
- [ ] clarify and document the role of numeric args to git-grep

### message-line

- [ ] messages gets too much noise but doesn't get 'version'. 'log' gets messages..
- [ ] Differentiate warnings from info, and blink-screen for warnings.

### docs

### hex

- [ ] improve doc:replaced handing, by tracking the visible region and
      checking if a replacement changes the number of chars.

### shell mode

- [ ] If current directory doesn't exist, cope somehow
- [ ] make sure CWD env var doesn't end '/'.
- [ ] 'shell-command' should try to use same pane even though it
      kills the old document and creates a new one
- [ ]  Use pattern-match on command to optionally choose an overlay
       which can highlight output and allow actions.
       e.g. (git )?grep   - highlight file names and jump to match
            git log  - highlight hash and jump to "git show"
            diff -u  - some diffmode handling
- [ ]  If no output, don't create a pane??  Or just one online.
- [ ]  Detect ^M in output and handle it... delete from start of line?
- [ ] always track time for a run and report it - or at least make it available

###  edlibclient
- [ ] run edlib directly if no socket
- [ ] option to create a new frame
- [ ] more work on server mode:
- [ ] improve protocol

### line count

- [ ] count-lines seems to go very slowly in base64/utf-8 email

### notmuch

- [ ] when I unhide an email part which is a single v.long line,
    redraw gets confused and point goes off-screen, which seems
    to leave it confused.
- [ ] make min top/bottom margin configurable, set for message list
- [ ] error check Popen of notmuch - don't want EPIPE when we write.
- [ ] render-lines calls render:reposition with m2 beyond the end of displayed region.
- [ ] search in thread list - and within a thread
- [ ] in notmuch I searched in a message (mimepart), then enter to choose,
   then 'q' and crash.
- [ ]  A multipart still had an active view.
- [ ] display counts of current thread somewhere, so I know where I'm up to.
- [ ] allow refresh of current search, especially when re-visit
- [ ] background colour for current message/thread
- [ ] how do work with multiple thread?
- [ ] in text/plain, wrap long lines on 'space'.
- [ ] need to resolve how charsets are handled.  Maybe an attribute to
   query to see if there is a need for a utf-8 layer on email
- [ ] in quoted-printable, line ends "=\n" does always join as it should.
   See email about Mobile number
- [ ] When click on first char in tagged range, I don't see the tag and
   don't get a Mouse-Activate event.
- [ ] search documents don't disappear when unused
     They, at least, should refresh and clean when visited.
- [ ] line wrap in header should not appear as space??
- [ ] how to make unselected messages disappear from list
- [ ] refresh thread list
- [ ] highlight current summary line
- [ ] if a subject line in wrapped in the email, the summary line look weird
- [ ] Add Close handler for doc-docs.c
- [ ] handle all Unicode newline chars.
- [ ] should multipart/visible be per-view somehow?
- [ ] dynamic search/filter pattern
- [ ] Handle \r\n e-o-l and display sensibly
- [ ] command to skip over whole thread
- [ ] use NOTMUCH_CONFIG consistently - not used for locking.
- [ ] look into stored-query!!
- [ ] 'class searches' should be given callback on creation??
- [ ] doc:notmuch:search-maxlen should be attribute, not command.
- [ ] re-arrange notmuch code so doc are first, then viewers, then commands
- [ ] Chr-g in search/message window should remove non-matching entries from
     search.  Chr-G discards and starts again.
- [ ] We *must* not change order or messages when reloading, without fixing all marks
   that refer to anything that moved in order.
- [ ] rel_date could report how long until display would change, and
   we could set a timer for the minimum.
- [ ] simplify thread,mesg ordering by using maxint instead of -1 ??

###  multipart-email

- [ ] I need a more structured/extensible way to decide which button was pressed
- [ ] I need key-click to work reliably somehow. Click on selected button?
- [ ] Auto-hide depending on type - with extensible table
- [ ] Open-with always,  Open only if a handler is known
- [ ] "save" to copy to buffer
- [ ] save function - doc:save-file given file name or fd
- [ ] brief summary of part type in button line
- [ ] open function
- [ ] '+' or '-' to change flags, S marks newspam N notspam ! unread,inbox
- [ ] make URLs clickable
- [ ] highlight current message in summary list - background colour?
- [ ] encourage current message to be visible when list is auto updated
- [ ] point in summary list should stay close to middle
- [ ] 'Z' should work in email-view window, not just summary window
- [ ] better feedback to 'g' - e.g flag that update is happening
- [ ] I don't think summary updates correctly
     when count notices a difference, it should trigger a refresh
- [ ] Chr-a should affect thing under cursor, not current thing
- [ ] detect and hide cited text
- [ ] detect and highlight links
- [ ] Make long to/cc headers truncate unless selected.
- [ ] select parts from multipart
- [ ] buttons for non-displayable
- [ ] display image on gtk,
- [ ] display image on ncurses.
- [ ] improve update of message list... sometimes disappears

### Presenter

- [ ] split into lower pane which parse markdown and upper which handles presentation.
- [ ] command to immediately change current pane in to presenter view
- [ ] add viewer pane so cannot accidentally edit - and space pages down.
- [ ] translucent bg colour for paragraphs
- [ ] partial-view-points. Only render beyond here if mark here or beyond.
    page-down goes to next such point
- [ ] temp attribute.  :bold: etc only apply to para, :: is appended to para format
- [ ] should doc attributes append to defaults, or replace?
- [ ] word-wrap.  Interesting task for centring
- [ ] force x:scale to be maximum width of any line - to avoid surprises
- [ ] proportional vertical space ??
- [ ] right:0 and right:1 don't do what I expect
- [ ] thumbnails for easy select?
- [ ] \_  etc to escape special chars
- [ ] boiler-plate, like page numbers

     - Maybe stuff before "# " is copied everywhere.
     - Need magic syntax for fields ##page#

### C-mode

- [ ]  auto-indent enhancements

     +   py: after "return" de-indent
     -  if statement is assignment, align to '='
     -   Should '/' see if at start of comment line following '* ', and discard space?
     -   A line after one ending ; or } or : or unindented is assumed to be
         correctly indented.

- [ ] configuration: use tabs or spaces for indent
- [ ] configuration: use only spaces for bracket-alignment indents - or tabs as well.
- [ ] python-mode: when changing indent, make same change to the whole block.
      Not sure how to handle 'else:' which looks like the next block.

### lang-python

- [ ] should Efallthrough be an exception?
- [X] Print error name when there is a python error
- [X] edlib.LOG can crash if there are '%' if the buffer.
- [X] expose rexel constants
- [ ] report error if release mark which isn't ours.
- [X] create a library of support functions like doc_next, doc_prev etc.
- [ ] Log loading of modules - Can I provide version info?
- [ ] we aren't catching errors from functions called from .connect()
       Maybe use sys.excepthook(typ,val,tb)
- [ ] Add version info to python modules

### white-space

- [ ] support highlight of spaces-for-indent
- [ ] support highlight of tabs-for-indent
- [ ] make set of highlights, and colors, configurable
- [ ] support highlight for hard spaces
- [ ] support for blank lines near blanklines or start/end of file
- [ ] support highlight for 8spaces after a tab

### test suite

- [ ] tests for double-click and drag.
- [ ] test for recent search improvements
- [ ] Add mechanism to easily run a command with pre-canned output.
- [ ] Add one test case, and arrange for auto-testing on commit. 
- [ ] allow single-step testing?
- [ ] Allow testing gtk as well an ncurses
- [ ] Allow testing of server/client accesses
- [ ] create a pane which exercises lots of code and measure coverage.
      particularly cover all the doc-text undo/redo code.
- [ ] Track 'coverage' of all commands in keymaps.

### dynamic completion

- [ ] provide a drop-down menu with options
- [ ] the 'back' option doesn't work.  Need to discard current
      expansion first.

### spell checker
- [ ] extract words better. e.g. '-' and '/' separate words.
      Maybe have a regexp which defaults [A-Za-z.']+ ??
- [ ] mode-specific so latex can ignore \foo
- [ ] Some way for 'c-mode' to report where comments are so they can be spell-checked
- [ ] auto-spell checking on display
- [ ] record what has been checked and what hasn't.  When content is changed, remove
      'is checked' indication for whole line.
- [ ] mark individual words that are mis-spelled.
- [ ] command to cycle between options.- maybe similar to Alt-/
- [ ] drop-down with options
- [ ] command to add word to per-document list, or personal list

### calculator
- [X] names values are stored in the view pane, not the doc.
      Should I just move them to the doc?  Should I then cache them in the pane?
      attr lookup on a doc isn't optimized...
- [ ] regression test
- [ ] calc-replace should leave result in selection. - or only in the selection.
- [ ] calc-replace could cycle through bases.
- [ ] highlight error location in red
- [ ] trunc(a,2) a^b  pi % // & | ~ &~
- [ ] increase precision of sqrt)()
- [ ] useful error messages.
- [ ] alt-p to interpolate previous expression
- [ ] fix Make dependencies so changing calc.mdc only requires one 'make'.
- [ ] Don't always show fraction - maybe request it like with '@' for octal
- [ ] if calculation produces same result as it present, don't modify doc.

New Modules - simple
--------------------

Possibly some of these will end up being features in other modules.

- [ ] C/python code "index" pane to quickly jump to function, and see context
      This part of the IDE project below.

- [ ] create view-pane that either puts a cursor on whole line, or moves
      the cursor to the "right" place.  Maybe a markup to say "here is the
      preferred column" ??  Maybe use for make output so I can see current
      match more easily.

- [ ] create a pane-type that just renders a marked-up line and use
      several of these for render-lines, and one for messageline.
      side-scrolling will be interesting.
      pane_clear might want to copy relevant region from underlying pane.
      Hopefully this will cure my performance problems with gtk on my slowish
      notebook.

- [ ] tags handling - and easy tag-search without tags. e.g. git-search.
      alt-S looks for TAGS or .git and either does a tags-search or a grep-l and
      check every match.  So maybe this is part of the 'make' module
- [ ] menus
      This might support a menu-bar, or drop-downs for spelling or dynamic completion.
- [ ] hex edit block device - mmap document type

- [ ] image-display pane - e.g. can be given a png/jpeg etc file and display
      it scaled, plus allow scaling and movement
- [ ] pdf-display pane - like image-display but with multiple pages.
      Would use libpoppler.

- [ ] Separate out filesystem access from doc-text and doc-dir and elsewhere
      into a filesystem access module.
- [ ] Create compress-access module that layers compression over fs access
- [ ] Create gpg-access module that layers encryption and decryption over fs access
- [ ] Create ssh-access module that uses ssh/scp to access files

New Modules - more complex
-------------------------

### threaded-panes

An import characteristic of a good editor is low latency.  While you can
usually get good latency in a single thread by dividing the work up into
small tasks run from an event loop, this isn't always possible or easy.
So some sort of threading will be useful.

Some of the use-cases for threading that have occurred to me are:

- multiple event loops:: gtk code needs to use the glib event loop, but
  I rather use a simpler event loop when not using gtk.  I current have
  a mechanism to select between event loops so the glib one is selected
  for everything if anything needs it.  But I don't like this approach.

  If I could have threads, I'd have the edlib event loop handling
  the main editor loop, and gtk could be in a separate thread with its
  own event loop, exchanging messages with the main loop as needed.

- long-running tasks:: word count and spell check can each be divided
  up into small events, which each do a limited amount of work and reschedule
  themselves, but having a separate thread that could just keep working
  until the job is done would be easier.

- fsync:: Calling fsync when saving a file is important, but can be slow.
  It would be nice if the main thread could just write out the file,
  then a separate thread could call fsync() and report when that was done.

- remote editing::  It probably doesn't require threads, but they might
  be a suitable abstraction for allowing one editor instance to work
  with another over a socket - possibly between machines.  I imagine
  an editor running on my notebook communicating with the editor
  running on my desktop - directly accessing the document as stored
  in the desktop editor.  These would have to be two separate threads - separate
  processes even.  The same mechanism used for local threads to communicate
  could be leveraged for remote threads to communicate.

I probably don't want any shared data structures except the pipe that sends
events back and forth, and these events need to be standard commands communicating
between panes.  So it might be a variant of notifications.

Keeping tracks of marks across the link will probably be the most complex
part.  Possibly only marks owned by the pane will be mirrored across.

###  interactive shell / terminal emulator

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

It would generally be useful to have a pane that read from a pipe and
only read more when some view wanted to see more of the output.  It might
also be useful for such a pane to store the content in a mem-mapped file
rather than in anon memory.

### Outline code rendering.

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

### email composition

There a several parts to composing an email message:

- Adding addresses - with auto-completion from address book or recent
  correspondents.
- adding text, possibly with mark-up (maybe to be converted to HTML).
- General text-edit support: wrap, auto-indent, spell-check
- adding attachments - with MIME-type and inline/attachment flags
- encrypt and sign
- sending the email

Some of this would be left to external tools, other bits would need help
from external tools.

### git-mode

Git adds an extra dimension to editing code - the dimension of history.
Somethings that the editor can help with:

- creating a commit, including writing the comment message and selection
  which files, or which lines in which files, should be added to the commit.
- browsing history - a bit like gitk.  So from a 'git log --oneline', open
  a view on a patch, or on a file at the time of the commit.  This might
  only fetch 100 lines - or only since origin/master, and fetch more
  only when it is accessed.  Including rebase functionality here would
  be cool.
- editing history - using "git rebase --interactive" or similar.
  The editor could open either a file or a diff at the time of a commit
  and allow them to be editing.  For smaller changes, editing the diff
  directly would be nice.  In either case, having one auto-update when the
  other is edited would be cool
- editing just the comments of recent commits is useful.
- Showing a current 'diff' which is dynamically updated and which distinguishes
  between staged and unstaged changes - and lets them be toggled - would be
  part of this.

A view on "git log" would only show the first page until you scroll down.  Then
more would be requested and displayed.  So we don't generate thousands of commits
unless that are actually wanted.

### vi mode

I currently support emacs-like editing, but that is mostly kept local to one module.
I'd like to add a comparable vi module, partly because some people like vi
and partly because it would encourage me to keep the key-bindings cleanly
separate from the functionality.

### office mode

- C-c for copy, C-x for cut, C-v for paste, 
- Shift-arrows to select C-arrows for word-movement
- C-bs C-del delete word
- C-a select all
- C-f find/replace C-S-f - search again
- home/end - start/end of line
- Shift-home/end - start/end of file
- C-up/down start/end para
- C-z undo C-y redo

Commands that are 'C-x' or 'C-c' in emacs would be 'alt-f' (For file) etc
and could pop-down menus from a menu bar.

### a “reflection” document so I can view the internal data structures.

When developing a new pane things go wrong, but it is hard to see what.
I want a pane which shows me the structure of all panes, with parent/focus
links, with notification chains, and with other ad-hoc connections.

I'd also like to be able to follow a command as it moves through the panes.
This probably needs to be recored, then played back for me.

### calendar/diary/planner

I don't keep a diary or use a planner much, so this seems like an odd thing to include.
But dates are cool, and this is a highly structured concept and I like structure.
At the very least I want a calendar pop-up.

### info browser

Info is widely used... rendering it like markdown and allowing
browsing would be nice.

### A suite of tools making use of some sort of "mark-down" like language

Restructured text? Markdown?  Commonmark?  Markright?

Having simple readable extensible markup is useful for writing
READMEs and TODO lists and emails and calendar entries and presentations
and all sorts of stuff.  I probably want to invent my own, because the
world always needs another markup language.

I want:

- easy render to PDF - e.g. use PyFPDF
- ASCII-Math or similar
- anchors for links, structure tags (author, title), foot notes,
  figure captures, tables, index, content.
- figure drawing.
- spreadsheet cells for auto calculations.
- outlining support of course.

Non-module functionality
------------------------

### Documentation

 Both user-documentation and developer documentation, extracted from
 literate programming comments, and viewable using markdown mode.  This
 would include links to other files with more content.  Maybe
 documentation from a given file could be parsed out and displated
 interactively by a doc pane.

### IDE

To build an IDE with edlib there are various parts that are needed.
They wouldn't all be in one pane, but the various panes might work
together.

Functionality includes:

- LSP (Language Server Protocol) integration.
- build:: I can already run "make" easily, though there is room for improvement.
- error location:: Going to an error line is easy.  It might be nice to have
   the error message appear in-line with the code, rather than needing to have
   a separate pane containing that.
- search::  jump to definition or use of function, types, variables, structure fields etc.
  Most of this needs an external tool, whether 'grep' or 'git grep' or 'cscope'.
- syntax highlight:: different colours for different types of symbols is sometimes nice.
- auto-format:: indenting and comment wrapping are particularly helpful to have
  automatic support for.  I've never found I wanted key-strokes to insert
  structured commands, but maybe having a close-bracket auto-added when
  the open is typed would be nice.  Then when close is typed, just step over
  the already existing one.
- outlining:: I would really like to always be able to see the function name and
  signature - and probably the names of surrounding functions so that I can easily
  navigate to them.  See "Outline code rendering" above.

Interaction with gdb would be nice too - things like

- set break points and watch points from the code
- step up and down stack and jump around code at same time.
- view values of variables directly from the code.

### config

What needs to be configured?  How is that done?

- fill mode and with
- default make command, and dir to run in
- preferred white-space options, and width
- unintereting file names for find-file. .git-ignore??

I want different configs in different trees.
Either the single config file identifies path, or we put
one in the tree root.  Or both.

So config must be based on path, and on file type.
Maybe the config is processed whenever a file is loaded, and attributes
are attached to the document.  Though global attrs should go on root.
