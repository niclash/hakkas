# 16. Administration

This chapter is for whoever installs and looks after a controller. It covers hakkas on a Linux
computer: a Raspberry Pi in a cabinet, or your desktop for development. The Colibri device's
firmware is to come; it will keep the same configuration file and data directory on its own
flash.

## 16.1 Installing

Hakkas is one program, `hakkas`, and its editor, a set of web files. Build both from the
source (the repository's `README.md` has the details):

```sh
cmake --preset release && cmake --build --preset release
cd frontend && npm install && npm run build && cd ..
```

A **package build** installs them in the usual places:
- configure with `-DHAKKAS_PACKAGE=ON`;
- `cmake --install` puts `hakkas` in `<prefix>/bin` and the editor in `<prefix>/share/hakkas/web`,
  where the program looks for it.

Then:

1. **Make a configuration directory** (default `/etc/hakkas`) and a **data directory** for the
   processes.
2. **Create the users** (16.3). Without users every login is refused.
3. **Start it**:

   ```sh
   hakkas --confdir /etc/hakkas --basedir /var/lib/hakkas run
   ```

   The editor is then at `http://<computer>:8080/`. Its connection to the controller uses the next
   port, 8081, so a firewall must let both through.
4. **Have it started at boot**, for example by a systemd service that runs the command above.
   Ctrl+C or `SIGTERM` stops hakkas cleanly. A failed start exits with 1 and logs why.

For a quick try on your desktop, `./build-debug.sh && ./dist-debug/run-debug.sh` in the
repository builds everything and starts it with a user it asks you for.

## 16.2 The configuration file

`hakkas.json` in the configuration directory. Every key is optional, and a missing file means all
defaults. Sections are nested objects: `{"log": {"level": "info"}}`.

| Key | Default | What |
|---|---|---|
| `label` | `"Hakkas"` | the controller's name, shown by *About* and in backup file names |
| `engine_id` | a new one at each start | the controller's identity; set it to keep one |
| `host` | `"0.0.0.0"` | the address to listen on (a numeric address) |
| `port` | `8080` | the editor's port; the editor's connection uses `port + 1` |
| `max_connections` | `4` | how many browser connections at once |
| `change_threshold` | `0.05` | how much a number must change to travel on (Section 2.2) |
| `time_zone` | `"UTC0"` | local time for the calendar blocks, as a POSIX TZ string, e.g. Sweden: `"CET-1CEST,M3.5.0,M10.5.0/3"` |
| `environment.socket` | none | the Unix socket of the environment program, such as the Colibri emulator (Appendix D) |
| `environment.pictures` | none | a directory of the device pictures' copies |
| `log.level` | `"warn"` | `fine`, `info`, `warn` or `error` (16.5) |
| `log.mirror` | `false` | also send the log to the editors' *Messages* panel |
| `auth.users_file` | `<confdir>/users.json` | the users (16.3) |
| `auth.idle_timeout_s` | `1800` | a session ends after this long without activity |
| `auth.max_lifetime_s` | `43200` | and after this long in all |
| `webdir` | where the build put the editor | the editor's files |
| `allowed_origins` | `[]` | other web addresses allowed to connect, for developing the editor |

**Rules:**
- **A value of the wrong type, or out of range**, makes hakkas refuse to start, naming the key.
  A controller never runs with a setting it misread.
- **An unknown key** is a warning, and ignored.
- **`auth.server_secret`** is written into the file by hakkas at its first start. The directory
  must be writable then. Keep the file private.

**The time zone** decides when the schedules switch, including daylight saving time. The string
has the form `STDoffsetDST,start,end`. For example `CET-1CEST,M3.5.0,M10.5.0/3` means:
- CET, one hour east of UTC;
- CEST in summer, from the last Sunday of March;
- back on the last Sunday of October at 03:00.

The `TZ` documentation of any Linux system lists the forms.

## 16.3 Users from the command line

Users are created on the controller itself, never over the network:

```sh
hakkas --confdir /etc/hakkas user add alice --role editor     # asks for the password twice
hakkas --confdir /etc/hakkas user passwd alice                # a new password
hakkas --confdir /etc/hakkas user del alice
hakkas --confdir /etc/hakkas user list                        # name and role per line
```

The roles are `viewer`, `operator`, `editor` and `admin` (Section 10.1).

The file keeps no password, only what a login needs to check one. A change takes effect at the
next login, without a restart. Sessions already open go on until they end.

## 16.4 The data directory

`--basedir` (or `datadir` in the configuration) holds what the controller saves:

```
graphs/      one file per process
types/       one file per composite block type
bindings/    one file per process with bindings
links.json   the links between processes
```

The files are written so that a power cut leaves either the old or the new version, never half a
file. Their names are the processes' ids and the types' names, encoded. Don't edit them while
hakkas runs.

**At start**, hakkas loads every process in `graphs/`:
1. It applies each process's bindings.
2. It makes the links.
3. It starts the processes that were saved running.

Nothing in a saved file stops a start:
- a file that cannot be read is skipped, with an error in the log, and the others run;
- a binding or a link that does not fit is skipped, with a warning;
- a missing block type leaves a placeholder (Section 6.4).

**Installing the standard library** of schedule types: copy its files into `types/` under the
names hakkas gives types, then restart:

```sh
for f in library/schedules/*.json; do cp "$f" "/var/lib/hakkas/types/schedule%2F$(basename "$f")"; done
```

Or upload them with *Upload…* in the editor (Section 9.2), which needs no restart.

**Backups**: *Back up* in the editor (Section 9.3) is the usual way. A copy of the data directory
also works, made while hakkas is stopped.

## 16.5 Logs

Hakkas logs to standard error, one line per event:

```
info host.c:143: 2 processes loaded, 2 started
warn environment.c:475: …/bindings/7c1e….json: bindings[1]: No index expected: slot2-dq: skipped
```

Each line has the level, the source, and the message. The levels are `fine` (everything), `info`,
`warn` (the default) and `error`. `--info` or `--fine` on the command line overrides the file for
one run, which is the first thing to try when something does not work (Appendix C). Under systemd,
`journalctl -u <service>` shows the log.

**Secrets never appear in the log**: no passwords, keys or session ids. Logins are logged with
the user name and address only.

With `log.mirror` on, the log also goes to every logged-in editor's *Messages* panel. Any user can
read it there, including the user names and addresses of logins.

## 16.6 Updating the editor

The editor is plain files in `webdir`. To update it, build the new version and replace the files:
- `frontend/dist` for a computer;
- `npm run build:device`'s `frontend/dist-device`, compressed, for a device's flash.

There is no need to restart hakkas: browsers get the new editor at their next reload.

Hakkas and its editor are versioned together. Update both from the same version.
