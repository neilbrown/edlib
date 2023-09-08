To-do list for edlib
====================

Current priorities
------------------

All of these, except Triage, are references to todo items elsewhere in
the file.

### Triage

- [X] find-document - if default doc has <>, displays wrongly.
- [X] From start-of-file move to end, then up, then down.
      Display jumps.  Why?
- [ ] Add menubar menu with recent documents?
- [X] why does clicking on status line go to top-of-file?
- [ ] should Docs menu go on doc name in status bar?
- [X] search hangs when seeking "^( *)"
- [ ] selection-menu item to show git-commit from list of known git
      trees
- [ ] selection-menu item for word-count
- [ ] selection-menu item for QR-code
- [ ] selection-menu sub-menu for UPPER lower Caps ??
- [ ] selection-menu item for text-fill
- [ ] selection-menu item for spell-check ??

### Small

- [X] notmuch addresses in From: list to have menu to add address to any
      from-* query
- [X] Disable <hide> if cursor is in the hidden region.
- [X] fill mode to handle all punctuation at start of this line
- [X] Enable lib-menu to show short-cut keys
- [X] Add menu-bar to lib-menu.  Pop it up on F10 with simple commands
- [X] attach an extensible menu to the selection
        cut, copy, paste-in, QR, git-view

### Medium

- [ ] split range management out of autospell so it can be used by other
      modules.
- [ ] make it easy for a make-search command to search backwards
- [ ] Make a start on CUA mode with mouse/menu/selection support.
      Also Function keys: help, close, refresh

### Large

- [ ] image-display pane
- [ ] git-mode
- [ ] render-markdown.py
- [X] lib-menu
- [ ] Remote display

Requirements for a v1.0 release
-------------------------------

- [X] logo!!!  to use as icon in X11 for example.  Building blocks?
      Window pane?
      Maybe a pencil in a window pane
- [ ] configuration
- [ ] vi mode
- [ ] CUA mode
- [ ] nano mode(?)
- [ ] multiple front ends: elvi, elma, elnm, eled, elpnt?
- [ ] introspection
- [ ] markdown editor (with PDF output)
- [ ] non-line-based render, such as a tabular render for spreadsheet,
      or side-scrolling horiz row of thumbnails for presentation, or
      even a menu-bar with row of menus.
- [ ] documentation reader
- [ ] block-dev (mmap) doc type, and some hex-mode support
- [ ] user documentation
- [ ] developer documentation
- [ ] some git support

Core features
-------------

- [ ] If a pane wants to block mouse events from parents, as lib-view
      does, it shouldn't need to catch all the combinations, or it
      should be much easier
- [ ] gather memory usage stats per-pane and allow a dump
- [ ] show doc size in doc list - include undo size?
- [ ] Ensure all panes that should use "Free" properly, and find some
      what to encourage its use.
- [ ] Add optional unit-test interface for modules.  This should be
      implemented at least by lib-search, doc-text and probably many
      others.  It is particularly for things that are awkward to test
      with the ncurses/replay test approach.
- [ ] Send global notify before/after refresh.  LOG must suspend logging
      (or notifications at least) during refresh if is visible anywhere
- [ ] Do I want "Display" as in "Display:close", or "window" as in
      "window:notify".  Decide, and make everything consistent.
- [ ] Do I really need global-multicall- or can I just use
      notifications.
      It would mean more modules would need a private pane, but might
      I want that to support module unload anyway?
      multicall- is currently used for:
      - open-doc- to find something to open a given file type
          doc-dir doc-email doc-text
      - doc:appeared- to configure doc based on name
          doc-docs hex c-mode viewer autosave calc mergeview config presenter
      - startup- for initialising editor
          autosave server

- [ ] if a view creation fails - like render_master_view_attach
      hitting a python error - there will be no active view so the
      screen will be meaningless.  I need to properly abort and
      auto-choose a new pane.
- [ ] LOG_BT() doesn't see TYPE_pane and TYPE_comm calls.
- [ ] give every pane a link to root/editor main and use that
      instead of statics.  Then maybe times_up() can use pane_too_long()
- [ ] teach input to allow a repeat command to be registered so that e.g.
      search/replace and do a bit of work, then ask to be called again.
      input can cancel this on suitable input.
- [ ] should input leave mode unchanged for key that isn't handled at
      all
- [ ] marks can be used after they go invalid too easily.  How to fix??
- [ ] catching doc:replace in a pane doesn't catch doc:insert-file.
      I need a simple way to intercept any change.
- [ ] switch-buffer in pop-up window - shouldn't kill the popup (does it?)
- [ ] explicitly guard against infinite loops in consistency checks
- [ ] skip consistency checks after several with no stop for input.
- [ ] unwanted docs too easily appear high in recent-list - *Output*
- [ ] design a way for a keystroke to interrupt a long-running function.
- [ ] extend Draw:measure protocol to allow constant-width-fonts to
      cannot-scale displays can be detected and measurement optimised for.
- [ ] improve timeout.  Set timer once, then set a flag so that all commands fail
      until some top-level clears the flag.
- [ ] reconsider all 'return comm_call()' calls.  Do we every really
      care if the callback succeeded?
- [ ] Change Efallthough to -1 so I can return '0' meaningfully.
      Efalse probably becomes 0.
- [ ] send warning message when recursive notification is prohibited.
       editor:notify:Message:broadcast
