
# Fundamentals: panes, commands, keymaps, and attributes

## Panes

A central object type within an edlib editor is the pane.  Each pane
provides storage and functionality.  Panes are linked together into
stacks and other arrangements to comprise everything within the editors.
There is a pane representing each display, a pane for each document, and
panes for each view of each document.  With one or two minor exceptions,
everything in the editor is comprised of panes.

Panes are arranged in a hierarchy with every pane having a parent and
possibly having siblings and children.  The root of this tree is a
distinguished pane representing the editor as a whole.  It is its own
parent.  The children of this root are all the panes that don't have any
special need for some other pane to be their parent.  This normally includes a
pane that is used to collect all documents together, and a pane for each
display window.  This list is no exhaustive.

There are two particular effects of having panes in a parent/child
relationship beyond the obvious of serving as a grouping mechanism.
The first is that when a parent pane is closed (i.e.  destroyed), all of
its children are automatically destroyed, and naturally this continues
recursively.  So if a display window is closed, all the panes attached
beneath it will automatically be closed.  If the editor is shut down by
closing the root window, you can be sure that everything will be cleaned
up.

The second particular effect is that messages, discussed later, often
flow up the tree from child to parent until they have been fully
handled.  They get injected at a leaf and any pane which recognises the
message can claim it.  Thus it is normal to provide a mixture of
services by stacking the relevant pane together, often without being
concerned about the order.  More general-purpose panes are likely to be
closer to the root and so able to capture messages for a wide range of
children.  The root is the final pane in the chain and likely to support
functionality that is globally relevant.  If the root cannot handle a
message, it doesn't get handled.

When a pane is created it will be allocated some memory for storing any
state that might be relevant, and will be given a keymap which is
explained later.  This keymap identifies which messages can be handled,
and what code should be used for each different message.

A pane also width and height, relative x and y locations and depth.
Often the first two are inherited from the parent, and the remainder are
zero.  In some cases, such as for a document, these numbers have no
meaning.  However many panes result in an image being displays to allow
a document to be edited.  For these panes the width and height indicate
how much room there is for the display, the x,y locations position the
image within the parent, and the depth is use to resolve situations
where two panes overlap.

FIXME mention "Close" and "Free"

### relevant interfaces

C:
  pane_register()
Python:
  edlib.Pane

## Commands and messages

All behaviour within the editor is the result of commands which respond
to to messages.  Message have a well defined format with a limited set
of fields.  Primarily there is a name, or key, which is often used to
determine which command handles the message.

There are several ways to send a message, each of which finds the command
in a different way.  The simplest is that a message can handed to a
specific command which acts on it immediately.  This most commonly
happens when a message contains some command as a call-back.  Whichever
command eventually handles that message may find that it need to send
information back to the caller and it does this by passing a message to
the callback command.

The next way to find a command is to send to some pane.  Each pane has a
key map while associates key names with commands.  If the pane doesn't
have a matching command, or if that command declines to handle the
message, then the message is passed on to the parent.  Alternately a
message can be passed to just the pane so that if it cannot handle the
message directly, no other panes are attempted.

Finally, a pane may request notifications from some other pane.  Then,
when that pane sends a message as a notification the registering pane
will be asked to act on it.  The registration request includes the two
pane and a key name.  When a pane sends a notification with the key name
it will be passed to each pane which registered for that notification,
roughly in the reverse order in which the panes registered.  When a pane
receives a notification its keymap will be consulted to find out which
command should handle the message.

The message that is received contains the key, a pair of panes (home and
focus), a pair of numbers, a pair of strings, a pair of marks, a pair of
commands, and a pair of co-ordinates (effectively another pair of
numbers).  The key, numbers, strings, marks, and co-ordinates are passed
from caller to handler unchanged, as are the focus pane and the second
command.

The home pane is set when a message is created to pane that is expected
to handle it.  When that pane does not handle the message and the
message is allowed to proceed to the parent, the home pane in the
message is updated to that parent.  So when a message is actually
reviewed, the home pane will be the pane the is handling the message.

Similarly the first command in a message when it is received is the
command that is actually handling the message.  This can only be set
when sending a message if it is sent to a specific command.

A command can return a simple integer value.  A positive value suggests
success and is returned to the call.  Other values have other meanings:

- Efallthrough means at the messages wasn't completely handled and it
  should be passed on to a parent.  This is currently zero, but that
  might changed.
- Enoarg indicates that some expected argument (part of the message)
  isn't present, so the message cannot be handled.  This suggests a
  programming error.
- Einval means that something else is wrong with the message and it
  cannot be handled because it doesn't make sense.
- Enosup means that the message does make sense, but for some other
  reason it cannot be handled at present.  There is currently no code
  that sends or expects this value, so it may well be removed.
- Efail means that the message make perfect sense but that an attempt to
  handle it failed.
- Efalse is similar to Efail but is gentler.  It is used when failure is
  expected and is a valid and useful result.

If the command needs to return more than a simple number it must be
given a callback command (called "comm2") which it can call with the
relevant information.  A message in the standard format can be passed to
this command and it can store parts of that message as relevant or
manipulate them in any other way.  The focus pane given to the callback
will be the original caller so that can be given the information.
Alternately the command can be attached to arbitrary data in some way
specific to the API language being used.  The command is given a
reference to itself in the message and that data can be found from the
command and accessed or modified as needed.

### relevant interfaces
- C
    + key_handle
    + pane_add_notify
    + pane_notify

## Keymaps

As mentioned each pane has a keymap which maps key names to commands. As
well mapping a simple name to a command it can map a range of names.
Typically the endpoints of the range only differ in the last character,
so the range might be "foo-A".."foo-Z" which allows any key that starts
"foo-" and is followed by an upper case letter.  A common range type
acts as a simple prefix, so the range "bar-\0".."bar-\0xff" will match
any key that starts "bar-".

The keymap support code uses a bloom filter to more efficiently
determine if a keymap doesn't support a given key.  The hash only
considers characters up to and including the first hyphen or colon.
Consequently the endpoints of any range must be the same up to the first
hyphen or colon.

## Attributes

Panes and various other objects managed by edlib can have attributes
attached to them.  An attribute is a name and a value, both strings no
more than 512 bytes long.  These can be examined or updated quite easily
and can be used however is needed.  For panes, when an attribute cannot
be found on the target pane, it will be sought on the parent.

As well a having attributes explicitly stored, a pane add the key
"attr-get" to its keymap, in which case the matching command will be run -
given the attribute name and a callback - to get the attribute value.

Some commonly used panes define protocols involving attributes on
themselves or related pane.

As well as panes, attributes can be stored on marks and characters in a
document, both of which are described later.  The command "doc:attr-get"
will be called if available to look up a given attribute at a given
location in a document.
