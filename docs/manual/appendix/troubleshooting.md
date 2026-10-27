# C. Troubleshooting

Most problems show in one of three places:
- the editor's error bar, and the *Messages* panel;
- the controller's log (Section 16.5): run hakkas with `--info` to see more;
- the live values: watch the connections around the problem (Section 5.11).

## C.1 Connecting and logging in

| Symptom | Cause and remedy |
|---|---|
| The page does not load | Hakkas is not running, or a firewall blocks the port (8080 by default). |
| *Cannot reach the server* on the login screen | The editor cannot reach the controller's connection port, the page's port + 1 (8081). Open it in the firewall. |
| *Login failed* | A wrong user name or password, or no users at all (Section 16.3). After repeated failures the controller slows down further attempts from your address for up to a minute. |
| *The server could not prove its identity* | The controller does not know your user's keys: you reached another controller than you think, or the users file was replaced. Check the address. |
| Logged out after a while | The session ended: 30 minutes without activity, or 12 hours in all (Section 10.2). |
| *The connection was lost and the session has ended* | The controller was restarted while you were disconnected. Log in again. |
| The editor is read-only with a banner | The connection was lost; it reconnects by itself (Section 10.2). |

## C.2 Editing

| Symptom | Cause and remedy |
|---|---|
| No block types, no *New…* | Your role is viewer or operator (Section 10.1). |
| *Not permitted: …* | Your role may not do that. |
| *Input already connected: block.port* | An input has one source. Remove the connection, value or binding first, or draw the new connection over it (it replaces a connection or value). |
| *Node exists: name* | Another block in the process has that name. |
| A block without ports, and a warning in the log | A placeholder: the controller lacks its type, typically a composite type that was not installed. Upload or install the type and restart (Section 6.4). |
| *Component in use* | A composite type cannot be changed or deleted while a block of it exists (Section 7.2). |
| *Not undone: changed since by someone else* | Someone else edited what your undo would change; that step is skipped (Section 5.10). |
| *Enter a number* | A number input takes numbers only, without units. |

## C.3 A process does not do what it should

Work from the output backwards, watching values:

1. **Is it running?** A stopped process shows a red tint in the list, and its outputs keep their
   last values.
2. **Is the output bound?** Its marker shows `⇄ channel`. Check the channel in the Environment
   panel or the device window. A DIO1's terminal is an output only while its `dir` channel is
   true.
3. **Does the input have its value?** A bound input's live value is in the inspector. A sensor
   that has never sent a value leaves the input at 0.
4. **Watch the connections** (*Watch all connections*) and follow the values to the block where
   they stop making sense.
5. **An input without a source** rests at 0 or false. A regulator rests in manual at 0, a schedule
   rests off, and a `process/Interval` rests idle. Missing settings are the most common cause of
   a block that does nothing.

Common causes:

| Symptom | Cause |
|---|---|
| A schedule is never true | Its setting is invalid (`9-17` on a `MinuteRange`, which wants `09:00-17:00`), or the time zone is UTC (Section 16.2). |
| A schedule is an hour off | The time zone has no daylight-saving rule. |
| A regulator does nothing | It is in manual (`auto` false), `sample_ms` is 0, or both limits are equal (no limits, not a range of zero). |
| A regulator starts far off and recovers slowly | It went to automatic before its measurement arrived: add a start delay (Section 14.7). |
| A small change does not get through | It is below the change threshold, 0.05 by default (Section 2.2). |
| A value flickers between two states | A feedback loop that does not settle, or a comparison without hysteresis: use `process/Hysteresis` (Section 14.8). |
| A delay never passes | Its input changes more often than its time. |
| `math/Max` of negative numbers gives 0 | Only fed inputs count. Check that the inputs are really connected. |

## C.4 Saving and restarting

| Symptom | Cause and remedy |
|---|---|
| Changes lost after a restart | They were not saved (Section 6.4). |
| A process does not start after a restart | It was saved while stopped. Start it and save. |
| *Unable to save graph* | The data directory is full or not writable. |
| A binding missing after a restart | It did not fit the environment and was skipped: see the log's warning. |
| Hakkas does not start | The log's last lines say why: a configuration value (named), a missing data directory, a port in use. |
| Hakkas waits at start | *waiting for environment at …*: the environment program (the emulator) is not running yet. Start it. |
