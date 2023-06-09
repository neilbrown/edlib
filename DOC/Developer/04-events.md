# Events

Most messages are send as a result of code running in the handler for
some other message.  Obviously there must be some starting point for
messages.  These come from the event subsystem.

The event subsystem registers some handlers with the root pane for
messages that themselves register other handlers for various events.
When these events occurs the relevant handlers are called, and so begins
the chain of messages that results in anything happening in the editor.

The event subsystems isn't quite the ultimate starting point.  The
editor must have a main loop with sends the message "event:run" to the
root pane, and then runs "pane_refresh()" on that pane.  The "event:run"
handler, registered by the event subsystem, determines what events have
happening, if any, and calls the relevant handlers.

The messages that the event subsystem listens for are:

- event:read - The first integer argument is a file descriptor.  If it
  ever becomes possible to read from that file descriptor, the command
  passed as "comm2" will be call.  The message passed to "comm2" will
  have both "focus" and "home" set to the focus that as in the
  "event:read" message.  If the command returns a negative value, such
  as Efalse, the event will be disabled, otherwise it will continue to
  respond whenever the file descriptor is readable.

- event:write - the first integer argument is again a file descriptor
  and comm2 will be called whenever a write can succeed.  Efalse should
  be returned to disable the event

- event:signal - the first integer is a signal number.  If that signal
  is ever received, the comm2 command will be called.

- event:timer - the first integer is a number of millisecond.  The comm2
  command will be called that many milliseconds after the event is
  requested. If a negative value like Efalse is returned, the timer will
  not be repeated.  If a non-negative value is returned, the event will
  be repeated after another period of the given number milliseconds.

- event:poll - The comm2 command will be called every time that
  "event:run" is called by the main loop.  This can be useful if some
  source of events doesn't always trigger event:read.  An example might
  be a stream that supports "unget".  In such a case there may be
  content to get, but not from the file descriptor.
  comm2 should return a positive number if it did anything, and 0 if
  if there was nothing to do this time.

- event:on-idle - The comm2 command will be called soon after the
  current event completes.  The command will only be called once and
  must be explicitly rescheduled if it is wanted again.
  'num' is a priority level.

  0/ is for background tasks.  Only one of these is run before checking
     for regular events.
  1/ is for pane_refresh(), and maybe similar tasks.  It is probably
     needed every time around the loop, and does non-trivial work
  2/ is for simple high priority tasks like freeing memory that was in
     using during the previous event.

- event:free - Any register event from the focus pane which is supposed
  to call the given comm2 will be deactivated.  If no comm2 command is
  given, all registered event for the given focus pane are deregistered.
  Note that when a pane is closed, the event subsystem detects this and
  automatically frees and events registered by it.

- event:noblock - tell the next call to event:run to not block
  indefinitely.  It should activate any events that are ready to be
  activated, and then return.  The internal "noblock" flag will be
  cleared before any event are run, so if none of them call
  "event:noblock" again, the next call to "event:run" may block if no
  events are ready.  This is used by the main loop to if there are any
  commands registered to be run when the loop is idle.  There might be a
  better way to achieve this.

- event:run - Run any pending events.  If there is nothing to be done
  and event:noblock hasn't been received, then wait until there is at
  least one event to handle.

- event:deactivate - deactivate the event subsystem.

- event:refresh - edlib allows multiple different event handles to be
  registered with one taking priority.  Every "event:action" message is
  handled by the editor core and mapped to the lexically first message
  registered that starts with "event:action-".

  When a new event handler loads it should call "event:refresh".  This
  tells the current handler to iterate its list of events and
  re-register them.  The new handler will see the registrations and so
  effect a smooth take over.
