/*
 * process/PID (03 §6.1, plan task 23), driven with explicit engine time like
 * tests/test_timers.c: the tests play the engine loop (runtime_fire_timers,
 * then drain), set inputs as initials, and close the loop over a simulated
 * first-order plant themselves.
 */
#include "test.h"

#include <math.h>
#include <stdint.h>

#include "blocktypes/block_types.h"
#include "graphs/deadlines.h"
#include "graphs/graph.h"

static Runtime rt;
static Graph *gr;
static Block *pid;

/* ---- Helpers ---------------------------------------------------------------- */

static void set(const char *port, double v);
static void set_bool(const char *port, bool v);

/* A bare regulator: every input at its resting value (0, false; 03 §6.1). */
static void setup_bare(void)
{
	runtime_init(&rt);
	gr = graph_create(&rt);
	CHECK(gr != NULL);
	pid = NULL;
	CHECK(graph_create_block(gr, "P", block_types_get("process/PID"), NULL, &pid) ==
	      GRAPH_OK);
}

/* A configured regulator, as the tests below assume: kp 1, max_dgain 10, output
 * limits 0-100, automatic, sampling every second. */
static void setup(void)
{
	setup_bare();
	set("kp", 1);
	set("max_dgain", 10);
	set("max", 100);
	set_bool("auto", true);
	set("sample_ms", 1000);
}

static void teardown(void)
{
	graph_destroy(gr);
	gr = NULL;
	CHECK(deadline_heap_len(&rt.timers) == 0);
	runtime_deinit(&rt);
}

static void drain(void)
{
	runtime_drain(&rt, SIZE_MAX);
}

/* Set an input at engine time now (an initial: delivered at once). */
static void set_at(uint64_t now, const char *port, Linkable v)
{
	runtime_set_now(&rt, now);
	CHECK(graph_add_initial(gr, "P", port, &v) == GRAPH_OK);
	linkable_clear(&v);
	drain();
}

static void set(const char *port, double v)
{
	set_at(rt.now_ms, port, linkable_from_f64(v));
}

static void set_bool(const char *port, bool v)
{
	set_at(rt.now_ms, port, linkable_from_bool(v));
}

static void start_at(uint64_t now)
{
	runtime_set_now(&rt, now);
	graph_start(gr);
	drain();
}

/* One engine loop iteration at time now. Returns the expiries fired. */
static size_t fire(uint64_t now)
{
	size_t n = runtime_fire_timers(&rt, now);
	drain();
	return n;
}

static uint64_t next_due(void)
{
	uint64_t due;
	return runtime_next_deadline(&rt, &due) ? due : UINT64_MAX;
}

static double out(void)
{
	const Linkable *v = block_output_value(pid, 0);
	CHECK(v->type == LINKABLE_F64);
	return linkable_to_f64(v);
}

static bool saturated(void)
{
	const Linkable *v = block_output_value(pid, 1);
	CHECK(v->type == LINKABLE_BOOL);
	return linkable_to_bool(v);
}

static bool near(double a, double b)
{
	if (fabs(a - b) <= 1e-9 * fmax(1.0, fabs(b)))
		return true;
	fprintf(stderr, "  got %.17g, expected %.17g\n", a, b);
	return false;
}

/* ---- Proportional ----------------------------------------------------------- */

static void test_proportional(void)
{
	setup();
	set("setpoint", 50);
	set("input", 20);
	start_at(0);
	CHECK(near(out(), 30)); /* kp 1 */
	CHECK(!saturated());
	CHECK(next_due() == 1000);

	/* Step responses at once, on the input change. */
	set_at(100, "setpoint", linkable_from_f64(200));
	CHECK(near(out(), 100));
	CHECK(saturated());
	set("setpoint", 25);
	CHECK(near(out(), 5));
	CHECK(!saturated());
	set("kp", 2);
	CHECK(near(out(), 10));

	/* Without integral or derivative a tick changes nothing. */
	CHECK(fire(1000) == 1);
	CHECK(near(out(), 10));

	/* At the lower limit. */
	set("input", 40);
	CHECK(near(out(), 0));
	CHECK(saturated());
	teardown();
}

