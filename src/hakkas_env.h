/*
 * The environment SPI (04 §0, §1; decided, environment SPI design, step 4;
 * the C types decided in plan task 22): how the environment — whatever hosts
 * hakkas and owns the physical inputs and outputs (the device firmware on
 * Zephyr, an external simulator program on POSIX, 04 §0.1) — tells hakkas
 * which inputs and outputs exist, delivers sensor values, takes actuator
 * values and provides the wall clock. One environment per runtime, optional,
 * handed to hakkas_create by the host (hakkas.h HakkasParams.environment).
 *
 * This header is public and portable, like hakkas.h (which includes it): an
 * environment implementation needs nothing else.
 *
 * The description (04 §1): the environment seen from the graph's side as one
 * block that cannot be instantiated. Its output ports (out_ports, JSON
 * "outPorts") are sensors: values that flow into graphs, bound to block
 * inputs. Its input ports (in_ports, "inPorts") are actuators: values graphs
 * send out, bound to block outputs. A port is addressable with count channels,
 * indexed 1..count, or a single channel, index 0 (decided, plan task 22: 0
 * stands for "no index", as a bindings file without "index"). A port's id is
 * unique across both directions (a binding names the port by id alone,
 * 04 §4). Modules (04 §1 "Modules and their metadata") and the carrier board
 * (04 §1 "The carrier board", plan task 61) are UI metadata only: what they
 * say is shown, not interpreted, and only their structure is validated (the
 * slots), not their view-only values (decided with the user 2026-10-25).
 *
 * Rules (04 §0):
 *  - describe, start, stop, write and wall_ms are called on the engine
 *    thread. describe is called once, in startup step 5 (05 §1.4); it may
 *    block until the environment can describe itself (the POSIX link waits
 *    for its program, 04 §0.1). The description must stay valid and unchanged
 *    until stop has returned.
 *  - start begins delivery. Nothing waits for sensor values (decided
 *    2026-10-13): processes start right after startup, from whatever their
 *    inputs hold — a sensor-bound input without a value yet holds its
 *    port's default — and each value propagates when it comes. Optionally,
 *    within start, on the engine thread, the environment may hand over
 *    values it already has with hakkas_env_sink_current, which the engine
 *    takes at once (no handoff, so no burst can overflow it); they are then
 *    there before any binding is applied or process started (startup steps
 *    5 and 7, 05 §1.4). Every other value goes through
 *    hakkas_env_sink_push. Protecting the plant at startup (e.g. a delay
 *    before actuators are driven) is the environment's job (04 §0 rule 2).
 *    0 is success; anything else refuses startup. The sink is valid until
 *    stop returns.
 *  - Values may be delivered from any thread (not an interrupt handler)
 *    with hakkas_env_sink_push; the engine takes them into its work queue
 *    through the PAL's handoff (pal_handoff_push, 01 §0.2: bounded, dropping
 *    the oldest entry when full). The environment never touches block
 *    state.
 *  - write is called for every propagation of a bound block output, the
 *    unconditional push when its process starts included, with the value
 *    converted to the actuator port's type (an f64 or a bool). It must return
 *    at once: the environment does the hardware access in its own context.
 *    Nothing is written while nothing drives a channel: the actuator keeps its
 *    last value (03 §4).
 *  - wall_ms is the UTC wall clock in ms since the Unix epoch (04 §1, 01
 *    §3.4); it may step. NULL: the platform's clock (pal_wall_ms) is used.
 *  - stop is called at shutdown, before the engine loop is destroyed; no
 *    sink push may happen once it has returned.
 */
#ifndef HAKKAS_HAKKAS_ENV_H
#define HAKKAS_HAKKAS_ENV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "graphs/linkable.h"

/* A value (01 §2.1): a string, an i64, an f64 or a bool. */
typedef Linkable HakkasValue;

/* Limits of a description (env_description_validate). */
#define HAKKAS_ENV_PORTS_MAX 256    /* in_ports + out_ports */
#define HAKKAS_ENV_COUNT_MAX 1024   /* channels of one addressable port */
#define HAKKAS_ENV_MODULES_MAX 64   /* modules */
#define HAKKAS_ENV_ID_MAX 64	    /* a port id, the name (bytes) */
#define HAKKAS_ENV_IO_SLOTS_MAX 31  /* a carrier's I/O slots, built-in ones included */
#define HAKKAS_ENV_COMM_SLOTS_MAX 4 /* a carrier's communication slots */
#define HAKKAS_ENV_TEXT_MAX 1024    /* any other string (bytes) */

/* A port's value type (04 §1): "number" or "bool". */
enum hakkas_env_type {
	HAKKAS_ENV_NUMBER = 0,
	HAKKAS_ENV_BOOL = 1,
};

/* One port of the description. Strings other than id may be NULL (none). */
typedef struct {
	const char *id; /* unique across in_ports and out_ports, 1..HAKKAS_ENV_ID_MAX bytes */
	enum hakkas_env_type type;
	const char *description;
	const char *unit; /* descriptive only (01 §2.1) */
	bool has_min, has_max;
	double min, max; /* descriptive only; min <= max when both are set */
	bool addressable;
	uint32_t count; /* addressable: 1..HAKKAS_ENV_COUNT_MAX channels; else 0 */
} HakkasEnvPort;

/* A module of a modular environment (04 §1): UI metadata, and the ids of the
 * ports it contributes. Every string may be NULL. */
typedef struct {
	bool has_slot;
	uint32_t slot; /* the I/O slot, from 0; with a carrier required */
	bool builtin;  /* in a built-in slot of the carrier: cannot be changed */
	const char *type; /* machine-readable module type, e.g. "colibri_aiv" */
	const char *name; /* human-readable, e.g. "Colibri AIV" */
	const char *vendor;
	const char *revision;
	const char *serial;
	const char *documentation; /* a link */
	const char *const *ports;  /* port ids of this description */
	size_t port_count;
} HakkasEnvModule;

