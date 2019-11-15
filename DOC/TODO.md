To-do list for edlib
====================

Current priorities
------------------

- [X] when grep visits a file, it should reload it first.
- [X] When a file is reloaded, grep should notice and forget its mark.
- [ ] diff output parser - with Enter to jump to the file, and some colorization
      word-diff highlighting would be ideal
- [X] c-mode move-expr to not treat _ as word separator
- [X] c-mode, alt-tab always indents, even if not start of line
- [X] history for find-file - incase I get it wrong and want to try again
- [X] arg to C-x-` should restart from first match
- [X] revisiting the grep-output buffer should attach pane to handle Enter etc
- [X] Introduce command to move a mark before or after all other marks
      at the same location. .make_first .make_last
- [ ] Review call calls to Notify:change do Notify:doc:Replace and ensure the
      marks sent make sense
- [ ] review notify:change handlers
- [ ] makes sure text:match and text:search do something sensible with the mark
- [X] message line timed updates - no change for 7 seconds, always revert after 30 seconds
- [X] copy-paste with mouse
- [X] select buffer to not accept a non-existant buffer
- [X] Fix c/python indenting
- [X] search and status if file-edit popup
- [x] fix undo positioning of pointer and grouping of edits
- [X] simplify/document rxl_advance interface, particularly 'restart'.
- [X] \b for word-break - maybe provide a regexp set for 'is a word char'
- [ ] \B for non-word-break.  This needs a change to how flags are handled.
- [X] rexel/rxl_advance: "clear out next lists" take too long on long patterns. memset?
      or follow the existing chain?  Does 'leng' need to be cleared?
- [ ] \1 substitutions
      Maybe to extract a given submatch we have a third array pair where we record
      the length since a particular point.
      We then repeat the match process against the found string to get start
      and end points.
      Or write a back-tracking matcher that records all groups in the stack
- [X] lax search - single space when case insensitve matches multiple spaces/tabs/newlines
- [ ] search-replace.
      This might just be an extension of the current search.  I would have
      two input boxes, maybe left and right, or one above the other.
      I like to be able to see the result after the change, particularly
      if the change as '\1' style references, so maybe some key-stroke to
      replace without moving forward.
      Also want an easy "undo" of the last changed.
      Changing the replace string during the process should be allowed too.
- [X] fill command
    - highlight any chars beyond the fill-column
    - need to choose a prefix of first line, and of subsequent lines.
    - possibly determine a suffix too - useful in comments.
    - alt-Q - reformat para
    - if there is a selection, that defines the para, else some sort of search.

- [ ] fill-mode
      - some way to select it (configuration?) and to set width
      - on space or enter, fill just current line, but use same mech as fill command
        to determine prefix
      - highlight text beyond width, which will wrap when space/enter is pressed.

- [ ] rename lib-renderline to lib-markup and create 'renderline' for a single
      line of render-lines.
- [X] Add 16bit semi-unique number to doc panes, which is inherited
      by any view pane and by every mark.  Check the numbers are
      consistent and fail with a warning when not.
- [ ] change notmuch-query-view to use doc-rendering
- [X] Discard rpos handling

Bugs to be fixed
----------------

- [X] in python code, protect Replace/doc:replace - file could be read-only
- [X] c-mode indent should let me delete one level when it was due to bracketing,
      so I can insert the close bracket.
- [X] comma sep list in enum doesn't get indented properly.  Need to recognize enum
      and treat ',' as end-of-statement .
- [X] case 0: indent seems to be broken... in "if ()\n switch(){ case 0:"
- [X] If you wrap a list of global variables, it doesn't indent.
- [X] <40> in a line of text (immediately after wrap) can make big text.
- [X] accessing a different frame shouldn't make a transient-popup
      disappear
- [X] server transient popups should use the most recently accessed frame
- [X] capturing 'Enter' in c-mode for indent interrupts a sequence
      of N2_undo_insert.  Do I want that?
- [X] message line needs to keep some messages permanently - 
      'type ` to search again' - and needs to clear it immediately.
- [X] allow rpt-num to be accessed from python
- [X] move-expr needs to understand sloshed quotes.
- [X] regexp ^[^a-zA-Z0-9]*$ doesn't match properly.
- [X] doc:replace with 2 marks should leave them at either end of insertion.
- [X] C-space, M-> doesn't highlight the selection
- [X] C-n causes 'target' column to be no more than EOL.
- [X] need a way to show that eol is highlighted - both for selection and search
- [X] C-x-o to change panes - if point is not visible in other pane, the visible cursor
      doesn't move (ncurses), which is confusing.