/* ---- Integral ---------------------------------------------------------------- */

static void test_integral_per_dt(void)
{
	setup();
	set("kp", 0);
	set("ki", 0.5);
	set("setpoint", 10);
	start_at(0);
	CHECK(near(out(), 0));
	CHECK(fire(1000) == 1);
	CHECK(near(out(), 5)); /* 0.5 · 10 · 1 s */
	CHECK(fire(2000) == 1);
	CHECK(near(out(), 10));

	/* An input change does not advance the integral. */
	set_at(2500, "kp", linkable_from_f64(1));
	CHECK(near(out(), 20));
	set("setpoint", 12);
	CHECK(near(out(), 22));
	CHECK(fire(3000) == 1);
	CHECK(near(out(), 12 + 16)); /* I = 10 + 0.5 · 12 · 1 s */

	/* A change of ki does not bump the output (the integral is stored as
	 * a term); ki = 0 disables it at once. */
	set("ki", 2);
	CHECK(near(out(), 28));
	set("ki", 0);
	CHECK(near(out(), 12));
	teardown();
}

/* Close the loop over y' = (u − y) / tau (gain 1, tau 5 s), sampled every
 * 100 ms for 60 s, exactly discretized; returns the final measurement. */
static double closed_loop(double kp, double ki)
{
	setup();
	const double tau = 5.0, dt = 0.1, sp = 40;
	double y = 0;
	set("kp", kp);
	set("ki", ki);
	set("sample_ms", 100);
	set("setpoint", sp);
	set("input", y);
	start_at(0);
	for (uint64_t k = 1; k <= 600; k++) {
		CHECK(fire(k * 100) == 1);
		double u = out();
		CHECK(u >= 0 && u <= 100);
		y += (u - y) * (1 - exp(-dt / tau));
		set("input", y);
	}
	teardown();
	return y;
}

static void test_closed_loop(void)
{
	/* P alone leaves a steady-state error: y = kp/(1+kp) · sp. */
	double y = closed_loop(2, 0);
	CHECK(fabs(y - 40.0 * 2 / 3) < 0.01);
	/* The integral removes it. */
	y = closed_loop(2, 0.5);
	CHECK(fabs(y - 40) < 0.01);
}

/* ---- Derivative -------------------------------------------------------------- */

/* kp 0, kd 2, every 10 ms, wide limits. A setpoint step gives no kick; a
 * measurement step of 1 gives d0 at the next tick, then d1. */
static void derivative_case(double max_dgain, double d0, double d1)
{
	setup();
	set("kp", 0);
	set("kd", 2);
	set("max_dgain", max_dgain);
	set("sample_ms", 10);
	set("min", -1000);
	set("max", 1000);
	start_at(0);
	CHECK(fire(10) == 1); /* the first tick records the measurement */
	CHECK(near(out(), 0));

	set_at(15, "setpoint", linkable_from_f64(50));
	CHECK(near(out(), 0));
	CHECK(fire(20) == 1);
	CHECK(near(out(), 0)); /* no derivative kick */

	set_at(25, "input", linkable_from_f64(1));
	CHECK(near(out(), 0)); /* the input change does not advance D */
	CHECK(fire(30) == 1);
	CHECK(near(out(), d0));

	/* An input change recomputes P with the stored D. */
	set_at(35, "kp", linkable_from_f64(1));
	CHECK(near(out(), 49 + d0));
	CHECK(fire(40) == 1);
	CHECK(near(out(), 49 + d1));

	/* kd 0 removes the derivative term at once. */
	set("kd", 0);
	CHECK(near(out(), 49));
	teardown();
}

static void test_derivative(void)
{
	/* Filtered: Tf = kd/max_dgain = 0.2 s, D = (Tf·D' + kd·Δy)/(Tf + dt),
	 * Δy = −1 (direct acting): at most max_dgain · |Δ| = 10, then decaying
	 * by Tf/(Tf + dt). */
	double d0 = -2 / 0.21;
	derivative_case(10, d0, 0.2 * d0 / 0.21);
	CHECK(fabs(d0) <= 10);
	/* Unfiltered: kd · Δy / dt, gone at the next tick. */
	derivative_case(0, -200, 0);
	derivative_case(-1, -200, 0);
	derivative_case(NAN, -200, 0);
}

