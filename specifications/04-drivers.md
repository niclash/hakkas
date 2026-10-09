# 04 — Hardware IO Layer and Networking

> **Provisional.** IO events will come from the Zephyr environment. This document captures the current intent, but the IO sections (§1–§5, §7) are expected to change substantially once hakkas is actually hooked up to Zephyr. The fixed requirement is §0: **the core program must not depend on Zephyr**, so a complete development system runs on a desktop against simulated IO.

Hardware bring-up itself — devicetree authorship, board support, peripheral driver implementations — is the responsibility of a separate effort. This document specifies hakkas' side of the boundary: how it discovers what hardware is present at boot, how it drives it generically without per-module-type firmware changes, and how a graph's logical IO references get bound to actual physical channels on a given deployment.

## 0. The IO-provider boundary

The engine sees IO only through the PAL's **IO-provider interface** (`01-architecture-overview.md` §0), which deals in **channels**: each channel has a stable id, a kind (digital/analog, input/output), and for analog channels a `Metric` category/unit. The interface:
- `io_enumerate()` — list the channels available this boot;
- `io_write(channel, value)` — hand a new value to an output channel (non-blocking; the provider does the actual hardware access on its own context);
- event delivery — the provider pushes `(channel, value)` **IO events** into the engine's queue via the PAL's cross-context enqueue function (`01-architecture-overview.md` §3.2) whenever an input channel's value changes.

Two providers implement it:
- **Zephyr IO provider** (`platform/zephyr/`): the boot-time discovery, descriptor-driven driver, and polling described in §1–§3.
- **Simulated IO provider** (`platform/posix/`): a desktop simulator. It defines a set of virtual channels (from a simulation config file in the configuration directory) whose input values can be set externally — e.g. from a small local control socket or a script — and whose output writes are logged/observable. This is what lets the full system, including `io/*` blocks, run and be tested on an x86_64 desktop with no hardware. The exact simulator interface is an implementation detail.

Everything above the IO-provider interface (the `io/*` block types, the binding table, delivering IO events to blocks) is platform-independent and lives in `drivers/` (`01-architecture-overview.md` §6).

## 1. Zephyr IO provider: boot-time discovery, then steady-state polling (not hot-pluggable)

Hardware identification happens once, at boot. Two phases:

1. **Boot-time discovery**: for each physical expansion slot the board provides (a fixed, build-time-known set — see §2 on devicetree's role), read the identity/descriptor data from that slot's onboard EEPROM over I2C, if a module is actually present and responds. A slot with no responding module is handled gracefully — logged and skipped, not a fatal error, since a given board may have fewer modules installed than slots available.
2. **Steady-state polling**: once a slot's module is identified and configured, a dedicated worker polls that module's live data on an ongoing basis and feeds readings into the engine's single work queue as IO events (§0; `01-architecture-overview.md` §3.2, §3.2.1 — the worker must only *enqueue* readings, never call block logic directly, since it runs outside the engine's own thread). Only readings that changed are enqueued.

There is no third phase: a module removed or inserted after boot is not detected until the next reboot.

## 2. Devicetree's role vs. the EEPROM's role

These are complementary, not overlapping:
- **Devicetree** (owned by the separate hardware effort, static per board/build): defines the board's fixed physical topology — which I2C buses and GPIO lines exist, and which bus/address range is reserved for each physical expansion slot. This is "what wiring does this board have," known at compile time.
- **Per-module EEPROM content** (read at runtime, varies with whatever is physically installed): identifies what is actually plugged into each of those devicetree-defined slots this boot — manufacturer/device/revision data, or richer descriptor data (§3). This is "what's actually there right now," not knowable until the EEPROM is read.

Both pieces are needed: devicetree says where the expansion slots are; the EEPROM says what's in each one today.

## 3. The driver is generic and descriptor-driven, not a fixed enum

Supporting a new module type must never require a firmware update. That rules out identifying a module via a numeric ID that selects among a fixed set of compiled-in driver implementations — adding a new module type under that scheme would require a new compiled case and a reflash.