- [X] An indent can still get more than 8 spaces.  Maybe a higher-level check is needed.
- [X] fn-complete for a symlink-to-dir should add a '/'
- [X] Use the requested name rather than canonical name for UI as much as possible.
- [X] when visit a non-existant file, it is CHANGED and read-only.
- [X] Indent to always use tabs where possible (in C mode)
- [X] C-q tab should insert a literal tab
- [ ] If completion has happened in open-file, but file doesn't exist,
      require an extra Enter (or something)
- [X] ncurses - provide a suitable colour for  highlighting tabs. #f5f5dc
- [X] change to document with name ending <2> fails
- [X] 'click' no longer makes target window the focus
- [X] file-name completion in git-grep dialog works in current dir instead of target
- [ ] M-0 M-.  does grep in current dir, but doesn't extract word from document.
        But M-- M-. does the right thing... what do I want?
- [X] directories used for name-completion should go to bottom of doc list.
- [X] start with "-g", open a -t, close the -g, then the -t : get into a spin
- [X] undo sometimes gathers too much into a single change.  Cursor movement
        should break the change. -- yank was the problem
- [X] when move down causes a scroll, we temp lose target column
- [X] Commands should *never* fall-through as marks get completely messed up.
      there are still places that try to protect against this.
- [X] mouse-click before the end of an active selection picks the wrong place.
- [X] mouse actions need to affect selection: set or clear.
- [X] detect when file has changed since it was read.
- [ ] alert when finding a name that is a link to another
- [X] dirname sometimes has 2 trailing '/'.
- [X] make sure *Welcome* has a dirname (it didn't once..) ... seems to work now
- [X] NO - nothing to be gained.
      temp docs, such as pop-up input, can be inline with the pop-up
      rather than separate.  However then the support provided by
      core-doc is hard to provide.  Maybe there is a simpler way to
      arrange for that support.

Core features
-------------

- [X] nested notification of the same key should be disallowed somehow
- [ ] make ->parent 'safe', root points to itself, every pane *must* lead to root eventually.
- [ ] undo often doesn't leave me where I expect to be left. - maybe it is the
      location of the previous undo?
- [X] already have!!  a way to protect *Documents* from being killed manually.
- [ ] account all mem allocation types separate, and (optionally) report
      stats regularly
- [ ] malloc anti-fail policy.  Small allocations don't fail but use pre-allocated.
      large allocations use different API and can fail.
- [X] add '~' support for pathname lookup
- [ ] ... and $SUBST??
- [ ] graceful failure when closing doc that still has views.
      Then call doc_free() internally so the module doesn't need to.
- [ ] clarify and document the use of Notify:doc:Replace.  What are the two
      marks exactly.
- [X] possibly merge mark->refcnt, mark->owner, and Mark->local
- [X] Make x,y,z,h,w short ?? or unsigned short with an "undefined" instead of -1.
- [X] unify doc_next_mark_view and vmark_next.  Any others?
- [X] some way to find column of point, or at least: width of line
- [X] Change tlist to use one bit from each pointer
- [ ] Need a debug mode where every mark usage is checked for validity.
      also check the setref sets up all linkages.
- [X] clarify difference between Efail and Esys, or remove one of them.
- [X] replace key_add(..key_register_prefix) with a macro that defines a static modmap.
- [ ] Make key names shorter and easier.
      There are two name spaces - characters and names
      characters are preceded by '-', names by ':'.
      K-E is the key "Captial E"
      K:Enter is the key "Enter"
      K:M-n  is meta-n
      K:M:C:Up  is Meta-Control-Up
      K:M:C-U   is Meta-Control-U
      K:Cx-f    is Control-X f -- assuming K:C-x causes Cx to be set as prefix.
- [X] add key_add_prefix()
- [ ]  ?? restrict prefix key lookup to punctuation.

      Current ranges are:

       -  doc:  Request:Notify:doc: Call:Notify:doc: doc:set:
       -  attach- event: global-multicall- Request/call:Notify:global-
       -  multipath-this: etc
       -  Chr-space-~ \200...
       -  M-Chr-0 .. M-Chr-0
       -  Move-
       -  doc:notmuch:remove-tag
       -  Present-BG:

      Require ranges to have 4-char common prefix
      Add hash of 4-chars for ranges.  For a lookup. hash first-4
      and full.
      ... NO, I think 'hash up to - or :, both for key_add_range and for lookup

- [ ] revise and document behaviour of doc:open etc, particularly for
       reloading and filenames etc.
- [ ] Record where auto-save files are, and recover them.
        A directory of symlinks
- [ ] remove all FIXMEs (there are 55) ... and any HACKs (5).
- [ ] Replace asserts with warnings where possible.
- [ ] hide view-num inside pane so number cannot be misused.
     i.e. each view is owned by a pane and can only be used by that pane.

Module features
---------------

### emacs

- [X] Movement should always damage cursor.  Move-para and move-expr and
      even 'undo' don't (sometimes?)
- [X] entering an unknown name to find-document should either create a doc, or
      give an error, or something less silent
- [ ] show status line in file-edit popup
- [ ] filename completion should ignore uninteresting files like ".o"
- [ ] invent a way to reserve 'extra' values for command sets
      do I need this across panes ?? probably
- [X] search highlights don't cross EOL.
- [ ] search highlight doesn't report empty match (eol)...
- [X] reverse search "fails" for patterns starting '^' or ending '$'
- [ ] emacs highlight should get close notification from popup,
      instead of catching abort.
- [ ] ask before killing modified buffer.
- [ ] maybe meta-, does c-x` if that is the recent search?
- [ ] Support write-file (providing a file name) - currently I only save
      to the file I loaded from.
- [ ] C-uC-xC-v prompts for file name, like C-xC-v in emacs
- [ ] compare two panes somehow
- [X] copy/paste with mouse
- [ ] pipe doc or selection to a command, optionally capture to replace with output.
- [ ] if typing when selection active, replace selection with new text

### ncurses

- [X] add general colour handling to display-ncurses
- [ ] add full list of colour names (to lib-colourmap)
- [ ] if we exhaust colours or pairs, reset and make use lower color depth.
- [ ] handle !can_change_colors better.
- [ ] Allow different colour-maps per pane so full redraw
      happens when changing colour-map.  This makes images
      practical.
- [ ] automatically ensure the fg colour contrasts with bg, unless explicitly disabled.
      If bg is bright, reduce fg brightness.  If bg is dark, reduce saturation.
- [ ] maybe borrow xsel code to get direct access to X selection
- [ ] merge 'catpic' code to draw low-res images.

### pygtk

- [ ] convert to pygobject
- [ ] support tracking of mouse movement - for dynamic selection

### render-lines

- [X] Rules for when ignore_point gets cleared need to be worked out.
      c-mode/Move-Paragraph shouldn't need to call Move-to just
      to ensure cursor becomes visible.  Maybe DAMAGED_CURSOR
      should be enough?
- [ ] can render-lines ensure that lines appearing immediately
      before first line displayed, appear.  This is particularly
      important when first line displayed is(was) first line of file.
- [ ] make renderlines "refresh everything when point moves" optional.
- [ ] if flush_line from render_line() keeps returning zero, abort
- [ ] render-lines should always re-render the line containing point, so
      the location of “point” can affect the rendering.

### lib-input
- [ ] keep log of keystrokes in a restricted document
- [ ] support keyboard macros
- [ ] if a prefix is unchange for a short while, display it in the message line

### doc-dir

- [ ] how to change sort order of a directory listing
- [ ] chown/chmod/unlink/rename etc
- [X] times older than 1 year need to report the year.

### doc-text

- [X] add 'read-only' mode - warn when change attempted.
- [X] add 'file-changed' mode and check before switching to "modified"
      If changed, abort change and switch to read-only instead.
- [X] On visit, reload if file changed and doc not modified.
- [X] When writing to a changed file, make a backup first.
- [ ] support disable of undo in text, e.g. for copybuf document.
      I think this is a completely different doc type
- [ ] don't use mb* funcs, use bespoke utf8 coding
- [ ] merge adjacent undo records that can be merged.
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

- [ ] mouse selection should work in completion pane
- [ ] filename completion should work for earlier component of path.
- [ ] case insensitive substring match for fn or doc completion?
- [ ] The “complete” popup should be positioned above/below the file name,
      not over the top of it.

### view

- [ ] review use of line-drawing chars for window boarders
- [ ] improve scroll bars
- [ ] scroll bar should show part of doc displayed, not location of point.

### doc-rendering

- [ ] doesn't support highlights from marks
- [ ] maybe should highlight whole line that has cursor.

### grep/make

- [ ] if file isn't already loaded, wait until it is wanted, or something
      else loads it.
- [ ] if file is reloaded, re-place all the marks, don't just ignore them.
- [X] Keep 'make' and 'grep' output separate.
- [X] If there is an error followed by some 'note:'s, goto the last note(?)
- [X] leave marks at every match as soon as possible
- [ ] if two results are at the same location, ignore the second.
- [ ] clarify and document the role of numeric args to git-grep
- [X] make/grep - when insert at end-of-file, a pointer that was at EOF should
      stay there.
- [X] sometime the pane doesn't go to the right line.
- [X] move point of display pane to first match asap
- [ ] when restart compile/grep, kill only one.
- [ ] allow make even if not all files are saved - 'q' from save-all?
- [ ] numeric-prefix to make will auto-save everything.
- [X] make/grep: highlight current match.
- [X] make/grep fail-safe if target file doesn't exist
- [ ] run make in a given parent
- [ ] use notify chain to allow stack of 'greps'
- [X] detect and honour absolute file names in error messages
- [X] When choosing word from context to search for, if there is an
      active selection, use that.
- [X] And select the text in the input popup so that it is easy to replace.

### message-line

- [X] timeout message-line messages and revert to time/date
- [ ] have *Messages* buffer to log recent messages.
- [ ] Differentiate warnings from info, and blink-screen for warnings.
- [ ] register a global-message function which sends global notifications
      to all message lines.

### regexp

- [ ] '\' shouldn't be auto-inserted inside [] set.
- [ ] write an alternate back-tracking matcher which supports \n
      in the pattern.

### docs

- [ ] save-all should accept 'y' and 'n' as well as 's' and '%'
      and I probably want to be told what to do.  And '%' is probably not a good idea.
- [ ] docs_callback should (maybe) use a keymap

### shell mode

- [ ]  Use pattern-match on command to optionally choose an overlay
       which can highlight output and allow actions.
       e.g. (git )?grep   - highlight file names and jump to match
            git log  - highlight hash and jump to "git show"
            diff -u  - some diffmode handling
- [ ]  If no output, don't create a pane??  Or just one online.
- [ ]  Detect ^M in output and handle it... delete from start of line?
- [ ] when insert at end-of-file, a pointer that was at EOF should
      stay there.

###  edlibclient
- [ ] run edlib directly if no socket
- [ ] option to create a new frame
- [ ] more work on server mode:
- [ ] improve protocol
- [ ] allow restart (re-open socket)

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

- [X] I need more general support for rpos.  Does it only apply to point?  When rendered.
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

- [X] TAB should *always* delete preceding spaces.
- [ ]  auto-indent enhancements
      
     -   py: after "return" de-indent
     X   type : in C, not in quote, re-indent
     X   Indent of line starting '}' should match start of EXPR
     X   when type '}', decrease indent. in C
     -??  '*' should check for comment, and add a space
     X   Enter in comment should add a ' * ' for the indent.
     X   Backspace in the indent after a comment deletes too much
     X   Don't assume 'indent' is only tabs in python mode - round down to x4
     X   Be careful of brackets in comments and quotes.
     X   Try harder to find start of statement to align with
     X   handle comments
     X   detect end of statement in C (; } ) :
     -   A line after one ending ; or } or : or unindented is assumed to be
         correctly indented.
     X   We search back - skipping bracketed bit, until we find one, and
         base everything on that.
- [ ] configuration: use tabs or spaces for indent
- [ ] configuration: use only spaces for bracket-alignment indents - or tabs as well.
- [ ] python-mode: when changing indent, make same change to the whole block.
      Not sure how to handle 'else:' which looks like the next block.
- [X] show-paren should use different colour if bracket doesn't match.
- [X] show-paren should work when next char is an 'open'
- [X] start of function should not get confused by comments, and should
      go to the real start.
- [ ] re-indent statement - once auto-indent is reliable.

### lang-python

- [ ] key_register_prefix functionality.
- [ ] remove the pane 'handler' functionality after converting
      display-pygtk to make EdDisplay an edlib.Pane.

### white-space

- [X] support highlight of long lines
- [ ] support highlight of spaces-for-indent
- [ ] support highlight of tabs-for-indent
- [ ] make set of highlights, and colors, configurable
- [ ] support highlight for hard spaces
- [ ] support for blank lines near blanklines or start/end of file
- [ ] support highlight for 8spaces after a tab

New Modules - simple
--------------------

Possibly some of these will end up being features in other modules.

- [ ] create view-pane that either puts a cursor on whole line, or moves
      the cursor to the "right" place.  Maybe a markup to say "here is the
      preferred column" ??  Maybe use for make output so I can see current
      match more easily.

- [ ] dynamic completion Alt-/
      My question here is how far to search for completions.  All of this doc?
      Which other docs?  Most recently visited I guess.
      I think I want options to appear in a drop-down menu so I can select one.
- [ ] create a pane-type that just renders a marked-up line and use
      several of these for render-lines, and one for messageline.
      side-scrolling will be interesting.
      pane_clear might want to copy relevant region from underlying pane.
      Hopefully this will cure my performance problems with gtk on my slowish
      notebook.

- [ ] tags handling - and easy tag-search without tags. e.g. git-search.
      alt-S looks for TAGS or .git and either does a tags-search or a grep-l and
      check every match.  So maybe this is part of the 'make' module
- [X] white-space: Highlight trailing spaces - and space before TAB - and any TAB
      This might work best as a post-processor for the render-line output.
- [ ] simple calculator in floating pane.
      Must display result in hex and dec (and others?)
      Must allow hex/dec etc entry
      Allow chained expressions, with ability to edit earlier ones to change final result.
      Maybe each line is $N and typing '$' gets the most recent N, but Alt-P changes.
- [ ] menus
      This might support a menu-bar, or drop-downs for spelling or dynamic completion.
- [ ] hex edit block device - mmap document type
- [ ] spell check
      This leave attributes where errors are found, and needs to be notified of
      all changes so it can queue some checks.

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
events back and forth, and these event need to be standard commands communicating
between panes.  So it might be a variant for notifications.

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
  a view on a patch, or on a file at the time of the commit.
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

### wiggle/diff

The purpose of 'wiggle' is to apply a patch that doesn't fit perfectly.
So the editor would show were the best fit was, and what the problems are.
It would allow the problems to be corrected in various ways.

wiggle already has a built-in editor (--browse) which supports some of
this, but I find it hard to use.  Embedding wiggle into edlib should allow
me to re-think the problem and find out what works.

One thing to do would be to show the conflicts (why the patch doesn't apply)
and the needed changes.  If editing causes either of those to become empty,
it should be easy to apply or abort the hunk.  Note if, in either case,
the differences are just white-space.

### vi mode

I currently support emacs-like editing, but that is mostly kept local to one module.
I'd like to add a comparable vi module, partly because some people like vi
and partly because it would encourage me to keep the key-bindings cleanly
separate from the functionality.

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

### A suite of tools making use of some sort of "mark-down" like language


Restructured text? Markdown?  Commonmark?  Markright?

Having simple readable extensible markup is useful for writing
READMEs and TODO lists and emails and calendar entries and presentations
and all sorts of stuff.  I probably want to invent my own, because the
world always needs another markup language.

I want:

- easy render to PDF
- ASCII-Math or similar
- anchors for links, structure tags (author, title), foot notes,
  figure captures, tables, index, content.
- figure drawing.
- spreadsheet cells for auto calculations.
- outlining support of course.

Non-module functionality
------------------------

### IDE

To build an IDE with edlib there are various parts that are needed.
They wouldn't all be in one pane, but the various panes might work
together.

Functionality includes:

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


### TEST SUITE.  really really.

- record/replay input event sequence
- global setting to hide irrelevant detail like timestamps?
- command/key-stroke to scrape display to save.
      use mvin_wchstr
- This could be entirely inside ncurses.  It records events and content,
      and/or replay a previous recording.
- This has parts, so:
    - [ ] set of files to work with
    - [ ] identify source of uninterestin variability and allow them
          to be blocked
          - time in status line
          - full path name in find-file and docs listing
          - possible changes in support programs (grep?). Maybe make
            all "program output" be generated from saved files.
    - [ ] Make it easy to reply a test up to the point of failure, and watch
          the output.
    - [ ] harness for running tests
    - [ ] collection of tests

### gpg / compress / ssh file access

I don't know where this will fit in yet.

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