/* The carrier board's power specification (04 §1), view only. */
typedef struct {
	bool has_voltage_min, has_voltage_max;
	double voltage_min, voltage_max; /* V */
	bool ac, dc;			 /* the supplies it takes */
	bool has_current;
	double current; /* A */
} HakkasEnvPower;

/* A communication module's address parameters (Ethernet, WiFi), view only:
 * as the environment gives them, not checked. Strings may be NULL. */
typedef struct {
	const char *mode;    /* "dhcp" or "static" */
	const char *address; /* IPv4 or IPv6; with DHCP the leased one */
	bool has_prefix;
	uint32_t prefix;
	const char *gateway;
	const char *const *dns;
	size_t dns_count;
	const char *mac;
} HakkasEnvIp;

/* An RS-485 module's parameters, view only. */
typedef struct {
	bool has_bitrate;
	uint32_t bitrate;
	const char *parity; /* "none", "even" or "odd" */
} HakkasEnvRs485;

/* A LoRaWAN module's parameters (OTAA), view only. The App Key is a secret:
 * only whether one is set (02 §2). */
typedef struct {
	const char *app_eui; /* 16 hex digits */
	const char *dev_eui;
	bool has_app_key_set;
	bool app_key_set;
} HakkasEnvLoRaWan;

/* A communication slot and its module (04 §1). The parameter groups present
 * say what kind of module it is (the type is the vendor's name): ip for
 * Ethernet, ip and ssid for WiFi, rs485, lorawan; NULL for none. No secret is
 * ever part of it. */
typedef struct {
	uint32_t slot;	  /* 1..HAKKAS_ENV_COMM_SLOTS_MAX */
	const char *type; /* e.g. "colibri_ethernet" */
	const char *name; /* human-readable, e.g. "Colibri Ethernet"; may be NULL */
	const HakkasEnvIp *ip;
	const char *ssid; /* WiFi */
	const HakkasEnvRs485 *rs485;
	const HakkasEnvLoRaWan *lorawan;
} HakkasEnvComm;

/* The carrier's MCU module (04 §1), view only. Strings may be NULL. */
typedef struct {
	const char *type; /* e.g. "colibri_mcu6" */
	const char *name; /* human-readable, e.g. "Colibri MCU6" */
	const char *vendor;
	const char *revision;
	const char *serial;
	const char *documentation; /* a link */
} HakkasEnvMcu;

/* The carrier board of a modular environment (04 §1). Its I/O slots are
 * numbered 0..builtin_count + slots - 1 (1..HAKKAS_ENV_IO_SLOTS_MAX of them);
 * builtin lists the built-in ones, each holding a module that cannot be
 * changed. Strings may be NULL. */
typedef struct {
	const char *type; /* e.g. "colibri_8" */
	const char *name; /* human-readable, e.g. "Colibri 8" */
	const char *vendor;
	const char *revision;
	const char *serial;
	const char *documentation; /* a link */
	uint32_t slots;		   /* the changeable I/O slots */
	const uint32_t *builtin;   /* the built-in slots' numbers */
	size_t builtin_count;
	const HakkasEnvPower *power; /* NULL: not given */
	const HakkasEnvComm *comm;   /* the communication slots in use */
	size_t comm_count;	     /* 0..HAKKAS_ENV_COMM_SLOTS_MAX */
	const HakkasEnvMcu *mcu;     /* NULL: not given */
} HakkasEnvCarrier;

/* The environment's description (04 §1). */
typedef struct {
	const char *name; /* what a bindings file's "environment" must match (04 §4) */
	const char *description;
	const char *icon;
	const HakkasEnvPort *in_ports; /* actuators: hakkas' outputs to the plant */
	size_t in_port_count;
	const HakkasEnvPort *out_ports; /* sensors: the plant's inputs to hakkas */
	size_t out_port_count;
	const HakkasEnvModule *modules;
	size_t module_count;
	const HakkasEnvCarrier *carrier; /* NULL: not a modular environment */
} HakkasEnvDescription;

/* The engine's side of sensor delivery, handed to start. Opaque. */
typedef struct HakkasEnvSink HakkasEnvSink;

/* Deliver a new value of sensor channel index (1..count; 0 for a port that
 * is not addressable) of out_ports[port]. value is an f64, an i64 or a bool
 * (converted to the port's type by the engine); it is copied. Any thread.
 * False, with nothing delivered, for a port or index the description does
 * not have, another value type, or once the sink is no longer valid. */
bool hakkas_env_sink_push(HakkasEnvSink *sink, int port, int index, const HakkasValue *value);

/* Optionally deliver a value the environment already has for a sensor
 * channel (as hakkas_env_sink_push) from within start, on the engine
 * thread: taken at once, not queued. Nothing requires or waits for it.
 * False outside start, and as hakkas_env_sink_push. */
bool hakkas_env_sink_current(HakkasEnvSink *sink, int port, int index,
			     const HakkasValue *value);

/* The environment (04 §0): its functions and their context. Every function
 * but wall_ms is required. */
typedef struct {
	const HakkasEnvDescription *(*describe)(void *ctx);
	int (*start)(void *ctx, HakkasEnvSink *sink);
	void (*stop)(void *ctx);
	/* in_ports[port], channel index (as for the sink); value is an f64 or
	 * a bool, valid during the call. */
	void (*write)(void *ctx, int port, int index, const HakkasValue *value);
	int64_t (*wall_ms)(void *ctx); /* NULL: the platform's clock */
	void *ctx;
} HakkasEnvironment;

#endif