- [ ] Make DEF_CB really different from DEF_CMD and ensure it is used properly.
- [ ] is DocLeaf really a good idea?  Maybe panes should have 'leafward'
      pointer separate to 'focus'?  Maybe panes could have optional
      'child' method which returns main child - pane_leaf() calls that.
      Maybe pane_leaf() find a pane with z=0 and matching w,h ??
- [ ] support text-replace as easy as text-insert (doc:char...)
- [ ] for doc:cmd transformation,  what about :Enter and BS TAB ESC ???
- [ ] For a notify handler, returning non-zero doesn't stop other handlers
      running.  For a call handler it does.  This inconsistency is awkward for
      messageline_msg which wants to allow fallthrough, but needs to acknowledge.
      How can I resolve this? Use Efallthrough as -1.
- [ ] make a doc read-only if dir doesn't exist or isn't writable
- [ ] account all mem allocation types separately, and (optionally) report
      stats regularly
- [ ] document the use of doc:replaced.  What are the two
      marks exactly? start and end of range.  Verify all clients and providers
      The 'num' is for when mark2 is absent and it suggests a number of bytes
      that might have changed.
- [ ] revise and document behaviour of doc:open etc, particularly for
       reloading and filenames etc.
- [ ] review all aspects of mark lifetime.  ->owner must be set for
      points and vmarks.  A non-owner may never hold onto a mark beyond
      the call which gave access to it, unless it registers for
      notifications from the owner, and that probably only applies to
      points.

### Longer term

- [ ] Make it possible to unload C modules when refcount on all commands
      reaches zero
- [ ] Make it possible to unload Python modules
- [ ] Malloc anti-fail policy.  Small allocations don't fail but use pre-allocated.
      large allocations use different API and can fail.
- [ ] support $SUBST in file-open path names ??
- [ ] Need a debug mode where every mark usage is checked for validity.
      also check the setref sets up all linkages.
- [ ] remove all FIXMEs (there are 65) ... and any HACKs (2).
- [ ] Replace asserts with warnings where possible.
- [ ] hide view-num inside pane so number cannot be misused.
     i.e. each view is owned by a pane and can only be used by that pane.

Module features
---------------

### lib-menubar

- enable activation via F10.  Cancel makes it disappear if configed
  not too.  How to keep??
- allow left/right arrows to move between menus
- emacs to disable 'save' when cannot be saved.

### lib-qrcode

- [ ] pop up window to show selection as QR code
- [ ] text qrcode (qr --ascii foo) don't look right in xcb display

### workspaces

- [ ] I think I want a "Workspaces" concept, maybe tabbed set of tile
      collections.  I would have one of mail and one for each project
      that I might be looking in.  I have lots of documents, but few
      projects so this would be easier to move between.
      "grep" and "make" etc don't care which document in a project they
      are in.
      "nm" already gets me to the email project quickly.  Getting back
      is not so easy.
      Maybe some docs could be marked "project" and so be easy to find?
      Maybe docs could be associated with a session, and are preferred
      for select-new-document operations.

### render-stack

   My idea is to have a generic rendering pane which draws row or column
   of other panes.  Maybe a doc-list describes those panes with attrs
   identifying the doc, the pos in the doc, the view.
   Marks only identify the panes, not anything in the pane. (maybe).
   I want to use this for:
        - menu bar - as stack of menu triggers
        - presenter thumbnails
        - image thumbnails

   However this *could* be render-lines which stacks lib-renderline
   panes all on the same document.  This wouldn't use an attribute to
   identify the location in the doc, so I'm not 100% sure of this

### lib-askpass

- [ ] have askpass tell shellmode (and others) about the SSH_ASKPASS*
      environment, so they don't need to know it themselves
- [ ] start-agent command which runs ssh-agent can records the
      environment reported for later shells.

### lib-linecount

- [ ] handle view:changed properly, and make sure total count changes
      appropriately in notmuch-query-view.  Maybe view:changed should
      report if content changed, or just attributes.

### lib-server

- [ ] ctrl-z in elc doesn't ask edlib to release the terminal
- [ ] do we need both .term and .disp?  When are they different?

### lib-x11selection-xcb

- [ ] will need to listen for property-change-event on requestor to know
      when result has been deleted to handle INCR.
- [ ] if too big, or alloc error, switch to INCR more for sending
- [ ] support INCR mode for receiving
- [ ] use iso-8859-15 for some of the text formats.
- [ ] test very large copy/paste
- [ ] need a queue of pending selection requests

### lib-textfill

- [X] auto-wrap on a line like this one doesn't recognize all the
  punctuation a the start of the line ... should it?
- [X] fill mode to handle all punctuation at start of this line

### render-format

- [ ] improve caching of attributes
- [ ] profile performance to find opportunities for optimisation.

### lib-search

- [ ] make it easy for a make-search command to search backwards

### autosave

- [ ] if multiple files are opened quickly (e.g. by grep), we might get cascading
      autosave prompts.  Introduce a mechanism to queue them and only have one per
      display

### tile


### rexel

- [ ] move to separate git repo and document well.
- [ ] review return code of rxl_advance().  What should be
      returned if a flag allowed a match, but the char didn't.
- [ ] add self-tests which ensure that the return value is correct.
- [ ] support backward search by compiling expression backwards
- [ ] make prefix-fast-search work for case-insensitive matches??
- [ ] rexel term for "expression" with balanced brackets.
      Also quotes and maybe <>.  Possibly a list of pairs?

