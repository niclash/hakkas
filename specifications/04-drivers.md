# 04 — Hardware IO Layer and Networking

> **Provisional.** IO events will come from the Zephyr environment. This document captures the current intent, but the IO sections (§1–§5, §7) are expected to change substantially once hakkas is actually hooked up to Zephyr. The fixed requirement is §0: **the core program must not depend on Zephyr**, so a complete development system runs on a desktop against simulated IO.

Hardware bring-up itself — devicetree authorship, board support, peripheral driver implementations — is the responsibility of a separate effort. This document specifies hakkas' side of the boundary: the interface through which the environment tells hakkas which external inputs and outputs exist (§1), and how a graph's ports get bound to them on a given deployment (§4).

## 0. The IO-provider boundary

The engine sees IO only through the PAL's **IO-provider interface** (`01-architecture-overview.md` §0), which deals in **channels**: each channel has a stable id, a kind (digital/analog, input/output), and optionally a unit, which is descriptive information for the user interface (values are plain numbers, `01-architecture-overview.md` §2.1). The interface:
- `io_enumerate()` — list the channels available this boot;
- `io_write(channel, value)` — hand a new value to an output channel (non-blocking; the provider does the actual hardware access on its own context);
- event delivery — the provider pushes `(channel, value)` **IO events** into the engine's queue via the PAL's cross-context enqueue function (`pal_handoff_push`, `01-architecture-overview.md` §0.2, §3.2) whenever an input channel's value changes. The handoff is bounded and drops its oldest entry when full, so a provider that produces faster than the engine drains loses the oldest readings, never blocks.

Two providers implement it:
- **Zephyr IO provider** (`platform/zephyr/`): the device firmware's environment (§1), reached through Zephyr's device driver subsystem; hakkas reads no module descriptors itself.
- **Simulated IO provider** (`platform/posix/`): a desktop simulator. It defines a set of virtual channels (from a simulation config file in the configuration directory) whose input values can be set externally — e.g. from a small local control socket or a script — and whose output writes are logged/observable. This is what lets the full system, including the environment bindings, run and be tested on an x86_64 desktop with no hardware. The exact simulator interface is an implementation detail.

Everything above the IO-provider interface (the bindings, delivering IO events to the bound block ports) is platform-independent and lives in `drivers/` (`01-architecture-overview.md` §6).

## 1. The environment and its interface (decided, open question 2)

There is no EEPROM and no module discovery by hakkas. The hardware platform these sections originally described no longer exists; hakkas does not read module descriptors, probe expansion slots, or drive registers itself.

Instead, the **environment** — whatever hosts hakkas and owns the physical inputs and outputs (the device firmware on Zephyr, a simulator on POSIX) — provides a **service provider interface** that hakkas queries for the external inputs and outputs that are available. The environment describes them in the same terms as block types (`01-architecture-overview.md` §2.3, `02-wire-protocol.md` §3.5): named, typed, described ports, with an icon and a description. The one principal difference from a block type is that an environment **cannot be instantiated**: it exists once, whether or not a graph uses it, and the user cannot create or delete it. Presenting it nicely in the user interface needs dedicated handling there.

**Description model** (decided, environment SPI design, step 1): the environment describes itself with the shape of a component descriptor (`02-wire-protocol.md` §3.5) — `name`, `description`, `icon`, `inPorts`, `outPorts` — seen **from the graph's side, as one block that cannot be instantiated**:
- its **output ports** are the plant's inputs to hakkas (sensors, digital inputs, meters): values that flow *into* graphs;
- its **input ports** are hakkas' outputs to the plant (relays, analog outputs): values graphs *send out*.
Each port has `id`, `type` (`number` or `bool`), `description`, and optionally `unit` and `min`/`max` (descriptive, for the user interface only — values are plain numbers, `01-architecture-overview.md` §2.1). A port that stands for a group of identical channels is **addressable** (`"addressable": true`) with a `count`: its channels are indexed `1…count`, as in the sister project's bindings (`{"port": "analog-in", "index": 1}`). Example:
```json
{"name": "emulator", "description": "Desktop simulator", "icon": "chip",
 "outPorts": [{"id": "analog-in",  "type": "number", "addressable": true, "count": 8, "unit": "V", "description": "0-10 V inputs"},
              {"id": "digital-in", "type": "bool",   "addressable": true, "count": 16}],
 "inPorts":  [{"id": "analog-out", "type": "number", "addressable": true, "count": 4, "unit": "V"},
              {"id": "relay",      "type": "bool",   "addressable": true, "count": 8}]}
```
User-facing naming from the plant's point of view (e.g. calling a sensor an "input") is a presentation matter for the user interface, e.g. via metadata; it does not change this model.