/* ---- Acting direction -------------------------------------------------------- */

static void test_reverse(void)
{
	setup();
	set_bool("reverse", true);
	set("setpoint", 20);
	set("input", 25);
	start_at(0);
	CHECK(near(out(), 5)); /* e = input − setpoint */
	set_bool("reverse", false);
	CHECK(near(out(), 0));
	CHECK(saturated());
	teardown();

	/* The derivative's sign follows: a rising measurement raises D. */
	setup();
	set_bool("reverse", true);
	set("kp", 0);
	set("kd", 1);
	set("max_dgain", 0);
	set("min", -100);
	start_at(0);
	CHECK(fire(1000) == 1);
	set("input", 2);
	CHECK(fire(2000) == 1);
	CHECK(near(out(), 2));
	teardown();
}

/* ---- Limits and anti-windup ------------------------------------------------- */

static void test_anti_windup(void)
{
	setup();
	set("kp", 0);
	set("ki", 1);
	set("setpoint", 10);
	start_at(0);
	for (uint64_t k = 1; k <= 9; k++)
		CHECK(fire(k * 1000) == 1);
	CHECK(near(out(), 90));
	CHECK(!saturated());
	CHECK(fire(10000) == 1);
	CHECK(near(out(), 100));
	CHECK(saturated());
	/* 100 s at the limit: an unclamped integral would be at 1000. */
	for (uint64_t k = 11; k <= 100; k++)
		CHECK(fire(k * 1000) == 1);
	CHECK(near(out(), 100));
	CHECK(saturated());

	/* The error reverses: the output leaves the limit at the next tick
	 * (an unclamped integral would hold it there for another 90 s). */
	set_at(100500, "input", linkable_from_f64(20));
	CHECK(near(out(), 100));
	CHECK(fire(101000) == 1);
	CHECK(near(out(), 90));
	CHECK(!saturated());
	teardown();

	/* The lower limit alike. */
	setup();
	set("kp", 0);
	set("ki", 1);
	set("input", 10);
	start_at(0);
	for (uint64_t k = 1; k <= 50; k++)
		CHECK(fire(k * 1000) == 1);
	CHECK(near(out(), 0));
	CHECK(saturated());
	set("input", -10);
	CHECK(fire(51000) == 1);
	CHECK(near(out(), 10));
	teardown();

	/* A proportional term beyond the limit freezes the integral. */
	setup();
	set("ki", 1);
	set("setpoint", 1000);
	start_at(0);
	for (uint64_t k = 1; k <= 20; k++)
		CHECK(fire(k * 1000) == 1);
	CHECK(near(out(), 100));
	set("setpoint", 50);
	CHECK(near(out(), 50)); /* I is 0: P alone */
	teardown();
}

static void test_limits(void)
{
	setup();
	set("setpoint", 500);
	start_at(0);
	CHECK(near(out(), 100));
	/* A NaN limit is no limit on its side. */
	set("max", NAN);
	CHECK(near(out(), 500));
	CHECK(!saturated());
	set("setpoint", -500);
	CHECK(near(out(), 0));
	set("min", NAN);
	CHECK(near(out(), -500));
	CHECK(!saturated());
	/* Limits the wrong way round are swapped. */
	set("min", 100);
	set("max", 0);
	CHECK(near(out(), 0));
	CHECK(saturated());
	set("setpoint", 50);
	CHECK(near(out(), 50));
	CHECK(!saturated());
	set("setpoint", 150);
	CHECK(near(out(), 100));
	CHECK(saturated());
	/* Equal limits, as when neither is set, are no limits. */
	set("min", 0);
	set("max", 0);
	CHECK(near(out(), 150));
	CHECK(!saturated());
	set("min", 7);
	set("max", 7);
	CHECK(near(out(), 150));
	CHECK(!saturated());
	teardown();
}

