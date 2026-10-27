# B. Glossary

**Actuator** — a channel that hakkas writes: a relay, an analog output. Driven by one process
output port through a binding. Chapter 8.

**Binding** — the connection of a process port to a channel of the environment. Section 2.5,
Chapter 8.

**Block** — one function in a process, an instance of a block type, with a name of its own.
Section 2.1.

**Block type** — what a block is: its inputs, outputs and what it computes, named
`family/Name`. Built in (Chapter 15) or composite (Chapter 7).

**Bumpless** — of a regulator switched from manual to automatic: its output continues from the
manual value without a jump.

**Change threshold** — how much a number must change before it travels on, 0.05 by default.
Section 2.2.

**Channel** — one input or output of a module, such as `slot1-ai[2]`, the second analog input
of slot 1. Section 8.1.

**Composite block type** (also *group*) — a block type of your own, made of other blocks.
Chapter 7.

**Connection** — carries an output's value to an input. An input has at most one. Section 2.2.

**Controller** — the device or computer that runs hakkas and its processes.

**Device window** — a picture of the controller with every channel's live value. Section 8.2.

**Emulator** — the Colibri emulator, which behaves like a Colibri controller's I/O on your
desktop. Appendix D.

**Environment** — the controller's I/O: its modules and their channels. Section 2.5.

**Fixed value** (also *initial*) — a constant on an input, drawn as a value node. Section 2.3.

**Graph** — the same as a process, in file names and the protocol.

**Inspector** — the panel on the right that shows and edits the selected block. Section 5.6.

**Link** — the connection of one process's output port to another process's input port.
Chapter 11.

**Mapping dialog** — asks where each binding goes when a bindings file does not fit the
controller. Section 8.5.

**Placeholder** — a block whose type the controller does not have. It keeps its place and
connections, and does nothing. Section 6.4.

**Process** — one diagram of blocks that runs as a unit, started and stopped on its own.
Section 2.4.

**Process port** — a block's input or output made part of its process's interface: an inport or
an outport. Bound to channels or linked to other processes. Section 5.12.

**Resting value** — what an input fed by nothing holds: 0, false or an empty text. The inspector
calls it *Default*. Section 2.3.

**Role** — what a user may do: viewer, operator, editor or admin. Section 10.1.

**Selector** — the setting of a calendar block: a value, a range or a list, such as `1-5` or
`6,7`. Section 14.4.

**Sensor** — a channel that hakkas reads: a temperature, a digital input. Feeds one process input
port through a binding.

**Session** — a login, kept over reloads until it ends. Section 10.2.

**Unsaved** — a process with changes not yet saved; marked ● in the process list. Section 6.4.

**Value node** — the small box beside an input that shows its fixed value. Section 5.5.

**Watch** — to show a connection's or an input's live value. Section 5.11.