### popup

### lib-wiggle

- [ ] maybe find matches considering only alphanum first, then
      refine the gaps considering punctuation.

### lib-diff

- [ ] highlight white-space errors.
- [ ] command to apply a hunk to a given document - or to reverse it.
      How much of a hunk?  Selection?  How to record which hunks are done?
      How to identify document?  Maybe I want a generic "Other" document where
       patches are applied, diffs are calculated, etc
       If only two panes, then "other" is clear, else it must be marked with C-x-7??
      'a' to apply current hunk
- [ ] Link wiggle code to find best-match if direct match fails
- [ ] command to find best 'wiggle' match, and another to apply it if no conflicts.
- [ ] command to move to matching place in other branch Cx-Cx if mark not active??

### lib-mergeview

- [ ] Menu of options somewhere to remind me.
- [ ] mergeview command to show diff between "found" and "replacement".
- [ ] mergeview command to include both found and replacement, discard
- [ ] Consider what else can be done with < and > on other markers.
- [ ] Capture a new doc-reappeared for when reloaded, and search again
      for markers.

### emacs

- [ ] multi-file search-replace.  Find files with 'git-grep'
- [ ] there is no way to count characters in a range, or find how many
      characters into the document I am.  Maybe bytes would be good too.
- [ ] :CX-= to look-up unicode name of character if not ASCII
- [ ] ":C-q ?" or "- :C-q" to open a find dialog with list of unicode
      chars.
- [ ] split some generic functionality like arrows and mouse clicks
      into a separate module to be shared with other edit modes.
- [ ] Do I want a 'truncated' marker at start/end of line when not
      wrapping, and does the '\' go away properly when I start shifting.
- [ ] change shift left/right to work in characters rather than pixels
- [ ] filename completion should ignore uninteresting files like ".o"
      Maybe use .gitignore, or have config module understand that.