The environment interface also provides the **wall clock** (`00-README.md`, open question 11): calendar blocks (`03-block-types.md` §6.2) and wire timestamps read real time from it; on POSIX the simulated environment uses the host's clock.

**Interim** (decided, plan task 24): until this interface exists (plan task 22), the wall clock is the PAL's `pal_wall_ms()` (UTC ms since the Unix epoch; POSIX `CLOCK_REALTIME`, `01-architecture-overview.md` §0.1), read only by the engine loop, the startup and the local C API into the runtime (`01-architecture-overview.md` §3.4). Plan task 22 folds it into the environment interface: the runtime keeps taking the wall clock as an injected value, so only its source changes, and the step handling of `01-architecture-overview.md` §3.4 stays.

How a graph's ports are connected to the environment's ports — so that the same graph runs unchanged in different environments, by applying a different bindings file — is open question 3 (§4). How the environment appears in the graph model and on the wire is open question 6.

On the Zephyr build the environment's implementation (device drivers, devicetree, board support) belongs to the device firmware, not to hakkas; on POSIX it is the simulated provider (§0).

## 4. Discovery and binding

1. At boot, hakkas queries the environment (§1) for its available inputs and outputs.
2. *(Removed with the EEPROM, open question 2.)*
3. **Binding** (platform-independent, above the IO-provider interface): an ordinary block port (`"<block>.<port>"`) is associated with one environment channel; there are no `io/*` blocks (`03-block-types.md` §4). **A binding is a connection** between the environment block (§1) and an ordinary block (decided, environment SPI design, step 2), with the connection rules of `01-architecture-overview.md` §2.2:
   - a **sensor** — a channel of an environment *output* port — binds to a **block input**, and becomes that input's one source (no fan-in: the input can have no connection, initial or link besides it). One sensor channel may feed any number of block inputs, in any number of processes;
   - an **actuator** — a channel of an environment *input* port — binds to a **block output**. One block output may drive several actuators, but each actuator channel has exactly **one driver across the whole deployment**; a second binding to the same actuator channel is refused when the bindings are applied.
   There is no "tap" binding of an actuator to a block input. The sister project's `examples/bindings-emulator.json` predates this design and is not authoritative (it binds an actuator to a block input).