/* ---- Resting values ----------------------------------------------------------- */

/* A regulator with no input set rests: manual, output 0, no sampling (03 §6.1). */
static void test_resting(void)
{
	setup_bare();
	set("setpoint", 50);
	start_at(0);
	CHECK(near(out(), 0));
	CHECK(!saturated());
	CHECK(next_due() == UINT64_MAX);
	/* Automatic with no gain: still 0; with a gain, unlimited. */
	set_bool("auto", true);
	CHECK(near(out(), 0));
	set("kp", 3);
	CHECK(near(out(), 150));
	CHECK(!saturated());
	/* Sampling starts when sample_ms is set. */
	set_at(10, "sample_ms", linkable_from_f64(500));
	CHECK(next_due() == 510);
	teardown();
}

/* ---- Manual / auto ---------------------------------------------------------- */

static void test_manual_bumpless(void)
{
	setup();
	set("ki", 1);
	set("setpoint", 50);
	set("input", 40);
	start_at(0);
	CHECK(near(out(), 10));
	CHECK(fire(1000) == 1);
	CHECK(near(out(), 20)); /* P 10 + I 10 */

	/* Manual: the output follows manual, clamped. */
	set_at(1500, "manual", linkable_from_f64(70));
	set_bool("auto", false);
	CHECK(near(out(), 70));
	CHECK(!saturated());
	set("manual", 150);
	CHECK(near(out(), 100));
	CHECK(saturated());
	set("manual", 70);
	CHECK(near(out(), 70));
	CHECK(fire(2000) == 1);
	CHECK(fire(3000) == 1);
	CHECK(near(out(), 70));
	set("setpoint", 55);
	CHECK(near(out(), 70));
	set("manual", NAN); /* held */
	CHECK(near(out(), 70));

	/* Back to auto without a jump: I is preset to 70 − P. */
	set_at(3500, "auto", linkable_from_bool(true));
	CHECK(near(out(), 70));
	CHECK(!saturated());
	CHECK(fire(4000) == 1);
	CHECK(near(out(), 70 + 15)); /* I += 15 · 1 s */
	teardown();
}

static void test_start_manual(void)
{
	setup();
	set("ki", 1);
	set("setpoint", 50);
	set("input", 40);
	set_bool("auto", false);
	set("manual", 30);
	start_at(0);
	CHECK(near(out(), 30));
	CHECK(fire(1000) == 1);
	CHECK(near(out(), 30));
	set_bool("auto", true);
	CHECK(near(out(), 30));
	CHECK(fire(2000) == 1);
	CHECK(near(out(), 40));
	teardown();

	/* Without an integral term there is nothing to preset: P alone. */
	setup();
	set("setpoint", 50);
	set("input", 40);
	set_bool("auto", false);
	set("manual", 30);
	start_at(0);
	CHECK(near(out(), 30));
	set_bool("auto", true);
	CHECK(near(out(), 10));
	teardown();
}

static void test_stop_restart(void)
{
	setup();
	set("kp", 0);
	set("ki", 1);
	set("setpoint", 10);
	start_at(0);
	CHECK(fire(1000) == 1);
	CHECK(near(out(), 10));
	runtime_set_now(&rt, 1500);
	graph_stop(gr);
	CHECK(next_due() == UINT64_MAX);
	start_at(5000); /* the integral is reset */
	CHECK(near(out(), 0));
	CHECK(next_due() == 6000);
	CHECK(fire(6000) == 1);
	CHECK(near(out(), 10)); /* dt from the restart */
	teardown();
}

/* ---- Timing ------------------------------------------------------------------ */