- [ ] maybe alt-, does c-x` if that is the recent search?
- [ ] C-uC-xC-v prompts for file name, like C-xC-v in emacs
- [ ] compare two panes somehow - new lib-compare function??
- [ ] file in pop-up window in 'view' mode by default
      From 'grep' this is probably OK.  For Cx-44, it isn't.

##### needs design work

- [ ] search/replace should support undo somehow
      I can already step out, undo, step back.  What more?  Maybe Alt-U (uppercase)?
- [ ] What should be passed to M-x commands?  prefix arg?  selection string?  point?
       Surely everything.  Prefix if present, string if active, point always.

#### history

- [ ] history doesn't restore initial default (or empty) string when
      returning after excursion into history or favourites.
- [ ] how can we save history-favourites back to a config file?

### ncurses

- [ ] What to do with Focus-in/out keys?
- [ ] add full list of colour names (to lib-colourmap)
- [ ] allow a pane to require 'true-colour' and discover number of colours available
      Colour map gets changed when it becomes the focus.
- [ ] When only 16 colours, maybe add underline when insufficient contrast available.
- [ ] automatically ensure the fg colour contrasts with bg, unless explicitly disabled.
      If bg is bright, reduce fg brightness.  If bg is dark, reduce saturation.

### pygtk

- [ ] can we capture the substates of character composition, and give feed-back?

### display-x11-xcb

- [ ] Add test option, fetching image back from server after 'post'
- [ ] would GraphicsMagick be better than ImageMagick?
- [ ] share connections among multiple windows

### render-lines

- [ ] improve Move-line in multi-line renders (images). prev
      must only move up one line, and moving down should start
     in column
- [ ] the background "call:" option should report if background was changed.
      An attribute could store chosen information for comparison.
      Would need a 'force' flag.
- [ ] I regularly hit problems because ->mdata is not up to date and we render
      to find a cursor and compare with ->mdata and get confusion.  How can I avoid this?
- [ ] view:changed shouldn't destroy the view, else Move-CursorXY
      gets confused.
- [ ] make renderlines "refresh everything when point moves" optional.
- [ ] if flush_line from render_line() keeps returning zero, abort
- [ ] render-lines should always re-render the line containing point, so
      the location of “point” can affect the rendering.

### lib-renderline

- [ ] Teach render-lines to pad spaces to left/right align text
- [ ] Maybe "word-wrap" should be a list of characters to wrap on??
      But I might want "comma and following spaces".  How do I say that?
      Possibly declare some chars that cause a map-attr callback??
- [ ] renderline *knows* about scaling and when it places the cursor
      in an image, it gets it wrong for ncurses.  It should ask about
      scaling.
- [ ] if the first char if a line is marked for <wrap> it disappears
      without cursor.  This is bad.

### lib-markup

- [X] Move <hide> handling to lib-renderline so we can disable <hide>
      if cursor is in the hidden region.

### lib-macro

- [ ] don't allow starting macro inside a macro
- [ ] detect errors including Abort and search failure etc. Abort capture or
      replay on error
- [ ] Possibly wait for a shell-command etc to complete before continuing.

### doc-dir

- [ ] if fstatat() fails for an entry, zero out the stat buf, and possibly
      schedule a reload of the directory (but not too often).
- [ ] allow setting a pattern, as alternate to substr, for 'complete'
      viewer.
      I think this would be more efficient than the current linefilter.
      We could have a doc-filter which adds the pattern to doc:char
      commands so forward/backward stepping is v.fast.  The pattern
      could even be a pre-compiled rexel command.
- [ ] support directory views for sorting.  I think this requires
      a separate dir document, which borrows state from the main one.
      It definitely needs an independent set of marks, so that means
      a separate document.  Using the same dir_ent content might help
      save space, but then we need an index separate from the dir_ents.
      Maybe that is OK.  We could have linked lists of small arrays.
      Marks would point to entries in the array and we walk back to find
      find start: low-bit-set.  So each array is fore, back, 14 dir_ent
      pointers.  Or use aligned_alloc() and mask out unwanted bits.

### doc-text

- [ ] things slow down after lots of edits.  Maybe track
      number of chunk, marks, undos etc and display them somewhere
- [ ] stop consistency checking a doc when it gets "big" ??
- [ ] avoid infinite loops in consistency checks
- [ ] doc-text: opening file with e.g. 200,000 lines is very slow
      Check this..
- [ ] moving in a big file is slow - check this
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

- [ ] The “complete” popup should be positioned above/below the file name,
      not over the top of it.

### lib-view

- [ ] review use of line-drawing chars for window boarders
- [ ] improve scroll bars
- [ ] review decision about that to do when high < 3*border-height.
      Current (disabled) code makes a mess when differing scales causes
      borders to be shorter than content.

### grep/make

- [ ] make uses too much CPU on large output
- [ ] Need keystroke to step through different grep/make windows
- [ ] if file isn't already loaded, wait until it is wanted, or something
      else loads it.
- [ ] if file is reloaded, re-place all the marks, don't just ignore them.
- [ ] clarify and document the role of numeric args to git-grep

### message-line

- [ ] reset timer each time so that message stay an exact period, and
      timestamp updates on the minute.
- [ ] allow marked-up messages to be given.
- [ ] Differentiate warnings from info, and blink-screen for warnings.

### docs

- [ ] accessing document list can be slow.  Same for large directories

### hex

- [ ] improve doc:replaced handing, by tracking the visible region and
      checking if a replacement changes the number of chars.

### shell mode

- [ ] allow input.  Can I use SIGTTIN?  Need to ignore SIGTTOU, attach
      task to pty and catch when SIGTTIN is received.  Then switch back
      pgrp, restart, and feed content.
      Any chars from ' ' to '~' are queued in a separate buffer shown at
      bottom.  It can be edited and :Enter will submit it.
      Could just use pipes and FIONREAD to detect when content is slurped.
- [ ] non-utf8 in output makes python spit the dummy
- [ ]  Use pattern-match on command to optionally choose an overlay
       which can highlight output and allow actions.
       e.g. (git )?grep   - highlight file names and jump to match
            git log  - highlight hash and jump to "git show"
            diff -u  - some diffmode handling
- [ ]  If no output, don't create a pane??  Or just one online.
- [ ]  Detect ^M in output and handle it... delete from start of line?
- [ ] always track time for a run and report it - or at least make it available

###  edlibclient
- [ ] Catch broken-pipe in all sock.send calls
- [ ] run edlib directly if no socket
- [ ] option to create a new frame
- [ ] more work on server mode:
- [ ] improve protocol

### line count

- [ ] Find a way to locate mark faster than walking the whole list
      adding up the interval counts.  Add a second mark list as a
      'skiplist' with 1 mark for every 100 of the main sequence.

### lib-utf-8

- [ ] lib-utf8 takes chars, not bytes, so it doesn't work over
      a utf8 document

### doc-email

- [ ] if charset module doesn't load, things go very bad. (utf-8 failure)
- [ ] use mimetypes.guess_type() to interpret filenames in email attachments??
- [ ] don't allow non-text email parts to appear as text.  Maybe hex??

### Notmuch - overview

- [ ] When moving among messages, always move cursor to search pane
- [ ] 'other' notmuch search doesn't show older messages sometimes
- [ ] split notmuch into two databases, last 6 months and the rest.
- [ ] script to move messages every week - but not deleted messages
  Maybe not spam either
- [ ] edlib to offer to search older messages

- [ ] Email summary line for single-message threads should show size??
- [ ] Only clear "new" tag on explicit quit, not when going to
      other search or simply closing the window
- [ ] 'm' in 'move marks on tid to before self.pos' was reportedly NULL once.
- [ ] Two threads with same timestamp swap order on reload
- [ ] When main_doc notices counts increase, it should ensure the next
      visit of the search triggers a refresh.
- [ ] all shares-ref docs must be careful about comparing marks ... or else
      we need to compare ignoring offset?  Best to compare <, not <=??
- [ ] If I open a search but there are no messages (yet) I get a python
      error that select-message failed
- [ ] saved queries that are not mentioned in any other query should get
      presented in the search list. ... except maybe current/unread/new ???
- [ ] if a thread matched query and so is still cached, but no
      individual messages match any more, then whole thread is shown.
      This is confusing.  Maybe we report an error when open is attempted,
      but somehow allow 'Z' to still work.
- [ ] purge old entries from query when updates but not being viewed
      ... don't we already do this?
- [ ] updating tags can take long when 100s.  Enable background queuing of these.
- [ ] Don't display query entries that have a 0 match count.??
- [ ] update counts more often when a query is being changed.  e.g. when any change happens
      while a query is open, schedule an update in 2 minutes.
- [ ] handle errors better.  e.g. file reported by notmuch might not
      exist, or not be readable
- [ ] allow opening drafts in composer on restart.
- [ ] allow deleting of drafts without posting.  Maybe just 'delete'..
- [ ] When active query changes, highlight on list view doesn't immediately
      follow
- [ ] TESTS
- [ ] make sure Clone actually works for all panes - or remove it
- [ ] add counter and colour for 'flagged'
- [ ] if no 'query.current' use "not exclude_tags"
- [ ] support selection messages and applying tags
- [ ] When changing any tag in a thread, or when opening the thread,
      assess thread tags by looking at all matched messages.
- [ ] make min top/bottom margin configurable, set for message list
- [ ] display counts of current thread somewhere, so I know where I'm up to. - new/unread/matched in status line
- [ ] review highlight on query when the message selected isn't the message displayed
- [ ] fix bugs in stored-query!! query: is slow and (I think) buggy
- [ ] rel_date could report how long until display would change, and
   we could set a timer for the minimum.
- [ ] allow re-ordering of saved-search list click-drag? +/-?
- [ ] allow editing of saved searches, including deletion
      must support undo. % for replace?
- [ ] make sure doc cleans up when closed. processes must be killed
      and query docs must be closed
- [ ] Can I implement "undo" for large-scale tag changes?
      Maybe don't write them to the DB immediately??
- [ ] Can I fix notmuch to extract resent-foo headers for searching?

###  Notmuch message display

- [X] notmuch addresses in From: list to have menu to add address to
      any from-* query
- [ ] "%d quoted lines" still not quite right.  Moving 'down' past it
      jumps to end of line.
- [ ] Don't wrap email header lines when cursor isn't on the line - too noisy
- [ ] word-wrap subject (only) in email summary line
- [ ] check for Efail errors from doc:open
- [ ] make it practical for 'text' documents to contain non-utf8 so that
      "Save" can copy to a buffer.  There is some support for a charset
      to be "8bit" (hex-mode only).  Maybe that is part of the answer.
- [ ] error check 'external viewer' code
- [ ] add module for external-view which creates a unique temp file and
      removes it when the viewer is done.
- [ ] check for "Command Line Error: Incorrect password" from pdf, and
      ask for password
- [ ] delay conversion until unhide
- [ ] detect Content-disposition, use for filename, and hide anything
      that is an attachment
- [ ] detect char-width and suppress images if 1 or 2
- [ ] create general choose-file pane which can seek an existing, or
      non-existing file.  Allow a default dir which can be remembered.
      Use this for Emacs, and for saving attachments
- [ ] when unhiding for a alternate part, hide any others.
- [ ] next part/prev part button on spacer
- [ ] closing a large section pushed cursor to top of display, which
      isn't really what I want.  I'd rather the cursor stayed still.
- [ ] in notmuch I searched in a message (mimepart), then enter to choose,
   then 'q' and crash.
- [ ] A multipart still had an active view.
- [ ] when I unhide an email part which is a single v.long line,
    redraw gets confused and point goes off-screen, which seems
    to leave it confused.
- [ ] maybe hide signature, unless small
- [ ] When click on first char in tagged range, I don't see the tag and
   don't get a Activate event.
- [ ] line wrap in header should not appear as space??
- [ ] handle all Unicode newline chars.
- [ ] Auto-hide depending on type - with extensible table
- [ ] Open-with always,  Open only if a handler is known
- [ ] "save" to copy to buffer
- [ ] save function - doc:save-file given file name or fd
- [ ] wiggle-highlight patches found in email
- [ ] detect and hide cited text
- [ ] maybe detect "-----Original Message-----" as indicating cited text
- [ ] Make long to/cc headers truncate unless selected.
- [ ] Make addresses active (menu?) to allow adding to a saved search
      with options and/or/andnot.  Also "mail to" or "save"..
- [ ] Allow any selection to be added to a saved search.
- [ ] verify signature if present
- [ ] decrypt if needed
- [ ] treat message/RFC822 much like multipart

### Notmuch composition

- [ ] when aborting email composition, unlink the file if it is
      empty.  Probably dispose of autosave too.
- [ ] should I look for Delivered-to headers. Even;
         1. To, Cc, Bcc, Reply-To, From
         2. Envelope-To
         3. X-Original-To
         4. Delivered-To
         5. Received (for)
         6. Received (by)
         7. configured primary address

- [ ] sanity check message:
      - body/subject/to not empty
      - only 1 'to' or 'cc'
      - provide 'sender' if multiple 'from'
      - word 'attach' without attachments
      - message has already been sent
- [ ] catch exceptions from email.message creation.
       particularly adding headers can complain
- [ ] be smart about quoting displayname before <addr>
- [ ] capture editing of to/cc and mark ',' as a wrap point.
- [ ] If attachments are requested, set mime-version etc
- [ ] inline images get displayed
- [ ] auto-insert signature... like an attachment?
- [ ] address-completion should be referred to module, not assumed to be notmuch
- [ ] support address book and allow completion from there
- [ ] markdown mode that creates HTML?
- [ ] encryption and signing
      gpg --no-tty --pinentry-mode=loopback .....
       will cause an error "gpg: Sorry, no terminal at all requested - can't get input"
       if it needs to prompt for a passphrase.
      If that happens then
       DISPLAY= GPG_TTY=/dev/whatever gpg .....
      will use the tty to ask for a password.
      But need to specifiy a suitable pinentry program via
       ~/.config/systemd/user.control.gpg-agent.server.d/pinentry.conf
          [Service] \ ExecStart= \ ExecStart=/usr/bin/gpg-agent --supervised --pinenty-program ...

### Presenter

- [ ] split into lower pane which parse markdown and upper which handles presentation.
- [ ] translucent bg colour for paragraphs
- [ ] partial-view-points. Only render beyond here if mark here or beyond.
    page-down goes to next such point
- [ ] temp attribute.  :bold: etc only apply to para, :: is appended to para format
- [ ] should doc attributes append to defaults, or replace?
- [ ] word-wrap.  Interesting task for centring
- [ ] force x:scale to be maximum width of any line - to avoid surprises
- [ ] proportional vertical space ??
- [ ] thumbnails for easy select?
- [ ] \_  etc to escape special chars
- [ ] boiler-plate, like page numbers

     - Maybe stuff before "# " is copied everywhere.
     - Need magic syntax for fields ##page#

### render-markdown.py

- [ ] parse markdown adding attributes for improved display
     - alignment?  Hide newlines and get render-line to wrap?
     - centring
     - Large text for headings
     - list bullets
     - non-cw font
     - bold, italic, colour?
     - code, quoted, images
     - urls and other references?
- [ ] Make sure editing causes quick re-rendering
- [ ] inline images - so welcome can have a logo
- [ ] consider following MyST
        https://jupyterbook.org/en/stable/content/myst.html

### C-mode

- [ ] in parse_code 'c' might be None - need to check
- [ ] if .. else switch adds into to the switch.  Should it?
- [ ] auto-indent enhancements: '/' should see if at start of comment line
       following '* ', and discard space?
- [ ] A line after one ending ; or } or : or unindented is assumed to be
         correctly indented.??

- [ ] configuration: use tabs or spaces for indent
- [ ] configuration: use only spaces for bracket-alignment indents - or tabs as well.
- [ ] python command to shift indent of range.
- [ ] python-mode: when changing indent, make same change to the whole block.
      Not sure how to handle 'else:' which looks like the next block.
- [ ] in python mode, a comment at the end of an 'if' block confuses indenting.
      next line cannot go back one level

### git-mode

- [ ] log view (:C-c l?) which uses --max-count and --skip to only
      collect enough log entries to fill the display.  Or better: have a
      shell mode which only reads from pipe if insertion position is
      visible.
- [ ] from log-view, view a commit.  From a commit, view the file
      at that time.  Maybe 'save' creates a fixup commit which is
      added to the log.
- [ ] Have a list of paths are top of log and any commit which doesn't
      touch the paths is greyed out.  "git log --oneline --stat"
      provides the info, some of which we hide.
- [ ] when a git-managed file is saved, if there is a "diff" view, it is
      updated.  Maybe show both cached and uncached in different colours.
- [ ] action from log-view to reword and commit - with auto rebase and
      update of the view
- [ ] actions to re-arrange and deleted the commits.  Integrate with
      git-rebase...  or simulate git-rebase with "git worktree add
      --no-checkout" on the most recent unchanged commit, then adding
      each required commit with git-apply--cached and git-commit
- [ ] diff-mode editing enhancements:
      - kill the file marker (+++) kills all chunks in the file
      - kill the chunk marker (@@) kills the whole chunk
      - insert line in chunk gets '+' prefix and updates line counts
      - delete line in chunk updates line counts
      - editing in chunk re-runs word-diff
- [ ] git-commit command (:Commit) which presents the patch and allows
      it to be edited (with consistency checks and number updates).  On
      :Commit the patch is applied with "git apply --cached" an if
      successful the message is added with "| git commit -F"

### lang-python

- [ ] review python doc:char implementations for simplification.
- [ ] repeated alarm(10)/alarm(0) calls slow things down
- [ ] array index should allow two args, second being a mark for
      doc:get-attr etc.
- [ ] should be able to test if a mark is NULL or Freed
- [ ] should Efallthrough be an exception?
- [ ] Log loading of modules - Can I provide version info?
- [ ] we aren't catching errors from functions called from .connect()
       Maybe use sys.excepthook(typ,val,tb)
- [ ] Add version info to python modules

### white-space

- [ ] don't show errors unless doc has been changed.???
- [ ] highlight of adjacent blank lines isn't removed if first has text added
- [ ] support highlight suitable for diff: a space is first character is allowed,
      even if EOL or followed by space.
- [ ] make set of highlights, and colors, configurable

### test suite

- [ ] always run with timezone of Australia/Sydney
- [ ] add test infrastructure to display-x11
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

- [ ] unify UI with spell

### spell-checker
- [ ] mode-specific so latex can ignore \foo
- [ ] Some way for 'c-mode' to report where comments are so they can be spell-checked
- [ ] drop-down with options
- [ ] unify UI with dynamic-completion
- [ ] split range management out of autospell so it can be used by other
  modules.

### calculator
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
- [ ] if calculation produces same result as is present, don't modify doc.

### lib-menu

- [ ] remove that blank line at the end of menus
- [X] Enable lib-menu to show short-cut keys
- [X] menu-bar to which we can add menus from which commands are sent
- [ ] track movement so entry under cursor can be highlighted
- [ ] support positioning above the target is no space below.
- [ ] support single-click to open modal menu.??
- [ ] improve hack for communicating action position.  Maybe require
      an 'is-button' attribute and report button height (and width?)
      in num and num2.

### lib-url

### config

- [ ] file patterns for "complete" to (optionally) ignore
- [ ] read .gitignore and add to "ignore" list
- [ ] top/bottom margin  - "render-vmargin" set on view or doc??
      vmargin should be relative or absolute
- [ ] c-mode to use tabs or spaces for indent
- [ ] c-mode: use only spaces for bracket-alignment, or tabs as well
- [ ] shell command regex to choose parsing: diff, git-log, grep/make
- [ ] shift width for auto and manual left/right display shift
- [ ] tab width ??

- [ ] colours of whitespace: tab, at-eol, tab-after-space, past-col ....
- [ ] whitespace max-col (per-line??)
- [ ] white-space blanklines at sof, eof, multiple-together
- [ ] colours for notmuch summary lines
- [ ] colours for urls
- [ ] colours: dir: flag/metadata/name/link
- [ ] colours: dir: header
- [ ] colours: email spacer
- [ ] colours: search: this, other, replaced
- [ ] colours: markup control
- [ ] colours: message-line
- [ ] colours: renderline wrap/truncate markers - char also
- [ ] colours: emacs: file-name open: ignored, Ok-dir, missing-dir,
      non-dir
- [ ] colours: emacs: selection: active, transient, replacable...
- [ ] colours: abbrev - prefix and completed
- [ ] colours: spell - errors
- [ ] colours: compose: headers, unknown, bad, header-marker,
      section-marker
- [ ] colours: compose: interpolated text, interpolation character.
- [ ] compose: known headers
- [ ] colours: diff: added, removed, '@@', diff-same
- [ ] colours: make: current-match, subordinate matches
- [ ] colours: mergeview: markers: conflict, space-only, good
- [ ] colours: mergeview: Unmatches, Extraneous, Changed, Conflict,
      Conflict-spaces, Already
- [ ] colours: notmuch - LOTS
- [ ] colours: c-mode: paren-same, paren-different
- [ ] colours: hex ....
- [ ] colours: complete: matching section

- [ ] config alternate key-strokes???


New Modules
-----------

- [ ] emacs-pinentry.  pinentry-curses can talk to emacs using a
      protocol that edlib can copy.
      export INSIDE_EMACS=yes
      Listen on a SOCK_STREAM at ${TMPDIR-/tmp}/emacs$(id -u)/pinentry.
      On connect, end "OK".  In general responses are lines terminated
      by '\n' with OK or ERR ending a response
      D <data up to newline>  with %XX esacpe for % \n \r
      OK
      ERR <errcode>
      #comment

      Respond to GETPIN with D data
      Respond to CONFIRM with  OK if user sends OK.
      Accept some settings:
           SETTITLE Context for PIN
           SETDESC  Enter PIN for Fred Bloggs <freddy@bloggs.net>
           SETERROR Error message
           SETPROMPT PIN:
           SETREPEAT  IF PIN needs to be entered twice for confirmation
           SETREPEATERROR  message to show if they don't match
           SETOK   Text for label of OK button
           SETCANCEL Text for label of "NO" or "Cancel" button
           SETNOTOK   NOT OK button???

- [ ] separate out CUA-base from mode-emacs:

     - selection management
     - mouse management
     - arrows - shift for selection, control for word
     - BS Del
     - Next Prior Home End
    - home/end - start/end of line
    - Shift-home/end - start/end of file
    - C-up/down start/end para
    - C-bs C-del delete word

- [ ] vi mode

    - two mode: command and entry
    - h,j,k,l w,e,W
    - d* c* s  a A i I ^ $ / ? . fX
    - numeric prefix
    - much more

- [ ] "CUA" mode

    - "extra" on CUA-base which might conflict with emacs/vi, including
      control-letter and function keys.

    - C-c for copy, C-x for cut, C-v for paste,
    - C-a select all
    - C-f find/replace C-S-f - search again
    - C-z undo C-y redo
    - A-x for menu x
    -  attach an extensible menu to the selection
        copy, paste-in, QR, git-view

- [ ] info browser

      Content is (mostly) ready-formatted.  Need to parse and handle navigation.

- [ ] man page viewer

      - MANWIDTH=72 MAN_KEEP_FORMATTING=yes man page and process X\bX
        and _\bX for bold and underline.


- [ ] more charset support? Next in my popularity list from my email database
     are: is0-8859-15  gb2312 iso-8859-2 iso-2022-jp gbk ansi_x3
     kc_c_5601-1987 is a korean with 2-byte encoding when firt is >=0x80.
     I don't think it can be parsed backwards..
     windows-1250 is needed - or at least a reliable fall-back
        AM6PR04MB6328CFDD9A91D3F0125D1A1491809@AM6PR04MB6328.eurprd04.prod.outlook.com
- [ ] use iconv(3) for char-set conversion

- [ ] generic syntax highlighting:
      strings, comments, keywords, type/function/const/var distinction
      urls, email address
      Possibly oversee diff/wiggle highlighting
      markdown bold/ital/code bullet, counter. heading.

      Call-out or regex to find para.
      Another to find words
      Maybe another to classify words

      Classify some content as "section" for outlining

- [ ] gdb integration.

       - break at line in code
       - show code from reported line
       - continue to code location

Possibly some of these will end up being features in other modules.

- [ ] C/python code "index" pane to quickly jump to function, and see context
      This part of the IDE project below.

- [ ] create view-pane that either puts a cursor on whole line, or moves
      the cursor to the "right" place.  Maybe a markup to say "here is the
      preferred column" ??  Maybe use for make output so I can see current
      match more easily.

- [ ] tags handling - and easy tag-search without tags. e.g. git-search.
      alt-S looks for TAGS or .git and either does a tags-search or a grep-l and
      check every match.  So maybe this is part of the 'make' module
- [X] menus
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
- [ ] Create ssh-access module that uses ssh/scp to access files - maybe use python paramiko

New Modules - more complex
-------------------------

### remote editing ideas
A good model for remote editing is to have a proxy at some point in the stack,
so that edlib runs on both ends, but at some point a pane is a proxy for a remote
pane (though maybe not 'pane' exactly) which connects over the network.
But what point?

- raw-display: this might have just ncurses on the client and everything else on
  the server.  client could send keystroke, mouse events, resize/refresh request,
  selection-request, selection-content, and maybe file content
  server could send "panel" create/resize/reposition, panel clear,
  draw text with attributes, selection-request, selection-content, file-content
  run-command request (e.g. to run a local viewer for attachments)
- generic-display:  This could be a full pane, but would need to proxy
  Draw:text-size measurement requests which might be slow
- doc-view:  This would be a proxying core-doc view (doc_handle) where all
  the viewing panes are local, and all the doc-side panes are remote.
  This seems most elegant, but managing updates to marks and handling all
  callback might be awkward
- specific docs:  Some documents would explicitly support proxying.
  e.g. text, dir, docs.  Any doc filtering (e.g. charset, crop etc) would
  happen locally.  The proxy would use a lease to cache content locally
  and would treat a lease timeout like on-disk file change.
- filesystem: this is little more than an sshfs mount.  Probably too low level.

One outcome of these musings is that edlib programs should run external
commands and access file through a specific pane - either a doc pane or
a display pane.  This might result in them running on different hosts.

The protocol over then should be QUIC if possible as they seems to allow
mobility nicely.  I'd need to look at how it handles network breaks.

### Remote display

I don't think I'll go with QUIC.  I'll make something focussed, like
mosh and wireguard do.
ssh will be used to request a key and 2 nounces - requested over the
server socket.  Using this key, messages are secured with the "secretbox"
module of libsodium and sent via UDP - client can send from anywhere and
server replies to there.

Client sends keystrokes, mouse-action, size report.  Each have an event
sequence number.

Server sends pane reset/create/destroy/clear/update/done messages.
These have sequence numbers with a history stored and updates are resent
if a client message has an old seq number.  If the client seq is too
old, the server can send a reset, then create and fill each pane.

The client acknowleges server messages with a 'size' update, and also
reports the size every 5 seconds as a ping.  Server replies with any
un-acked updates, or with a new 'done' message.

Some day I might need "measure" requests from the server which the
client can reply to, so texts can be measured for variable-sized fonts.
Server would need to cache results for performance, and would need to
know if a font is constant-width, so a single measurement will suffice.

I also need basic file transfer - particularly from server to client
so that an external viewer can be run.  May as well make it bi-di so
that an external editor can be requested.

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

### a “reflection” document so I can view the internal data structures.

When developing a new pane things go wrong, but it is hard to see what.
I want a pane which shows me the structure of all panes, with parent/focus
links, with notification chains, and with other ad-hoc connections.

I'd also like to be able to follow a command as it moves through the panes.
This probably needs to be recorded, then played back for me.

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

- easy render to PDF - e.g. use PyFPDF
- ASCII-Math or similar
- anchors for links, structure tags (author, title), foot notes,
  figure captures, tables, index, content.
- figure drawing.
- spreadsheet cells for auto calculations.
- outlining support of course.

Syntax - markdown plus...

Lines:
- basic paragraph: not indented more than 3 spaces
- heading: starts # or single line followed by 3 or more - or = on next
  line
- bullet list: lines starts '-' in first 4.  Following lines that are
  indented are included.  Maybe '*' or '+' also do same?? A waste
- number list: digits period.
- block-quote: lines start '>'  These nest naturally
- code: lines indented with no prefix
- fenced code: line starts ``` then until a close.  Ouch - hard to parse
  backwards
- horzontal rule: --- or ___ or *** and nothing else following blank line

- table.  Line start '|' continue to blank line.??
    A row starts with '|' at start of line and ends with '|' at end of line.
- formatting info starts ':'
- image: ![caption](file)
- reference [label]: link "text" If link in HERE then internal
  reference.  link can have #label to find label in that file.

Special:
- is a line ends 2 or more spaces, hard line break.  If 0 or 1, then continue

Intra line:
 **word** is bold
 _word_ is italic
 `word` is monospaced (code)
 [word] is a link. [word](link) is too
    or [word](link "title text")

 <url>
 <user@domian.thing>

 \X  is X literal.  \ * _ `

 $=number[calculation](format)

Formatting controls..

Non-module functionality
------------------------

### Documentation

 Both user-documentation and developer documentation, extracted from
 literate programming comments, and viewable using markdown mode.  This
 would include links to other files with more content.  Maybe
 documentation from a given file could be parsed out and displayed
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

