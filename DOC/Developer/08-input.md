
# The input pane

As discussed in [Details of displays][file:07-displays.md] input from
the user is converted to messages which are sent to the parent of the
display module.  This parent is usually the input pane ("attach-input").

The core functionality of the input pane is to take these messages and
send them in a modified form to the relevant leaf pane.  For Keystroke
messages, the leaf pane is found by following "focus" links from the
input pane.  Each pane with any children must designate one child as the
"focus", so by following these from the "input" pane, the current
overall focus can be found.  The "input" pane caches the current focus
rather than searching for it each time, and captures "pane:refocus"
messages to invalidate that cache.

For "Mouse-event" messages which indicate a button press the target pane
is found by iteratively choosing a child pane which contains the x,y
co-ordinates of the event, then continuing from there.  For other
"Mouse-event" messages, the target pane found for the previous
button-press event is used, unless it has already be been closed in
which event a search for the given co-ordinate is used.

The "input" pane maintains a small amount of state which affect the
message that actually gets sent to the chosen pane.  There is a textual
mode, and two numbers: num1 and num2.  These can be set individually by
messages from child panes "Mode:set-mode", "Mode:set-num", and
"Mode:set-num2", or set all together with "Mode:set-all".  These three
are copied into each message, and then cleared (to empty string,
NO_NUMERIC, and 0) before the message is sent.  The handle that responds
to the message may restore the values or set new ones as appropriate.

The key of the message which is sent once a receiving pane is determined
is create by prefixing the content of the Keystroke of Mouse-event
message with either a "K" or an "M" respectively followed by the mode.
The num1 and num2 values of the message are those most recently set by
"Mode:set-num" etc.