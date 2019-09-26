To-do list for edlib
====================

Active issue and pre-requisites
-------------------------------

- [ ] change notmuch-query-view to use doc-rendering
- [ ] Discard rpos handling

Bugs to be fixed
----------------

- [X] start with "-g", open a -t, close the -g, then the -t : get into a spin
- [X] undo sometimes gathers too much into a single change.  Cursor movement
        should break the change. -- yank was the problem
- [X] when move down causes a scroll, we temp lose target column
- [X] Commands should *never* fall-through as marks get completely messed up.
      there are still places that try to protect against this.
- [X] mouse-click before the end of an active selection picks the wrong place.
- [ ] mouse actions need to affect selection: set or clear.
- [ ] detect when file has changed since it was read.
- [ ] alert when finding a name that is a link to another
- [ ] make sure *Welcome* has a dirname (it didn't once..)
- [ ] temp docs, such as pop-up input, can be inline with the pop-up
      rather than separate.  However then the support provided by
      core-doc is hard to provide.  Maybe there is a simpler way to
      array for that support.

Core features
-------------

- [ ] need a way to protect *Documents* from being killed manually.
- [ ] account all mem allocation types separate, and (optionally) report
      stats regularly
- [ ] malloc anti-fail policy.  Small allocations don't fail but use pre-allocated.
      large allocations use different API and can fail.
- [ ] add '~' support for patchname lookup - and $SUBST??
- [ ] graceful failure when closing doc that still has views.
      Then call doc_free() internally so the module doesn't need to.
- [ ] clarify and document the use of Notify:doc:Replace.  What are the two
      marks exactly.
- [X] unify doc_next_mark_view and vmark_next.  Any others?
- [ ] some way to find column of point, or at least: width of line
- [ ] Change tlist to use one bit from each pointer
- [ ] Need a debug mode where every mark usage is checked for validity.
      also check the setref sets up all linkages.
- [X] clarify difference between Efail and Esys, or remove one of them.
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

- [ ] revise and document behaviour of doc:open etc, particularly for
       reloading and filenames etc.
- [ ] Record where auto-save files are, and recover them.
- [ ] remove all FIXMEs ... and any HACKs.
- [ ] hide view-num inside pane so number cannot be misused.
     i.e. each view is owned by a pane and can only be used by that pane.

Module features
---------------

### emacs

- [ ] entering an unknown name to find-document should either create a doc, or
      give an error, or something less silent
- [ ] show status line in file-edit popup
- [ ] invent a way to reserve 'extra' values for command sets
      do I need this across panes ?? probably
- [ ] search highlights don't cross EOL.
- [ ] emacs highlight should get close notification from popup,
      instead of catching abort.
- [ ] ask before killing modified buffer.
- [ ] maybe meta-, does c-x` if that is the recent search?
- [ ] Support write-file (providing a file name) - currently I only save
      to the file I loaded from.

### ncurses

- [ ] add general colour handling to display-ncurses
      Allow different colour-maps per pane so full redraw
      happens when changing colour-map.  This makes images
      practical.

### render-lines

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

### doc-dir

- [ ] how to refresh a directory listing

### doc-text

- [ ] support disable of undo in text, e.g. for copybuf document.
      I think this is a completely different doc type
- [ ] doc-text: don't use mb* funcs, use bespoke utf8 coding
- [ ] merge adjacent undo records that can be merged.
- [ ] how to prune old undo history?
- [ ] report stats on:
        undo usage, chunk usage
- [ ] if 'find-file' finds same inode/inum, check the name is still valid.
       file might have changed (stg pop/push) underneath us.

### completion

- [ ] case insensitive substring match for fn or doc completion?
- [ ] The “complete” popup should be positioned above/below the file name,
      not over the top of it.

### view

- [ ] review use of line-drawing chars for window boarders
- [ ] improve scroll bars
- [ ] scroll bar should show part of doc displayed, not location of point.

### doc-rendering

- [ ] doesn't support highlights from marks

### grep/make

- [ ] clarify and document the role of numeric args to git-grep
- [ ] make/grep - when insert at end-of-file, a pointer that was at EOF should
      stay there.
- [ ] sometime the pane doesn't go to the right line.
- [ ] move point of display pane to first match asap
- [ ] when restart compile/grep, kill only one.
- [ ] allow make even if not all files are saved - 'q' from save-all?
- [ ] numeric-prefix to make will auto-save everything.
- [ ] make/grep: highlight current match.
- [ ] make/grep fail-safe if target file doesn't exist
- [ ] run make in a given parent
- [ ] use notify chain to allow stack of 'greps'
- [ ] detect and honour absolute patch names in error messages

### message-line

- [ ] timeout message-line messages and revert to time/date
- [ ] have *Messages* buffer to log recent messages.

### regexp

- [ ] \1 substitutions
      Maybe to extract a given submatch we have a third array pair where we record
      the length since a particular point.
      We then repeat the match process against the found string to get start
      and end points.
- [ ] rexel/rxl_advance: "clear out next lists" take too long on long patterns

### docs

- [ ] save-all should accept 'y' and 'n' as well as 's' and '%'
      and I probably want to be told what to do.
- [ ] docs_callback should (maybe) use a keymap

### shell mode

- [ ]  detect grep output and ?? goto line
- [ ]  detect diff (And git-diff) output and goto line
- [ ]  If no output, don't create a pane??  Or just one online.

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
- [ ] background color for current message/thread
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
- [ ] handle all unicode newline chars.
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

- [ ] I need more general support for rpos.  Does it only apply to point?  When rendered.
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
     when count noticies a difference, it should trigger a refresh
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
- [ ] should doc attribtes append to defaults, or replace?
- [ ] word-wrap.  Interesting task for centering
- [ ] force x:scale to be maximum width of any line - to avoid surprises
- [ ] proportial vertical space ??
- [ ] right:0 and right:1 don't do what I expect
- [ ] thumbnails for easy select?
- [ ] \_  etc to escape special chars
- [ ] boiler-plate, like page numbers

     - Maybe stuff before "# " is copied everywhere.
     - Need magic syntax for fields ##page#

### C-mode

- [ ]  auto-indent enhancements
      
     -   after "return" de-indent
     -   type : in C, not in quote, re-indent
     -   Indent of line starting '}' should match start of EXPR
     -   when type '}', decrease indent. in C
     -  '*' should check for comment, and add a space
     -   Enter in comment should add a ' * ' for the indent.
     -   Backspace in the indent after a comment deletes too much
     -   Don't assume 'indent' is only tabs in python mode - round down to x4
     -   Be careful of brackets in comments and quotes.
     -   Try harder to find start of statement to align with
     -   handle comments
     -   detect end of statement in C (; } )
     -   A line after one ending ; or } or : or unindented is assumed to be
     -   correctly indented.
     -   We search back - skipping bracketed bit, until we find one, and
         base everything on that.

- [ ] show-paren should use different color if bracket doesn't match.

New Modules
-----------

- [ ] create view-pane that either puts a cursor on whole line, or moves
      the cursor to the "right" place.  Maybe a markup to say "here is the
      preferred column" ??  Maybe use for make output so I can see current
      match more easily.

- [ ] search-replace
- [ ] dynamic completion Alt-/
- [ ] create a pane-type that just renders a marked-up line and use
      several of these for render-lines, and one for messageline.
      side-scrolling will be interesting.
      pane_clear might want to copy relevant region from underlying pane.

- [ ] fill command / fill-mode

    - need to choose a prefix of first line, and of subsequent lines.
    - alt-Q - reformat para

- [ ] copy/paste with mouse
- [ ] threaded-panes

    - useful for multiple event loops, so edlib doesn't have to use gtk
    - probably important part is sending messages between threads.
    - probably don't want shared data structures
    - so maybe a pane represents an alternate thread which only access
      through that pane
    - pane can also represent a connection to another program, or host.

- [ ] TEST SUITE.  really really.

    - record/replay input event sequence
    - global setting to hide irrelevant detail like timestamps?
    - command/key-stroke to scrape display to save.
         use mvin_wchstr
    - This could be entirely inside ncurses.  It records events and content,
         and/or replay a previous recording.
    - This has parts, so:

           - harness for running tests
           - collection of tests

- [ ] - tags handling - and easy tag-search without tags. e.g. git-search.
      alt-S looks for TAGS or .git and either does a tags-search or a grep-l and
      check every match.  So maybe this is part of the 'make' module
- [ ] white-space: Highlight trailing spaces - and space before TAB - and any TAB
- [ ] interactive shell / terminal emulator

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

- [ ] IDE

    - build
    - search
    - syntax highlight
    - indent
    - outline
    - gdb


- [ ] Outline code rendering.

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


- [ ] simple calculator in floating pane
- [ ] Markdown/rst/Markright

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

- [ ] calculator/spreadsheet using markX
- [ ] outlining with markX
- [ ] git-mode

    -  easy rebase, saving select changes to each commit
    - list of recent commits allows editing of comment
    - also allows rebase
    - diff output allows selective add
    - diff output in different colors depending one whether staged or not.
    - jump from diff output to file, and update quickly


- [ ] email composition
- [ ] hex edit block device - mmap document type
- [ ] dir-edit
- [ ] menus
- [ ] config
- [ ] vi
- [ ] ssh file access
- [ ] wiggle/diff
- [ ] gpg / compress /
- [ ] spell check
- [ ] calendar/diary/planner
- [ ] a “reflection” document so I can view the internal data structures.