static void test_sample_ms(void)
{
	setup();
	set("kp", 0);
	set("ki", 1);
	set("setpoint", 1);
	start_at(0);
	CHECK(next_due() == 1000);

	/* A new period re-arms from now. */
	set_at(300, "sample_ms", linkable_from_f64(250));
	CHECK(next_due() == 550);
	CHECK(fire(550) == 1);
	CHECK(near(out(), 0.55)); /* dt from the start */
	CHECK(next_due() == 800);
	set("sample_ms", 250); /* unchanged: no re-arm */
	CHECK(next_due() == 800);

	/* <= 0, NaN, infinite: no sampling; inputs still recompute. */
	set_at(600, "sample_ms", linkable_from_f64(0));
	CHECK(next_due() == UINT64_MAX);
	set("sample_ms", NAN);
	set("sample_ms", -5);
	set("sample_ms", INFINITY);
	CHECK(next_due() == UINT64_MAX);
	set("kp", 1);
	CHECK(near(out(), 1.55));
	set("kp", 0);

	/* Sampling again: the idle time is not integrated. */
	set_at(5000, "sample_ms", linkable_from_f64(100));
	CHECK(next_due() == 5100);
	CHECK(fire(5100) == 1);
	CHECK(near(out(), 0.65));

	/* Whole milliseconds, at least 1. */
	set_at(5100, "sample_ms", linkable_from_f64(0.3));
	CHECK(next_due() == 5101);
	set("sample_ms", 99.6);
	CHECK(next_due() == 5200);

	/* Set while stopped: applied at the start. */
	runtime_set_now(&rt, 5150);
	graph_stop(gr);
	set("sample_ms", 200);
	CHECK(next_due() == UINT64_MAX);
	start_at(10000);
	CHECK(next_due() == 10200);
	teardown();
}

static void test_skipped_ticks(void)
{
	setup();
	set("kp", 0);
	set("ki", 1);
	set("setpoint", 1);
	set("sample_ms", 100);
	start_at(0);
	CHECK(fire(100) == 1);
	CHECK(near(out(), 0.1));
	/* Late: ticks 200, 300 and 400 are skipped; one step with dt 0.35 s. */
	CHECK(fire(450) == 1);
	CHECK(near(out(), 0.45));
	CHECK(next_due() == 500);
	CHECK(fire(500) == 1);
	CHECK(near(out(), 0.5));
	teardown();
}

/* ---- Non-finite inputs ------------------------------------------------------- */

static void test_nan_inputs(void)
{
	setup();
	set("ki", 1);
	set("kd", 1);
	set("setpoint", 50);
	set("input", 20);
	start_at(0);
	CHECK(near(out(), 30));
	CHECK(fire(1000) == 1);
	CHECK(near(out(), 60)); /* P 30 + I 30, D 0 (first tick) */

	/* A NaN measurement holds the output and the integral. */
	set_at(1500, "input", linkable_from_f64(NAN));
	CHECK(near(out(), 60));
	CHECK(fire(2000) == 1);
	CHECK(near(out(), 60));
	CHECK(!isnan(out()));

	/* Back: the same integral; the time while invalid is not integrated,
	 * and the derivative restarts without a kick. */
	set_at(2500, "input", linkable_from_f64(25));
	CHECK(near(out(), 55));
	CHECK(fire(3000) == 1);
	CHECK(near(out(), 25 + 30 + 25)); /* I += 25 · 1 s, D 0 */

	/* A NaN setpoint or gain likewise. */
	set("setpoint", NAN);
	CHECK(near(out(), 80));
	CHECK(fire(4000) == 1);
	CHECK(near(out(), 80));
	set("setpoint", 50);
	set("kp", NAN);
	CHECK(near(out(), 80));
	set("kp", INFINITY);
	CHECK(fire(5000) == 1);
	CHECK(near(out(), 80));
	set("kp", 1);
	CHECK(near(out(), 80)); /* P 25 + I 55 */
	teardown();
}

int main(void)
{
	CHECK(block_types_register_builtins() == BLOCK_TYPES_OK);
	RUN(test_proportional);
	RUN(test_integral_per_dt);
	RUN(test_closed_loop);
	RUN(test_derivative);
	RUN(test_reverse);
	RUN(test_anti_windup);
	RUN(test_limits);
	RUN(test_resting);
	RUN(test_manual_bumpless);
	RUN(test_start_manual);
	RUN(test_stop_restart);
	RUN(test_sample_ms);
	RUN(test_skipped_ticks);
	RUN(test_nan_inputs);
	block_types_reset();
	return test_result();
}