4. **Portability**: the same graph must run against different physical hardware configurations. The binding from a graph's logical IO reference to an actual discovered slot/channel is resolved per-deployment, at boot — not baked into the portable graph file. The graph itself contains no reference to hardware; a separate binding configuration maps its block ports to this environment's ports. The binding configuration is a **bindings file** per process and environment, paired with its process in the process list (`"bindings"` member, `05-app-lifecycle-persistence.md` §1.2). **Format** (decided, environment SPI design, step 3):
   ```json
   {"environment": "emulator",
    "bindings": [
      {"block": "block1", "port": "in1", "env": "analog-in",  "index": 1},
      {"block": "block7", "port": "out", "env": "analog-out", "index": 1}
    ]}
   ```
   - `environment` names the environment the file was written for; if it differs from the running environment's `name`, the file is refused (emulator bindings can't be applied to real hardware by mistake).
   - Each binding names a block port (`block`, `port`) and an environment port (`env`); `index` (1…`count`) is required for an addressable environment port and absent otherwise. The direction is not written: an environment output port is a sensor and must bind a block input, an environment input port is an actuator and must bind a block output.
   - **Errors on the graph side are configuration errors** that refuse startup (like a graph file that doesn't load or a link that is refused, `05-app-lifecycle-persistence.md` §1.2): an unknown block or port, the wrong direction, an input that already has a source, a second driver for an actuator channel, a malformed file or a mismatched `environment`. Graph files are normally made with the user interface and saved by hakkas, so a reference that doesn't resolve is a real error.
   - **A channel the environment doesn't provide is not an error** (item 6): an unknown environment port or an `index` beyond its `count` is skipped with a warning, and the block port stays inert — so one bindings file also works on a board variant with fewer channels.
5. An environment input delivers IO events to its bound block port, which propagates the value exactly like any other value change (`01-architecture-overview.md` §3); an environment output receives each new value of its bound block port through the IO provider (`io_write`, §0). An output keeps its last value whenever nothing drives it (`03-block-types.md` §4).
6. **Graceful degradation**: a logical IO reference with no live binding on a given deployment (the environment doesn't provide that input or output) holds its default value and takes no other action — it doesn't fail graph startup. This is what makes a graph portable across differently-configured hardware: references to IO points a particular board doesn't have simply stay inert.

## 5. What hakkas owns vs. the separate hardware effort

Hakkas owns: the environment interface it queries (§1, §0), the simulated environment on POSIX, the FBP-facing behavior of whatever represents the environment in graphs (open question 6), and the binding of graph ports to environment ports (§4) including graceful degradation.

The environment's side — on Zephyr the device firmware: devicetree, board support, device drivers, and the implementation of the environment interface — belongs to the separate hardware/firmware effort. There is no EEPROM descriptor format.

## 6. Networking on the Zephyr targets — proposal

Nothing here has been worked on yet; this is a proposal to start from. The device hosts the full WebSocket/FBP server and serves the frontend's static assets, on Zephyr's native IP stack and BSD-style socket API (`zephyr/net/socket.h`), never a Linux-specific library. See `01-architecture-overview.md` §6 for the module/library guidance and the PAL split that keeps this isolated from the POSIX build tree.

**Network interface hardware** — this is the first decision, and it needs the hardware effort:
- **STM32H5/H7/N6**: on-chip Ethernet MAC + external RMII PHY, supported by Zephyr's STM32 Ethernet driver. This is the most straightforward path.
- **RP2354 (RP2350)**: has no on-chip network interface. Options:
  - a **WIZnet W5500** SPI Ethernet controller, which has a Zephyr driver (`wiznet,w5500`) and runs Zephyr's own IP stack. **Recommended** for a wired controller.
  - an **Infineon CYW43439** Wi-Fi chip, as on the Pico 2 W. Verify Zephyr's driver support for it on the RP2350 before committing.
  - ENC28J60 (10 Mbit, also supported by Zephyr) as a low-cost fallback.

**IP configuration**:
- A DHCPv4 client (`CONFIG_NET_DHCPV4`), with a static address from the configuration file as override (`05-app-lifecycle-persistence.md` §1.1).
- IPv4 link-local addressing (`CONFIG_NET_IPV4_AUTO`) as fallback, so a laptop connected directly with a cable can still reach the device.

**Discovery**: an mDNS responder plus DNS-SD (`CONFIG_MDNS_RESPONDER`, `CONFIG_DNS_SD`) advertising:
- `<hostname>.local`;
- `_http._tcp` on `port`;
- `_fbp-ws._tcp` on `port+1`, with the TXT records the upstream FBP protocol defines (`fbp-protocol.html`, "Runtime discovery": `txtvers=1`, `id=<engine_id>`, …).

The user can then type `http://<hostname>.local:<port>/` instead of an IP address.

**HTTP + WebSocket server** — two options:
1. A small, portable HTTP/1.1 `GET` server plus an RFC 6455 WebSocket layer, written once against the PAL socket primitives and used by **both** build trees. It integrates directly into the single-threaded engine loop (`01-architecture-overview.md` §3.2.1). **Recommended**: the same code runs on desktop and device, and no cross-thread handoff is needed. The HTTP server is built this way (plan task 13, `05-app-lifecycle-persistence.md` §3); its buffer sizes and connection count are per-build overrides to size for the MCU.
2. Zephyr's built-in HTTP server (`CONFIG_HTTP_SERVER`, with static/gzip resources and WebSocket upgrade support). Less code to write, but it runs on its own thread(s), so every received frame would have to be handed into the engine queue, and the POSIX build would need a different server. Keep it as a fallback if code size or schedule demands.

**TLS**: optional, via Zephyr's mbedTLS-backed TLS sockets (`02-wire-protocol.md` §2.5 item 9); off by default.

**Wall clock**: the environment provides UTC (`04-drivers.md` §1); on Zephyr, SNTP (`CONFIG_SNTP`) or an RTC is the natural source — the firmware's business. Local time for calendar blocks comes from the `time_zone` setting (`00-README.md`, open question 11).

**Sizing**: socket count and network buffer Kconfig options (`CONFIG_NET_MAX_CONN`, buffer pools, …) are sized from `max_connections` + 1 listener per port + DNS/mDNS/DHCP. Web assets are stored pre-gzipped (`05-app-lifecycle-persistence.md` §3).

## 7. Addressable expansion buses (Modbus and similar)

If the hardware lineup ever needs Modbus-connected devices or other addressable-expansion-over-a-shared-bus patterns, implement them as another binding behind the same generic-driver/binding-table model (§3–4), not as a separate bespoke subsystem. Whether this is actually needed depends on the separate hardware effort's design.
