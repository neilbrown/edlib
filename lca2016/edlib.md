:H1:center,20,fg:blue,space-above:15,family:sans
:H2:center,15,fg:darkblue
:background:image-stretch:back.jpg,page-local
:scale:x:533,y:300
:P:left:30,12,family:sans
:bullet:fg:red,13
:L1:,left:30,12,bullet:•,tab:30,family:sans
:L1c:,left:30,12,tab:30,family:sans
:L2:,left:50,10,bullet:*,tab:30
:L2c:,left:50,10,tab:30

# Edlib
## because one more editor is never enough









:P:center,14
Neil Brown


LCA 2016 - Geelong

# Edlib
## because one more editor is never enough

!400:200:standards.png
:P:center,16,fg:purple
xkcd.com/927

:background:overlay:B-nun.JPG
# I Love EMACS

- have been using it for nearly 30 years
- *vim* is quite good too!
- good for code
- good for email
- lots of packages and add-ons.
- not quite perfect
- wikipedia lists over 100 others


:background:overlay:B-bather2.JPG
# Consider the MVC pattern
## Model-View-Controller

!350:190:mvc1.png

:background:overlay:B-blue.JPG
# Consider the MVC pattern
## Model-View-Controller

!380:160:mvc2.png

Model is text buffer
- characters with attributes.
- indefinite undo.

:background:overlay:B-boy.JPG
# **Buffer List** buffer

!400:160:bufferlist.png

- That **GNU Emacs** buffer doesn't exist any more
- Model requires indirection for non-text sources
- "style" formatting must be stored in the buffer

:background:overlay:B-fisher.JPG
# `hexl-mode`

!400:160:hexl.png
- binary file must be converted to text (external program)
- not effective for `/dev/sda`

:background:overlay:B-flag.JPG
# Back to Model-View-Controller

!480:140:mvc3.png

View is a window that interprets attributes
- can hide, highlight, insert chars etc.
- text can be "intangible" and "read-only"
- attribute with hooks to call on change
- no hooks to call on display

:background:overlay:B-glamor.JPG
# An attempt at a spreadsheet

!400:200:xlnt-tex.png

Uses a LaTeX table with expressions in comments

:background:overlay:B-sailor.JPG
# An attempt at a spreadsheet

!450:150:xlnt-ss.png

- code: hides and highlights and calculates.
- felt like programming in assembly-language.
- rendering isn't programmable.

:background:overlay:B-bather.JPG
# Model-View-Controller again

!480:180:mvc4.png

- Controller is elisp.  Awesome.... or not.
- steep learning curve for single use-case

:background:overlay:B-camera.JPG
# `edlib` - scratching an itch

Everything is plugable

- multiple "document" backends
- multiple "language" bindings
- multiple multi-stage "renderers"
- multiple "display" managers
- configurable key/event bindings (of course)
- core provides essential abstractions
- loadable libraries for everything else

:background:overlay:B-nurse.JPG
# edlib core - panes

!400:170:panes.png

- a pane represents an area of interaction
- may have children with depth
- sends and receives all messages

:background:overlay:B-girl.JPG
# edlib core - documents and marks

- a document is a set of interfaces provided by a pane
- document can be accessed as bytes, characters, lines, ...
- multiple panes can access the one document
- a mark is a location in a document - with state
- marks can be grouped
- easy to find "next" or "previous" in given group.

:background:overlay:B-minister.JPG
# edlib core - commands

- commands pass control between panes
- can pass control between languages
- args are:
  - two panes - source and destination
  - two integers
  - two strings
  - two marks
  - two co-ordinates (x,y or w,h)
  - another command
- return an integer ... or call-back the command

:background:overlay:B-police.JPG
# edlib core - attributes

- attributes are named strings
- elements (characters) in a document have attributes
- panes, marks, and documents have attributes
- can be used to pass extra information between 
 commands

:background:overlay:B-clown.JPG
# edlib plugins

- documents: text buffer and directory
- displays: ncurses and pygtk
- render: lines, hex, format, complete, presentation
- key bindings: emacs-like, per-pane
- tile manager
- status line/scroll bar
- popups: search and find-file

:background:overlay:B-scots.JPG
# future work

Currently have a working prototype. Want more...
- re-evaluate all interfaces
- unit tests!
- lots more edit functionality
- understand copy/paste for non-text documents
- Bindings for Lua, and Rust? and ...
- vi-like bindings
- hexmode overlays for known structures
 e.g. filesystem superblocks
- Email client (based on notmuch)
- spreadsheet?
- wiggle plugin ... or plug in to wiggle

:background:overlay:B-naked.JPG
# Edlib - a naked bollard

:P:center,samily:sans,15

Ready for decorating



github.com/neilbrown/edlib




:P:center,family:sans,20
Questions?

