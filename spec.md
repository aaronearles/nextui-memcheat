# memcheat.pak — Specification

A NextUI tool pak for the TrimUI Brick that runs a background memory-scanning
daemon with a web UI. The user starts it on demand from the Tools menu, scans a
running game's memory from any browser on the same WiFi network, freezes and
modifies values, and exports results as a RetroArch `.cht` cheat file.

---

## Reference Material

The pak shell scaffolding is modelled directly on
[josegonzalez/minui-dropbear-server-pak](https://github.com/josegonzalez/minui-dropbear-server-pak).
Read that repo's `launch.sh` and `settings.json` before touching any shell
code.  The UI toggle pattern (Enable / Start on boot, minui-list driven, PID
and address shown when running) must match that pak's behaviour exactly.

josegonzalez's
[pak authoring guide](https://josediazgonzalez.com/2025/06/16/writing-a-pak-for-the-minui-and-nextui-launchers/)
explains environment variables, BusyBox constraints, and the `auto.sh` boot
hook pattern.

---

## Target Platform

| Property | Value |
|---|---|
| Device | TrimUI Brick |
| Platform token | `tg5040` |
| CPU | Allwinner A133 Plus — 4× Cortex-A53, aarch64 |
| RAM | 1 GB LPDDR3 (shared with OS and running emulator) |
| OS | Linux, BusyBox userland, `/proc` mounted |
| WiFi | 802.11 b/g/n — present and working under NextUI |
| Firmware | NextUI (MinUI fork) |

The binary **must** be a fully static aarch64 executable.  There is no
guarantee of glibc or any shared library on the device.  Use musl libc via the
`aarch64-linux-musl` cross-compiler.

---

## Repository Layout

```
memcheat.pak/
├── spec.md                        ← this file
├── README.md
├── LICENSE
├── Makefile
├── pak.json
├── settings.json                  ← minui-list toggle definitions
├── launch.sh                      ← Tools menu entry point
│
├── src/                           ← C daemon source
│   ├── main.c
│   ├── api.c / api.h              ← HTTP route handlers
│   ├── scanner.c / scanner.h      ← memory scan logic
│   ├── watch.c / watch.h          ← watch list + freeze loop
│   ├── procutil.c / procutil.h    ← /proc helpers, PID detection
│   ├── cht.c / cht.h              ← .cht file writer
│   └── mongoose.c / mongoose.h    ← embedded HTTP+WS library (vendored)
│
├── www/
│   └── index.html                 ← entire web UI (single file, no build step)
│
└── bin/
    ├── aarch64/
    │   └── memcheat               ← compiled static binary (git-ignored, built by Makefile)
    ├── shared/
    │   ├── on-boot                ← called by auto.sh on NextUI start
    │   ├── service-on             ← starts the daemon
    │   ├── service-off            ← kills the daemon
    │   └── service-is-running     ← exits 0 if daemon is alive
    └── tg5040/
        ├── jq                     ← static aarch64 jq binary (vendored)
        ├── minui-list             ← from josegonzalez/pakman release
        └── minui-presenter        ← from josegonzalez/pakman release
```

`bin/aarch64/memcheat` and the `bin/tg5040/` binaries are not committed;
the Makefile downloads or builds them.

---

## Environment Variables

NextUI sets these before running any pak script:

| Variable | Example value |
|---|---|
| `PLATFORM` | `tg5040` |
| `SDCARD_PATH` | `/mnt/sdcard` |
| `USERDATA_PATH` | `/mnt/sdcard/.userdata/tg5040` |
| `LOGS_PATH` | `/mnt/sdcard/.userdata/tg5040/logs` |
| `SHARED_USERDATA_PATH` | `/mnt/sdcard/.userdata` |

Persistent state for this pak lives at `$USERDATA_PATH/memcheat.pak/`.
The daemon writes its PID file there.

---

## pak.json

```json
{
  "name": "Memory Editor",
  "version": "0.1.0",
  "author": "",
  "platforms": ["tg5040"]
}
```

---

## settings.json

Matches the dropbear pak's schema exactly — `minui-list` reads this file.

```json
{
  "settings": [
    {
      "name": "Enable",
      "options": ["false", "true"],
      "selected": 0
    },
    {
      "name": "Start on boot",
      "options": ["false", "true"],
      "selected": 0
    }
  ]
}
```

`launch.sh` reads the live state at runtime and patches `selected` before
handing the JSON to `minui-list`, exactly as the dropbear pak does.

---

## launch.sh

Behaviour must match the dropbear pak's `launch.sh` pattern:

1. Standard pak header (set PAK_DIR, PAK_NAME, redirect logs, cd, set PATH).
2. Set `SERVICE_NAME="memcheat"`, `HUMAN_READABLE_NAME="Memory Editor"`,
   `NETWORK_PORT=8080`, `NETWORK_SCHEME="http"`.
3. Define the same helper functions: `show_message`, `enable_start_on_boot`,
   `disable_start_on_boot`, `will_start_on_boot`, `wait_for_service`,
   `wait_for_service_to_stop`, `get_service_pid`, `get_ip_address`,
   `current_settings`, `main_screen`, `cleanup`.
4. `get_ip_address` returns `http://<wlan0_ip>:8080` when WiFi is up, shown
   as a read-only item in the `minui-list` display when the daemon is running.
5. Main loop: show `minui-list`, compare old vs new `selected` values, call
   `service-on` / `service-off` and `enable_start_on_boot` /
   `disable_start_on_boot` accordingly.
6. Write `1` to `/tmp/stay_awake` to prevent the device sleeping while the
   menu is open.
7. Exit cleanly on B button (exit code 2) or Menu button (exit code 3).

### bin/shared/service-on

```sh
#!/bin/sh
PAK_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
mkdir -p "$USERDATA_PATH/memcheat.pak"
"$PAK_DIR/bin/aarch64/memcheat" \
    --port 8080 \
    --www "$PAK_DIR/www" \
    --pidfile "$USERDATA_PATH/memcheat.pak/memcheat.pid" \
    --logfile "$LOGS_PATH/memcheat.txt" \
    --watchfile "$USERDATA_PATH/memcheat.pak/watchlist.json" \
    --cheatdir "$SDCARD_PATH/Cheats" \
    &
```

### bin/shared/service-off

```sh
#!/bin/sh
pidfile="$USERDATA_PATH/memcheat.pak/memcheat.pid"
if [ -f "$pidfile" ]; then
    kill "$(cat "$pidfile")" 2>/dev/null
    rm -f "$pidfile"
fi
# belt-and-suspenders
pkill -f memcheat 2>/dev/null || true
```

### bin/shared/service-is-running

```sh
#!/bin/sh
pidfile="$USERDATA_PATH/memcheat.pak/memcheat.pid"
[ -f "$pidfile" ] && kill -0 "$(cat "$pidfile")" 2>/dev/null
```

### bin/shared/on-boot

Called by `auto.sh` when "Start on boot" is enabled.  Sources the same
environment and invokes `service-on` in the background:

```sh
#!/bin/sh
BIN_DIR="$(dirname "$0")"
PAK_DIR="$(cd "$BIN_DIR/../.." && pwd)"
# inherit NextUI env vars, then start
"$BIN_DIR/service-on" &
```

---

## Daemon: memcheat (C binary)

### Command-line flags

| Flag | Default | Description |
|---|---|---|
| `--port N` | `8080` | HTTP listen port |
| `--www PATH` | `./www` | Directory to serve static files from |
| `--pidfile PATH` | `/tmp/memcheat.pid` | Write daemon PID here on start |
| `--logfile PATH` | stderr | Append log output here |
| `--watchfile PATH` | `/tmp/watchlist.json` | Persist watch list across restarts |
| `--cheatdir PATH` | `/tmp` | Root directory for `.cht` export |

### Source files

**main.c** — argument parsing, daemonisation (write PID file, redirect
stdout/stderr to logfile), initialise subsystems, start mongoose event loop.

**scanner.c / scanner.h** — wraps the memory scan logic.

- `scanner_t` struct holds: target PID, array of candidate matches
  `{addr, last_value}`, match count, data width, scan state enum.
- `scanner_attach(pid)` — validates the PID exists, stores it, resets state.
- `scanner_detach()` — clears all state, does not kill the target.
- `scanner_first_scan(value, width, op)` — reads `/proc/<pid>/maps` to find
  readable writable regions, then uses `process_vm_readv` to read each region
  and collect addresses whose value matches `op(value)`.  Ops: `SCAN_EQ`,
  `SCAN_LT`, `SCAN_GT`.
- `scanner_refine(value, op)` — re-reads only current candidate addresses and
  filters.  Additional ops: `SCAN_CHANGED`, `SCAN_UNCHANGED`, `SCAN_INC`,
  `SCAN_DEC`.
- `scanner_reset()` — free candidate list, reset to idle.
- `scanner_read(addr, width, out_value)` — single address read via
  `process_vm_readv`.
- `scanner_write(addr, width, value)` — single address write via
  `process_vm_writev`.
- Candidate list is a flat heap-allocated array; realloc'd in chunks of 4096
  entries.  Cap display at 500 entries in the API response; always report
  total count.

> **Do not use ptrace for scanning.**  `process_vm_readv` /
> `process_vm_writev` do not pause the target process and have lower overhead.
> Only fall back to `/proc/<pid>/mem` if `process_vm_readv` returns `ENOSYS`
> (kernel too old — unlikely on this device).

**watch.c / watch.h** — watch list and freeze loop.

- `watch_entry_t`: `{ uint64_t addr; char label[64]; int width; int frozen;
  uint64_t freeze_value; }`.
- Up to 64 watch entries (sufficient for cheat development).
- `watch_add`, `watch_remove`, `watch_write_value`, `watch_set_freeze`.
- `watch_load(path)` / `watch_save(path)` — serialise to/from JSON manually
  (no JSON library dependency; the format is simple enough to write by hand
  with `fprintf`/`sscanf`).
- Freeze loop: a `pthread` that wakes every 100 ms, iterates all frozen
  entries, and calls `scanner_write` for each.  Sleeps immediately if the
  scanner has no attached PID.

**procutil.c / procutil.h**

- `procutil_list(results, max)` — scan `/proc/*/cmdline`, return array of
  `{pid, name}` for processes whose name matches a known emulator list:
  `retroarch`, `gambatte_libretro`, `snes9x`, `mgba`, `pcsx_rearmed`,
  `fbalpha2012`, and any process whose cmdline contains `.pak`.  Return all
  non-kernel processes as a fallback if the known list yields nothing.
- `procutil_name(pid, buf, len)` — read `/proc/<pid>/comm`.
- `procutil_exists(pid)` — `kill(pid, 0) == 0`.

**cht.c / cht.h**

- `cht_export(path, game_name, watch_entries, count)` — write a RetroArch
  cheat file.

Output format:

```
cheats = N

cheat0_desc = "Label"
cheat0_code = "XXXXXXXX+YYYYYYYY"
cheat0_enable = false

cheat1_desc = "Label"
...
```

`code` field: `%08X+%08X` formatted as `address+value` (uppercase hex, no
`0x` prefix).  This is the RetroArch-handled cheat format for direct memory
writes.

The file is written to `<cheatdir>/<system>/<game_name>.cht` if a system
subdirectory can be inferred, otherwise directly to `<cheatdir>/<game_name>.cht`.
System inference: not attempted in v0.1 — always write flat to `cheatdir`.

**api.c / api.h** — mongoose HTTP event handler.  Routes listed below.

**mongoose.c / mongoose.h** — vendored from
https://github.com/cesanta/mongoose (single-file embed).  Use the latest
stable release.  HTTP + WebSocket in one event loop, no external dependencies.

### HTTP API

All request/response bodies are JSON.  All endpoints return
`Content-Type: application/json`.  Static files (anything not matching `/api/`
or `/ws`) are served from the `--www` directory.

```
GET  /api/status
```
Response:
```json
{
  "running": true,
  "attached_pid": 1234,
  "process_name": "retroarch",
  "scan_state": "idle",
  "candidate_count": 0,
  "watch_count": 2
}
```
`scan_state` values: `"idle"`, `"has_results"`.

---

```
GET  /api/processes
```
Response:
```json
[
  { "pid": 1234, "name": "retroarch" },
  { "pid": 1235, "name": "mgba" }
]
```

---

```
POST /api/attach
Body: { "pid": 1234 }
```
Response: `{ "ok": true }` or `{ "ok": false, "error": "..." }`.
Resets scan state.  Does not reset watch list.

---

```
POST /api/detach
```
Response: `{ "ok": true }`.
Clears attached PID and scan state.  Does not stop the daemon.

---

```
POST /api/scan/start
Body: {
  "value": 100,
  "width": 4,
  "op": "eq"
}
```
`width`: 1, 2, 4, or 8 (bytes).  `op`: `"eq"`, `"lt"`, `"gt"`.
Response: `{ "ok": true, "candidate_count": 4821 }`.
Replaces any existing scan.

---

```
POST /api/scan/refine
Body: {
  "value": 95,
  "op": "eq"
}
```
`op`: `"eq"`, `"lt"`, `"gt"`, `"changed"`, `"unchanged"`, `"inc"`, `"dec"`.
`value` is ignored for `changed`, `unchanged`, `inc`, `dec`.
Response: `{ "ok": true, "candidate_count": 3 }`.

---

```
POST /api/scan/reset
```
Response: `{ "ok": true }`.

---

```
GET  /api/scan/results?offset=0&limit=100
```
Returns up to `limit` (max 500) candidates starting at `offset`.
Response:
```json
{
  "total": 4821,
  "offset": 0,
  "results": [
    { "addr": "0x00AB04", "value": 95 },
    ...
  ]
}
```
Addresses are returned as hex strings with `0x` prefix.

---

```
GET  /api/watch
```
Response:
```json
[
  {
    "addr": "0x00AB04",
    "label": "Lives",
    "width": 4,
    "value": 95,
    "frozen": false,
    "freeze_value": 0
  }
]
```
`value` is the current live reading; 0 if the PID is not attached.

---

```
POST /api/watch
Body: { "addr": "0x00AB04", "label": "Lives", "width": 4 }
```
Response: `{ "ok": true }` or `{ "ok": false, "error": "already exists" }`.

---

```
DELETE /api/watch/:addr
```
`:addr` is the hex address string (e.g. `0x00AB04`).
Response: `{ "ok": true }`.

---

```
POST /api/watch/:addr/write
Body: { "value": 99 }
```
Response: `{ "ok": true }` or `{ "ok": false, "error": "..." }`.

---

```
POST /api/watch/:addr/freeze
Body: { "enabled": true, "value": 99 }
```
Response: `{ "ok": true }`.

---

```
POST /api/watch/:addr/label
Body: { "label": "Lives" }
```
Response: `{ "ok": true }`.

---

```
POST /api/export/cht
Body: { "game_name": "Super Mario World" }
```
Writes `<cheatdir>/Super Mario World.cht`.
Response: `{ "ok": true, "path": "/mnt/sdcard/Cheats/Super Mario World.cht" }`.

---

```
WS /ws
```
On connect, the daemon sends the current watch list state immediately.  After
that, every 500 ms it broadcasts a live-values update to all connected clients:
```json
{
  "type": "watch_update",
  "entries": [
    { "addr": "0x00AB04", "value": 95 },
    { "addr": "0x00AB08", "value": 255 }
  ]
}
```
Clients should handle `type: "watch_update"` to refresh displayed values.

---

## Web UI (www/index.html)

Single HTML file.  Vanilla JS only — no framework, no build step, no external
CDN resources.  Must work offline on the device's local network.  The daemon
serves it as a static file.

### Layout

Three collapsible sections on a single page, styled for comfortable mobile use
(large tap targets, readable on a phone screen).

**Section 1 — Process**
- "Refresh" button calls `GET /api/processes` and populates a `<select>`.
- "Attach" button calls `POST /api/attach` with selected PID.
- Status line: "Attached to retroarch (PID 1234)" or "Not attached".

**Section 2 — Scanner**
- Value input (number).
- Width selector: 1 byte / 2 bytes / 4 bytes (default) / 8 bytes.
- Op selector for first scan: Equal / Less than / Greater than.
- Op selector for refine: Equal / Less than / Greater than / Changed /
  Unchanged / Increased / Decreased.
- "First Scan" button — calls `/api/scan/start`, updates result count.
- "Refine" button — calls `/api/scan/refine`, updates result count.
- "Reset" button — calls `/api/scan/reset`.
- Result count display: "4821 candidates" or "3 candidates — add to watch list".
- Results table (shown when ≤ 500 candidates): Address | Value | [Add] button.
  If total > 500 show "Showing 500 of N — refine further to narrow results."
- Clicking [Add] on a row opens a small inline prompt for a label, then calls
  `POST /api/watch`.

**Section 3 — Watch List**
- Table: Label | Address | Value | Freeze | Write | Remove.
- Label cell: click to edit inline, calls `/api/watch/:addr/label` on blur.
- Value cell: updated live via WebSocket.
- Freeze toggle: checkbox, calls `/api/watch/:addr/freeze`.  When checked,
  reveals a freeze-value input.
- Write button: prompts for value inline, calls `/api/watch/:addr/write`.
- Remove button: calls `DELETE /api/watch/:addr`.
- "Game name" text input + "Export .cht" button at the bottom.  Calls
  `/api/export/cht`, shows the returned path on success.

### WebSocket handling

Open `ws://<host>/ws` on page load.  On disconnect, retry with exponential
backoff (1 s, 2 s, 4 s, max 30 s).  On `watch_update` message, update the
Value cells in the watch list table.

### Style

Dark background (#1a1a1a), light text, accent colour #e8a020 (warm amber —
evokes a classic trainer tool aesthetic without being garish).  No external
fonts.  System sans-serif stack.  Minimum tap target 44 px height.

---

## Build System (Makefile)

### Targets

```
make all        — cross-compile memcheat, download tool binaries, package zip
make daemon     — cross-compile src/ → bin/aarch64/memcheat (static, aarch64)
make tools      — download jq, minui-list, minui-presenter for tg5040
make package    — zip the pak directory → Memory\ Editor.pak.zip
make clean      — remove bin/aarch64/memcheat and build artefacts
```

### Cross-compiler

Use `aarch64-linux-musl-gcc` from the musl cross-compiler suite.  On macOS,
install via Homebrew: `brew install FiloSottile/musl-cross/musl-cross`.  On
Linux (Debian/Ubuntu): download from
`https://musl.cc/aarch64-linux-musl-cross.tgz` and add to `PATH`.

Compile flags:
```
CC = aarch64-linux-musl-gcc
CFLAGS = -std=c99 -O2 -Wall -Wextra -static -pthread
LDFLAGS = -static -pthread
```

No external libraries beyond libc and libpthread (both included in the musl
static toolchain).  `libscanmem` is **not** used — scanner logic is
implemented directly in `scanner.c` using `process_vm_readv`.  This avoids a
cross-compile dependency and keeps the binary fully self-contained.

### Tool binary downloads

`minui-list` and `minui-presenter` are downloaded from the latest
josegonzalez/pakman release asset for `tg5040`.  `jq` is downloaded from the
jq GitHub releases page (static aarch64 build).  The Makefile should check
whether these files already exist before downloading.

---

## .cht File Placement

RetroArch on NextUI looks for cheat files at:

```
$SDCARD_PATH/Cheats/<GameName>.cht
```

or, with system subdirectory (RetroArch convention):

```
$SDCARD_PATH/Cheats/<System>/<GameName>.cht
```

In v0.1, write directly to `<cheatdir>/<game_name>.cht`.  The `cheatdir`
flag defaults to `$SDCARD_PATH/Cheats` when launched from `service-on`, so
the user only needs to enter the game name.

---

## Known Constraints and Gotchas

**RetroArch memory offsets.** RetroArch-handled cheats use addresses relative
to the emulated system's memory map inside the libretro core, not absolute
process virtual addresses.  When the user scans against the `retroarch`
process, the addresses they find are absolute virtual addresses within the
retroarch process.  These are valid for `process_vm_writev` (freeze/write will
work), but may not match the relative offsets RetroArch expects in a `.cht`
file.  Document this clearly in the UI: add a note below the export button
reading "Exported addresses are raw process addresses. If cheats do not apply
correctly in RetroArch, you may need to subtract the core's base memory
offset."  Do not attempt to automate the offset calculation in v0.1.

**1 GB RAM limit.** During a first scan of a large process, the candidate list
may hold millions of entries.  Cap the in-memory candidate array at 2 million
entries (each entry is 12 bytes on aarch64 — addr uint64 + value uint32 =
~24 MB max).  If the cap is reached, return `{ "candidate_count": 2000000,
"capped": true }` and prompt the user to refine.

**PID changes between sessions.** The daemon does not auto-reattach.  If the
user exits a game and relaunches it, they must use the Process section to
attach to the new PID.  The watch list persists across reattachments (addresses
are stored; values will simply fail to read until a new PID is attached).

**Root privileges.** `process_vm_readv` on Linux requires that the calling
process either owns the target process or has `CAP_SYS_PTRACE`.  NextUI paks
run as root, so this is not an issue.  The daemon should log a clear error if
the readv syscall returns `EPERM` anyway.

**BusyBox `sh`, not bash.** All shell scripts must use `/bin/sh` and avoid
bashisms.  No arrays, no `[[`, no `<()` process substitution.  Use `pgrep`,
`grep`, `awk`, `sed`, `jq` only.

**`jq` dependency.** `launch.sh` uses `jq` to manipulate the settings JSON.
The static `jq` binary is bundled in `bin/tg5040/` and added to PATH in the
pak header, matching the dropbear pak's approach.

**WiFi must be up.** The daemon binds to `0.0.0.0:8080` so it is reachable
regardless of interface, but the address shown in the Tools menu is only
populated when `wlan0` has an IP.  If WiFi is not connected, `get_ip_address`
returns "Not connected to WiFi" and that string is shown as the Address item
instead of a URL.

---

## Out of Scope for v0.1

- Pointer scanning
- Code patching / NOP injection
- Authentication / access control
- HTTPS
- Auto-detection of RetroArch core memory base address for offset calculation
- System subdirectory inference for `.cht` export
- Multi-device / multiple simultaneous browser clients (WebSocket broadcast
  handles this naturally but is untested)
