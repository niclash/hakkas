# 04 — Hardware IO Layer and Networking

> **Provisional.** IO events will come from the Zephyr environment. This document captures the current intent, but the IO sections (§1–§5, §7) are expected to change substantially once hakkas is actually hooked up to Zephyr. The fixed requirement is §0: **the core program must not depend on Zephyr**, so a complete development system runs on a desktop against simulated IO.

Hardware bring-up itself — devicetree authorship, board support, peripheral driver implementations — is the responsibility of a separate effort. This document specifies hakkas' side of the boundary: the interface through which the environment tells hakkas which external inputs and outputs exist (§1), and how a graph's ports get bound to them on a given deployment (§4).

## 0. The IO-provider boundary

The engine sees IO only through the PAL's **IO-provider interface** (`01-architecture-overview.md` §0), which deals in **channels**: each channel has a stable id, a kind (digital/analog, input/output), and for analog channels a `Metric` category/unit. The interface:
- `io_enumerate()` — list the channels available this boot;
- `io_write(channel, value)` — hand a new value to an output channel (non-blocking; the provider does the actual hardware access on its own context);
- event delivery — the provider pushes `(channel, value)` **IO events** into the engine's queue via the PAL's cross-context enqueue function (`pal_handoff_push`, `01-architecture-overview.md` §0.2, §3.2) whenever an input channel's value changes. The handoff is bounded and drops its oldest entry when full, so a provider that produces faster than the engine drains loses the oldest readings, never blocks.

Two providers implement it:
- **Zephyr IO provider** (`platform/zephyr/`): the device firmware's environment (§1), reached through Zephyr's device driver subsystem; hakkas reads no module descriptors itself.
- **Simulated IO provider** (`platform/posix/`): a desktop simulator. It defines a set of virtual channels (from a simulation config file in the configuration directory) whose input values can be set externally — e.g. from a small local control socket or a script — and whose output writes are logged/observable. This is what lets the full system, including `io/*` blocks, run and be tested on an x86_64 desktop with no hardware. The exact simulator interface is an implementation detail.

Everything above the IO-provider interface (the `io/*` block types, the binding table, delivering IO events to blocks) is platform-independent and lives in `drivers/` (`01-architecture-overview.md` §6).

## 1. The environment and its interface (decided, open question 2)

There is no EEPROM and no module discovery by hakkas. The hardware platform these sections originally described no longer exists; hakkas does not read module descriptors, probe expansion slots, or drive registers itself.

Instead, the **environment** — whatever hosts hakkas and owns the physical inputs and outputs (the device firmware on Zephyr, a simulator on POSIX) — provides a **service provider interface** that hakkas queries for the external inputs and outputs that are available. The environment describes them in the same terms as block types (`01-architecture-overview.md` §2.3, `02-wire-protocol.md` §3.5): named, typed, described ports, with an icon and a description. The one principal difference from a block type is that an environment **cannot be instantiated**: it exists once, whether or not a graph uses it, and the user cannot create or delete it. Presenting it nicely in the user interface needs dedicated handling there.

How a graph's ports are connected to the environment's ports — so that the same graph runs unchanged in different environments, by applying a different bindings file — is open question 3 (§4). How the environment appears in the graph model and on the wire is open question 6.

On the Zephyr build the environment's implementation (device drivers, devicetree, board support) belongs to the device firmware, not to hakkas; on POSIX it is the simulated provider (§0).

## 4. Discovery and binding

1. At boot, hakkas queries the environment (§1) for its available inputs and outputs.
2. *(Removed with the EEPROM, open question 2.)*
3. **Binding** (platform-independent, above the IO-provider interface): a logical `io/*` block port (e.g. an `AnalogInput` instance's `out` port) is associated with one discovered physical channel.
4. **Portability**: the same graph must run against different physical hardware configurations. The binding from a graph's logical IO reference to an actual discovered slot/channel is resolved per-deployment, at boot — not baked into the portable graph file. A graph references physical IO by a stable logical identifier (e.g. `"sensor1"`); a separate, per-device binding configuration maps that identifier to a specific discovered slot/channel on this board. The exact form of that binding configuration (a separate file, a section of boot configuration, etc.) is open (`00-README.md`, open question 3). So is where an `io/*` block instance records its logical identifier (its instance name, a field in its node metadata, …).
5. An input block receives IO events for its bound channel and propagates the value exactly like any other block's output (`01-architecture-overview.md` §3); an output block hands each new value to the IO provider (`io_write`, §0) on `update()`.
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

**Wall clock**: SNTP (`CONFIG_SNTP`) is the natural future source of UTC time (`00-README.md`, open question 11).

**Sizing**: socket count and network buffer Kconfig options (`CONFIG_NET_MAX_CONN`, buffer pools, …) are sized from `max_connections` + 1 listener per port + DNS/mDNS/DHCP. Web assets are stored pre-gzipped (`05-app-lifecycle-persistence.md` §3).

## 7. Addressable expansion buses (Modbus and similar)

If the hardware lineup ever needs Modbus-connected devices or other addressable-expansion-over-a-shared-bus patterns, implement them as another binding behind the same generic-driver/binding-table model (§3–4), not as a separate bespoke subsystem. Whether this is actually needed depends on the separate hardware effort's design.
