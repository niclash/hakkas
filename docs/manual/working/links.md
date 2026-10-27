# 11. Links between processes

::: info Partly available
The controller runs links between processes, but the editor cannot show or make them yet. The
**process map**, a view of the processes and their links with a button beside *New…*, is planned
(plan task 79). This chapter will describe it once it exists.
:::

## 11.1 Links

A **link** connects one process's **output port** to another process's **input port** (Section
2.4). It carries a value from one process to the other, just as a connection does inside a
process: on change, through the same queue, with no delay of its own. A typical use is one
process that computes the outdoor temperature's mean or the season, and several that use it.

The rules are those of connections:

- **An input port has one source:** a link, a binding to a sensor, or (through its block) a
  connection or fixed value.
- **An output port may feed any number of links**, and drive actuators at the same time.
- **Removing a link** returns the input to its resting value, at once.
- **A stopped process's outputs keep their last values**, and so do the links from it.

**Today**, links are kept by the controller in a file of its own, `links.json` in its data
directory, which an administrator writes. Each entry is `{"from": "<process
id>.<outport>", "to": "<process id>.<inport>"}`.

The controller makes the links when it starts:
- after every process is loaded, and before any process starts, so that a linked input sees its
  source's first value;
- skipping, with a warning in its log, a link it cannot make (an unknown process or port, or an
  input that already has a source).

Links are not part of process files or backups (Section 9.4). Renaming a process port ends the
links to it.

Until the process map exists, prefer **one process per job**, and connections inside it, where
two parts must share values. A composite type (Chapter 7) keeps a large process readable.

## 11.2 The process map

Planned (plan task 79): a view, beside the process editor, in which each process is a block with
its process ports as sockets, and each link a connection. You will be able to draw and remove
links there as you draw connections, and open a process with a double-click.