**The EEPROM must therefore carry enough information for one generic driver to configure itself per module**: the IO channels the module exposes, their types (digital/analog/etc.), register addresses or access patterns, scaling/units, and polling requirements — not just an ID. The firmware's job is: read the descriptor, build an in-memory channel table for that slot, and drive every channel through one generic read/write/poll engine — the same generalization principle as the block-type runtime in `01-architecture-overview.md` §2.3.

The exact byte layout of this descriptor is not specified here — it depends on what the separate hardware effort defines, or needs to be designed jointly (`00-README.md`, open question 2).

## 4. Discovery and binding

1. At boot, for each devicetree-defined expansion slot, attempt to read that slot's EEPROM over its I2C bus.
2. For each slot that responds, parse its descriptor (§3) and configure the generic driver's channel table for that slot.
3. **Binding** (platform-independent, above the IO-provider interface): a logical `io/*` block port (e.g. an `AnalogInput` instance's `out` port) is associated with one discovered physical channel.
4. **Portability**: the same graph must run against different physical hardware configurations. The binding from a graph's logical IO reference to an actual discovered slot/channel is resolved per-deployment, at boot — not baked into the portable graph file. A graph references physical IO by a stable logical identifier (e.g. `"sensor1"`); a separate, per-device binding configuration maps that identifier to a specific discovered slot/channel on this board. The exact form of that binding configuration (a separate file, a section of boot configuration, etc.) is open (`00-README.md`, open question 3). So is where an `io/*` block instance records its logical identifier (its instance name, a field in its node metadata, …).
5. An input block receives IO events for its bound channel and propagates the value exactly like any other block's output (`01-architecture-overview.md` §3); an output block hands each new value to the IO provider (`io_write`, §0) on `update()`.
6. **Graceful degradation**: a logical IO reference with no live binding on a given deployment (module absent, or this board variant doesn't have it) holds its default value and takes no other action — it doesn't fail graph startup. This is what makes a graph portable across differently-configured hardware: references to IO points a particular board doesn't have simply stay inert.

## 5. What hakkas owns vs. the separate hardware effort

Hakkas owns: the IO-provider interface and the simulated provider (§0), the `io/*` block types' FBP-facing behavior (`03-block-types.md` §4), the generic descriptor-driven driver engine (§3), the boot-time discovery and steady-state polling worker (§1), and the logical-identifier-to-physical-channel binding table (§4) including graceful degradation.

The separate hardware effort owns: devicetree authorship, Zephyr board support, and — jointly with hakkas at the format-definition stage — the EEPROM descriptor format itself, since hakkas' generic driver is what has to interpret it.

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
1. A small, portable HTTP/1.1 `GET` server plus an RFC 6455 WebSocket layer, written once against the PAL socket primitives and used by **both** build trees. It integrates directly into the single-threaded engine loop (`01-architecture-overview.md` §3.2.1). **Recommended**: the same code runs on desktop and device, and no cross-thread handoff is needed.
2. Zephyr's built-in HTTP server (`CONFIG_HTTP_SERVER`, with static/gzip resources and WebSocket upgrade support). Less code to write, but it runs on its own thread(s), so every received frame would have to be handed into the engine queue, and the POSIX build would need a different server. Keep it as a fallback if code size or schedule demands.

**TLS**: optional, via Zephyr's mbedTLS-backed TLS sockets (`02-wire-protocol.md` §2.5 item 9); off by default.

**Wall clock**: SNTP (`CONFIG_SNTP`) is the natural future source of UTC time (`00-README.md`, open question 11).

**Sizing**: socket count and network buffer Kconfig options (`CONFIG_NET_MAX_CONN`, buffer pools, …) are sized from `max_connections` + 1 listener per port + DNS/mDNS/DHCP. Web assets are stored pre-gzipped (`05-app-lifecycle-persistence.md` §3).

## 7. Addressable expansion buses (Modbus and similar)

If the hardware lineup ever needs Modbus-connected devices or other addressable-expansion-over-a-shared-bus patterns, implement them as another binding behind the same generic-driver/binding-table model (§3–4), not as a separate bespoke subsystem. Whether this is actually needed depends on the separate hardware effort's design.
