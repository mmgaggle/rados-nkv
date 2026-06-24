/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 IBM Corporation.
 *   All rights reserved.
 */

/*
 * Standalone round-trip test for the front Mercury client core
 * (nkvx_front_client.{h,c}). No SPDK reactor: it drives nkvx_front_progress()
 * in a loop exactly as the integrated SPDK poller will, and asserts the async
 * done-callback fires with the expected status decoded back from the executor.
 *
 * Slice C7 extends it to the large-result path: it allocates a host output
 * buffer (the stand-in for the tenant DPTR), hands it to nkvx_front_forward()
 * as the result sink, and — exactly as the real bridge does — copies any inline
 * result into that same buffer. After completion it can sha256 the delivered
 * bytes and compare against --expect-sha256, proving the executor PUSH landed
 * the correct bytes (or the inline copy did) end to end over a real transport.
 *
 * Run against a live executor (nkvx_service) — see nkvx_front_test.sh (C4
 * skeleton round-trip) and nkvx_c7_test.sh (large-result bulk RMA).
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <getopt.h>
#include <errno.h>

#include <openssl/sha.h>

#include "nkvx_front_client.h"

struct done_state {
	bool				called;
	int				calls;		/* C6a: exactly-once check */
	enum spdk_kvdev_io_status	status;
	uint32_t			result_len;
	uint32_t			result_inline_len;
	void				*sink;		/* host output buffer */
	uint32_t			sink_cap;
};

static void
on_done(void *arg, enum spdk_kvdev_io_status status, uint32_t result_len,
	const void *result_inline, uint32_t result_inline_len)
{
	struct done_state *st = arg;

	st->status = status;
	st->result_len = result_len;
	st->result_inline_len = result_inline_len;
	st->calls++;

	/*
	 * Mirror the real bridge (kvdev_rados_nkvx_front_done): an inline result is
	 * copied into the host output buffer; a bulk-pushed result is already there
	 * (result_inline NULL/0), so this is a no-op on the push path.
	 */
	if (result_inline != NULL && result_inline_len > 0 &&
	    st->sink != NULL && st->sink_cap > 0) {
		uint32_t n = result_inline_len < st->sink_cap ? result_inline_len : st->sink_cap;
		memcpy(st->sink, result_inline, n);
	}
	st->called = true;
}

static int
read_addr_file(const char *path, char *buf, size_t buf_sz)
{
	FILE *f = fopen(path, "r");
	if (f == NULL) {
		return -1;
	}
	char *p = fgets(buf, (int)buf_sz, f);
	fclose(f);
	if (p == NULL) {
		return -1;
	}
	buf[strcspn(buf, "\r\n")] = '\0';
	return (buf[0] != '\0') ? 0 : -1;
}

static int
parse_hex(const char *hex, unsigned char *out, size_t out_len)
{
	if (strlen(hex) != out_len * 2) {
		return -1;
	}
	for (size_t i = 0; i < out_len; i++) {
		unsigned byte;

		if (sscanf(hex + i * 2, "%2x", &byte) != 1) {
			return -1;
		}
		out[i] = (unsigned char)byte;
	}
	return 0;
}

int
main(int argc, char **argv)
{
	const char *na_init = "na+sm://";
	const char *target = NULL;
	const char *addr_file = NULL;
	int expect = SPDK_KVDEV_IO_STATUS_NOT_SUPPORTED;
	const char *key = "ping";
	int runtime = 1;			/* NKVX (built-in native or wasm; module-ns selects) */
	const char *module_key = "nkvx:bytecount";
	const char *module_ns = "kvpool";
	int osize = 4096;
	int expect_result = -1;			/* >=0: assert result_len == this */
	const char *sha256_hex = NULL;		/* module bound hash (64 hex) */
	const char *expect_sha_hex = NULL;	/* expected sha256 of delivered bytes */
	int iters = 1;				/* C7.2: repeat N forwards on one front+sink */
	int distinct = 1;			/* C7.2: round-robin N distinct sink buffers */
	int cancel = 0;				/* C6a: cancel each forward after submitting */
	int cancel_after = 0;			/* C6a: progress ticks before the cancel */
	int poison_after_cancel = 0;		/* C6b: after an ABORTED cancel, poison the sink and
						 * progress extra ticks; assert NO late PUSH overwrites it */
	int input_len = 0;			/* F1 (spdk-8od): large-input Exec — input_len bytes of
						 * registered input_bulk; with a small osize there is no
						 * result_sink, so result_sink==NULL but input_bulk!=NULL */
	int concurrent = 0;			/* F2 (spdk-8od): submit this many forwards CONCURRENTLY
						 * (one front, same op_id -> distinct front call_ids), keep
						 * all in flight, then cancel_all + drain */

	enum { OPT_KEY = 256, OPT_RUNTIME, OPT_MODULE, OPT_MODULE_NS, OPT_OSIZE,
	       OPT_EXPECT_RESULT, OPT_SHA256, OPT_EXPECT_SHA, OPT_ITERS, OPT_DISTINCT,
	       OPT_CANCEL, OPT_CANCEL_AFTER, OPT_POISON_AFTER_CANCEL,
	       OPT_INPUT_LEN, OPT_CONCURRENT };
	static const struct option opts[] = {
		{ "listen",        required_argument, NULL, 'l' },
		{ "target",        required_argument, NULL, 't' },
		{ "addr-file",     required_argument, NULL, 'a' },
		{ "expect",        required_argument, NULL, 'e' },
		{ "key",           required_argument, NULL, OPT_KEY },
		{ "runtime",       required_argument, NULL, OPT_RUNTIME },
		{ "module",        required_argument, NULL, OPT_MODULE },
		{ "module-ns",     required_argument, NULL, OPT_MODULE_NS },
		{ "osize",         required_argument, NULL, OPT_OSIZE },
		{ "expect-result", required_argument, NULL, OPT_EXPECT_RESULT },
		{ "sha256",        required_argument, NULL, OPT_SHA256 },
		{ "expect-sha256", required_argument, NULL, OPT_EXPECT_SHA },
		{ "iters",         required_argument, NULL, OPT_ITERS },
		{ "distinct",      required_argument, NULL, OPT_DISTINCT },
		{ "cancel",        no_argument,       NULL, OPT_CANCEL },
		{ "cancel-after",  required_argument, NULL, OPT_CANCEL_AFTER },
		{ "poison-after-cancel", required_argument, NULL, OPT_POISON_AFTER_CANCEL },
		{ "input-len",     required_argument, NULL, OPT_INPUT_LEN },
		{ "concurrent",    required_argument, NULL, OPT_CONCURRENT },
		{ NULL,            0,                 NULL, 0 },
	};
	int c;
	while ((c = getopt_long(argc, argv, "l:t:a:e:", opts, NULL)) != -1) {
		switch (c) {
		case 'l': na_init = optarg; break;
		case 't': target = optarg; break;
		case 'a': addr_file = optarg; break;
		case 'e': expect = atoi(optarg); break;
		case OPT_KEY: key = optarg; break;
		case OPT_RUNTIME: runtime = atoi(optarg); break;
		case OPT_MODULE: module_key = optarg; break;
		case OPT_MODULE_NS: module_ns = optarg; break;
		case OPT_OSIZE: osize = atoi(optarg); break;
		case OPT_EXPECT_RESULT: expect_result = atoi(optarg); break;
		case OPT_SHA256: sha256_hex = optarg; break;
		case OPT_EXPECT_SHA: expect_sha_hex = optarg; break;
		case OPT_ITERS: iters = atoi(optarg); break;
		case OPT_DISTINCT: distinct = atoi(optarg); break;
		case OPT_CANCEL: cancel = 1; break;
		case OPT_CANCEL_AFTER: cancel = 1; cancel_after = atoi(optarg); break;
		case OPT_POISON_AFTER_CANCEL: poison_after_cancel = atoi(optarg); break;
		case OPT_INPUT_LEN: input_len = atoi(optarg); break;
		case OPT_CONCURRENT: concurrent = atoi(optarg); break;
		default:
			fprintf(stderr, "usage: %s --listen NA (--target ADDR | --addr-file PATH) "
				"[--key K] [--runtime N] [--module M] [--module-ns NS] [--osize N] "
				"[--sha256 HEX] [--expect N] [--expect-result N] [--expect-sha256 HEX] "
				"[--iters N] [--cancel] [--cancel-after N] [--poison-after-cancel N] "
				"[--input-len N] [--concurrent N]\n",
				argv[0]);
			return 2;
		}
	}
	if (iters < 1) {
		fprintf(stderr, "test: --iters must be >= 1\n");
		return 2;
	}
	if (distinct < 1) {
		fprintf(stderr, "test: --distinct must be >= 1\n");
		return 2;
	}

	if (osize <= 0) {
		fprintf(stderr, "test: --osize must be > 0\n");
		return 2;
	}
	size_t key_len = strlen(key);
	if (key_len == 0 || key_len > SPDK_KVDEV_EXEC_KEY_MAX_LEN) {
		fprintf(stderr, "test: --key must be 1..%d bytes\n", SPDK_KVDEV_EXEC_KEY_MAX_LEN);
		return 2;
	}
	unsigned char sha256[SPDK_KV_EXEC_SHA256_LEN];
	bool sha256_valid = false;
	if (sha256_hex != NULL) {
		if (parse_hex(sha256_hex, sha256, sizeof(sha256)) != 0) {
			fprintf(stderr, "test: --sha256 must be %d hex chars\n",
				SPDK_KV_EXEC_SHA256_LEN * 2);
			return 2;
		}
		sha256_valid = true;
	}
	unsigned char expect_sha[SHA256_DIGEST_LENGTH];
	bool have_expect_sha = false;
	if (expect_sha_hex != NULL) {
		if (parse_hex(expect_sha_hex, expect_sha, sizeof(expect_sha)) != 0) {
			fprintf(stderr, "test: --expect-sha256 must be %d hex chars\n",
				SHA256_DIGEST_LENGTH * 2);
			return 2;
		}
		have_expect_sha = true;
	}

	char addr_buf[512];
	if (target == NULL && addr_file != NULL) {
		if (read_addr_file(addr_file, addr_buf, sizeof(addr_buf)) != 0) {
			fprintf(stderr, "test: cannot read addr-file %s: %s\n",
				addr_file, strerror(errno));
			return 1;
		}
		target = addr_buf;
	}
	if (target == NULL) {
		fprintf(stderr, "test: need --target or --addr-file\n");
		return 2;
	}

	/* The host output buffer(s) == the tenant DPTR stand-in / result sink. A
	 * poison fill makes a short or absent push detectable. --distinct allocates N
	 * separate buffers (distinct cache keys) round-robined across the iters, to
	 * exercise the handle cache's LRU eviction when N exceeds its slot count. */
	unsigned char **sinks = calloc((size_t)distinct, sizeof(*sinks));
	if (sinks == NULL) {
		fprintf(stderr, "test: out of memory for sink table\n");
		return 1;
	}
	for (int b = 0; b < distinct; b++) {
		sinks[b] = malloc((size_t)osize);
		if (sinks[b] == NULL) {
			fprintf(stderr, "test: out of memory for %d-byte sink %d\n", osize, b);
			for (int j = 0; j < b; j++) {
				free(sinks[j]);
			}
			free(sinks);
			return 1;
		}
	}

	struct nkvx_front *front = NULL;
	int rc = nkvx_front_init(na_init, target, &front);
	if (rc != 0) {
		fprintf(stderr, "test: nkvx_front_init(%s, %s) failed: %d\n",
			na_init, target, rc);
		for (int b = 0; b < distinct; b++) {
			free(sinks[b]);
		}
		free(sinks);
		return 1;
	}

	nkvx_exec_in_t in;
	memset(&in, 0, sizeof(in));
	in.op_id = 0x4242;
	in.read_only = 1;
	in.runtime = (uint8_t)runtime;
	in.key_len = (uint8_t)key_len;
	memcpy(in.key, key, key_len);
	if (sha256_valid) {
		memcpy(in.sha256, sha256, sizeof(sha256));
		in.sha256_valid = 1;
	}
	in.module_key = (char *)module_key;
	in.module_ns = (char *)module_ns;
	in.osize = (uint32_t)osize;
	in.input_len = 0;
	in.input_inline = NULL;
	in.input_bulk = HG_BULK_NULL;
	in.result_sink = HG_BULK_NULL;

	/*
	 * F1 (bead spdk-8od): a LARGE-INPUT Exec. When --input-len > NKVX_INLINE_MAX the
	 * front registers in.input_inline as a READ-mode input_bulk and the executor PULLs
	 * it; with a SMALL --osize (<= NKVX_INLINE_MAX) the result rides inline and NO
	 * result_sink is registered. That is the exact F1 shape: result_sink == NULL but
	 * input_bulk != NULL. The buffer must outlive the RPC (the executor PULLs it
	 * asynchronously), so it lives here in main's frame until after the drain. The
	 * backend never reads input bytes (executor.h), so the contents are immaterial;
	 * we still fill them so a torn PULL would be observable in principle.
	 */
	unsigned char *inbuf = NULL;
	if (input_len > 0) {
		inbuf = malloc((size_t)input_len);
		if (inbuf == NULL) {
			fprintf(stderr, "test: out of memory for %d-byte input\n", input_len);
			for (int b = 0; b < distinct; b++) {
				free(sinks[b]);
			}
			free(sinks);
			return 1;
		}
		memset(inbuf, 0x3C, (size_t)input_len);
		in.input_len = (uint32_t)input_len;
		in.input_inline = inbuf;
	}

	int ret = 1;

	/*
	 * ====================================================================
	 * F2 (bead spdk-8od): same-op_id concurrent-cancel UAF guard. Submit `concurrent`
	 * Execs on ONE front WITHOUT draining between them, so they are all in flight at
	 * once. They share the SAME op_id (in.op_id == 0x4242), but the front assigns each
	 * a DISTINCT front-unique client_call_id, which is what the executor keys its
	 * cancel registry on. We keep them in flight via the executor PUSH-stall hook (the
	 * caller sets NKVX_TEST_PUSH_STALL_TICKS and a large --osize so each registers a
	 * result_sink and stalls pre-PUSH), then nkvx_front_cancel_all() and drain.
	 *
	 * REGRESSION SIGNAL: if the executor registry aliased the two same-op_id reqs (the
	 * F2 bug — keyed on op_id, not client_call_id), the second cancel would match the
	 * first req and the executor would ack the OTHER cancel ALREADY_DONE-as-duplicate,
	 * so one of the two Execs would NOT get its own do-not-PUSH / ABORTED handshake.
	 * With the fix each cancel finds its own req by client_call_id. The driver asserts
	 * BOTH done-cbs fired EXACTLY once with ABORTED and outstanding drained to 0; the
	 * shell harness additionally asserts the executor logged TWO DISTINCT cancelled
	 * call_ids (no cross-abort / early ALREADY_DONE between the pair).
	 * ====================================================================
	 */
	if (concurrent > 0) {
		struct done_state *sts = calloc((size_t)concurrent, sizeof(*sts));
		unsigned char **csinks = calloc((size_t)concurrent, sizeof(*csinks));
		int k;

		if (sts == NULL || csinks == NULL) {
			fprintf(stderr, "test: out of memory for %d concurrent states\n", concurrent);
			free(sts);
			free(csinks);
			goto done;
		}
		/* Each concurrent Exec gets its OWN sink DPTR (distinct buffers) so a per-call
		 * cross-abort or late PUSH would be attributable; poison each up front. */
		for (k = 0; k < concurrent; k++) {
			csinks[k] = malloc((size_t)osize);
			if (csinks[k] == NULL) {
				fprintf(stderr, "test: out of memory for concurrent sink %d\n", k);
				for (int j = 0; j < k; j++) {
					free(csinks[j]);
				}
				free(csinks);
				free(sts);
				goto done;
			}
			memset(csinks[k], 0xA5, (size_t)osize);
			sts[k].sink = csinks[k];
			sts[k].sink_cap = (uint32_t)osize;
		}

		/* Submit ALL forwards FIRST (no drain between) so they are concurrently in
		 * flight — same op_id, distinct front call_ids. */
		for (k = 0; k < concurrent; k++) {
			rc = nkvx_front_forward(front, &in, csinks[k], (uint32_t)osize,
						on_done, &sts[k]);
			if (rc != 0) {
				fprintf(stderr, "test: concurrent forward %d failed: %d\n", k, rc);
				goto f2_cleanup;
			}
		}
		if (nkvx_front_outstanding(front) != (unsigned)concurrent) {
			fprintf(stderr, "test: FAIL F2 (expected %d in flight, got %u)\n",
				concurrent, nkvx_front_outstanding(front));
			goto f2_cleanup;
		}
		printf("test: F2 submitted %d concurrent Execs (same op_id=0x%04x, distinct "
		       "front call_ids), all in flight\n", concurrent, in.op_id);

		/* A few ticks so each req reaches the executor and registers (PUSH stalled). */
		for (int i = 0; i < cancel_after; i++) {
			(void)nkvx_front_progress(front, 1);
		}

		/* Cancel ALL in-flight Execs (the channel-destroy primitive). Each must run
		 * its OWN two-phase handshake against its OWN executor req. */
		nkvx_front_cancel_all(front);

		/* Drive progress until both halves of every join resolve (outstanding -> 0). */
		for (int i = 0; i < 200000 && nkvx_front_outstanding(front) != 0; i++) {
			if (nkvx_front_progress(front, 100) < 0) {
				fprintf(stderr, "test: F2 progress failed\n");
				goto f2_cleanup;
			}
		}
		if (nkvx_front_outstanding(front) != 0) {
			fprintf(stderr, "test: FAIL F2 (%u Exec(s) still in flight after "
				"cancel_all + drain — a same-op_id cancel did not resolve)\n",
				nkvx_front_outstanding(front));
			goto f2_cleanup;
		}

		/* Each Exec's tenant cb must have fired EXACTLY once, with ABORTED. If the
		 * registry aliased the pair, one cb would be missing or mis-statused. */
		for (k = 0; k < concurrent; k++) {
			if (!sts[k].called || sts[k].calls != 1) {
				fprintf(stderr, "test: FAIL F2 (Exec %d done-cb fired %d time(s), "
					"expected exactly 1 — same-op_id cancels aliased?)\n",
					k, sts[k].calls);
				goto f2_cleanup;
			}
			if ((int)sts[k].status != SPDK_KVDEV_IO_STATUS_ABORTED) {
				fprintf(stderr, "test: FAIL F2 (Exec %d status %d != ABORTED(%d) — "
					"a same-op_id cancel hit the wrong req)\n",
					k, (int)sts[k].status, SPDK_KVDEV_IO_STATUS_ABORTED);
				goto f2_cleanup;
			}
		}
		printf("test: F2 PASS (%d concurrent same-op_id Execs each cancelled "
		       "independently -> exactly-once ABORTED, drained)\n", concurrent);
		ret = 0;

f2_cleanup:
		for (k = 0; k < concurrent; k++) {
			free(csinks[k]);
		}
		free(csinks);
		free(sts);
		goto done;
	}

	/*
	 * Run `iters` Execs SEQUENTIALLY on the SAME front, round-robining `distinct`
	 * sink buffers: submit, drain to completion, verify, repeat. Each sink is
	 * re-poisoned before its Exec so a missing push is detectable. With
	 * distinct==1 (the recurring-DPTR case the cache targets) the first large
	 * result MISSes and the rest HIT; with distinct > cache slots the LRU evicts.
	 */
	for (int it = 0; it < iters; it++) {
		unsigned char *sink = sinks[it % distinct];
		struct done_state st;
		memset(&st, 0, sizeof(st));
		st.sink = sink;
		st.sink_cap = (uint32_t)osize;
		memset(sink, 0xA5, (size_t)osize);

		rc = nkvx_front_forward(front, &in, sink, (uint32_t)osize, on_done, &st);
		if (rc != 0) {
			fprintf(stderr, "test: iter %d nkvx_front_forward failed: %d\n", it, rc);
			goto done;
		}

		/*
		 * Slice C6a abort path (channel-destroy teardown drain): cancel the
		 * in-flight Exec via nkvx_front_cancel_all() — exactly the production
		 * teardown primitive (the per-token cancel surface was DE-SHIPPED: ABA-
		 * unsafe and no safe caller; per-command abort needs bead spdk-5ia). There
		 * is exactly ONE forward in flight here, so cancel_all aborts it. Optionally
		 * after a few progress ticks (--cancel-after). Cancellation resolves through
		 * the normal completion (cb fires exactly once); the result is ABORTED if
		 * the cancel won the race, or the real status if the Exec finished first.
		 * Either is acceptable — the load-bearing checks are exactly-once, no leak
		 * (valgrind), bulk released, drained (outstanding==0), and a reusable DPTR.
		 */
		if (cancel) {
			for (int i = 0; i < cancel_after && !st.called; i++) {
				(void)nkvx_front_progress(front, 0);
			}
			nkvx_front_cancel_all(front);
		}

		/* Drive progress exactly as the SPDK poller will, until the cb fires. */
		for (int i = 0; i < 200000 && !st.called; i++) {
			int prog = nkvx_front_progress(front, 100);
			if (prog < 0) {
				fprintf(stderr, "test: iter %d nkvx_front_progress failed: %d\n",
					it, prog);
				goto done;
			}
		}

		if (!st.called) {
			fprintf(stderr, "test: FAIL iter %d (completion never fired — "
				"cancel must resolve through the normal completion path)\n", it);
			goto done;
		}
		/*
		 * C6a: the done-cb must have fired EXACTLY once and the in-flight slot must
		 * be drained (the call ctx + bulk refs released — the UAF-safe teardown).
		 */
		if (st.calls != 1) {
			fprintf(stderr, "test: FAIL iter %d (done-cb fired %d times, expected 1)\n",
				it, st.calls);
			goto done;
		}
		if (nkvx_front_outstanding(front) != 0) {
			fprintf(stderr, "test: FAIL iter %d (%u forward(s) still in flight after "
				"completion)\n", it, nkvx_front_outstanding(front));
			goto done;
		}
		if (cancel) {
			/* The cancel either won the race (ABORTED) or lost it (the Exec
			 * completed first with its real status == expect). Both acceptable. */
			if ((int)st.status != SPDK_KVDEV_IO_STATUS_ABORTED &&
			    (int)st.status != expect) {
				fprintf(stderr, "test: FAIL iter %d (cancel: status %d not ABORTED(%d) "
					"nor expected %d)\n", it, (int)st.status,
					SPDK_KVDEV_IO_STATUS_ABORTED, expect);
				goto done;
			}
			printf("test: cancel iter %d -> status=%d (%s)\n", it, (int)st.status,
			       (int)st.status == SPDK_KVDEV_IO_STATUS_ABORTED ?
			       "ABORTED — cancel won" : "completed — cancel lost race");

			/*
			 * Slice C6b LOAD-BEARING "no PUSH after ack" assertion. The done-cb
			 * fired only after the two-phase cancel JOINED — i.e. after the executor
			 * ACKED the cancel (do-not-PUSH set / in-flight PUSH HG_Bulk_cancel'd,
			 * remote bulk view gone). So the DPTR is now ours again and NOTHING may
			 * land in it. Poison it with a fresh sentinel, progress extra bounded
			 * ticks (any stray late PUSH would fire here), and assert the sentinel is
			 * untouched. With the C6a origin-only cancel this could be violated by a
			 * late cross-process PUSH; with the handshake it must hold. (Only mean-
			 * ingful when the cancel actually WON — a lost race delivered real bytes.) */
			if (poison_after_cancel > 0 &&
			    (int)st.status == SPDK_KVDEV_IO_STATUS_ABORTED) {
				const unsigned char POISON = 0x5C;
				memset(sink, POISON, (size_t)osize);
				for (int i = 0; i < poison_after_cancel; i++) {
					(void)nkvx_front_progress(front, 1);
				}
				for (int o = 0; o < osize; o++) {
					if (sink[o] != POISON) {
						fprintf(stderr, "test: FAIL iter %d (LATE PUSH after cancel "
							"ack — byte %d = 0x%02x != poison 0x%02x; the "
							"do-not-PUSH handshake did not hold)\n",
							it, o, sink[o], POISON);
						goto done;
					}
				}
				printf("test: iter %d no-late-PUSH verified (sink poison intact "
				       "%d ticks post-ack)\n", it, poison_after_cancel);
			}
			continue;	/* skip result_len / sha checks on the abort path */
		}
		if ((int)st.status != expect) {
			fprintf(stderr, "test: FAIL iter %d (status %d != expected %d)\n",
				it, (int)st.status, expect);
			goto done;
		}
		if (expect_result >= 0 && st.result_len != (uint32_t)expect_result) {
			fprintf(stderr, "test: FAIL iter %d (result_len %u != expected %d)\n",
				it, st.result_len, expect_result);
			goto done;
		}
		if (have_expect_sha) {
			uint32_t n = st.result_len < (uint32_t)osize ?
				st.result_len : (uint32_t)osize;
			unsigned char got[SHA256_DIGEST_LENGTH];

			SHA256(sink, n, got);
			if (memcmp(got, expect_sha, sizeof(got)) != 0) {
				fprintf(stderr, "test: FAIL iter %d (delivered %u bytes sha256 "
					"mismatch — torn/short bulk push?)\n", it, n);
				goto done;
			}
		}
	}

	/*
	 * Slice C6b DPTR-REUSE-AFTER-CANCEL: after cancelling Execs on a sink, run ONE
	 * NON-cancel Exec on that SAME sink and sha-check the delivered bytes. Proves
	 * the just-cancelled DPTR is fully reusable and the executor PUSHes correct
	 * bytes into it (no residual MR confusion from the cancel handshake). Only when
	 * a content hash is known (--expect-sha256) and a real result is expected.
	 */
	if (cancel && have_expect_sha && expect == SPDK_KVDEV_IO_STATUS_SUCCESS) {
		unsigned char *sink = sinks[0];
		struct done_state st;
		memset(&st, 0, sizeof(st));
		st.sink = sink;
		st.sink_cap = (uint32_t)osize;
		memset(sink, 0xA5, (size_t)osize);

		rc = nkvx_front_forward(front, &in, sink, (uint32_t)osize, on_done, &st);
		if (rc != 0) {
			fprintf(stderr, "test: reuse-after-cancel forward failed: %d\n", rc);
			goto done;
		}
		for (int i = 0; i < 200000 && !st.called; i++) {
			if (nkvx_front_progress(front, 100) < 0) {
				fprintf(stderr, "test: reuse-after-cancel progress failed\n");
				goto done;
			}
		}
		if (!st.called || st.calls != 1 || nkvx_front_outstanding(front) != 0 ||
		    (int)st.status != SPDK_KVDEV_IO_STATUS_SUCCESS) {
			fprintf(stderr, "test: FAIL reuse-after-cancel (called=%d calls=%d "
				"outstanding=%u status=%d)\n", st.called, st.calls,
				nkvx_front_outstanding(front), (int)st.status);
			goto done;
		}
		{
			uint32_t n = st.result_len < (uint32_t)osize ?
				st.result_len : (uint32_t)osize;
			unsigned char got[SHA256_DIGEST_LENGTH];

			SHA256(sink, n, got);
			if (memcmp(got, expect_sha, sizeof(got)) != 0) {
				fprintf(stderr, "test: FAIL reuse-after-cancel (delivered %u bytes "
					"sha256 mismatch — DPTR not cleanly reusable?)\n", n);
				goto done;
			}
		}
		printf("test: DPTR reuse-after-cancel verified (correct bytes into the "
		       "just-cancelled sink)\n");
	}

	/*
	 * C7.2 acceptance: with a large result (osize > NKVX_INLINE_MAX) reusing ONE
	 * sink buffer, the cache must register exactly once and reuse thereafter —
	 * misses == 1, hits == iters - 1, no eviction (single recurring DPTR). With
	 * --distinct > 1 the pattern is workload-dependent (eviction in play), so the
	 * strict assert applies only to the single-buffer case; stats are printed
	 * always for the caller to inspect.
	 */
	struct nkvx_front_bulk_stats bs;
	nkvx_front_get_bulk_stats(front, &bs);
	printf("test: bulk cache hits=%llu misses=%llu evicts=%llu\n",
	       (unsigned long long)bs.hits, (unsigned long long)bs.misses,
	       (unsigned long long)bs.evicts);
	if (!cancel && iters > 1 && distinct == 1 && osize > (int)NKVX_INLINE_MAX) {
		if (bs.misses != 1 || bs.hits != (uint64_t)(iters - 1) || bs.evicts != 0) {
			fprintf(stderr, "test: FAIL (cache reuse: expected misses=1 hits=%d "
				"evicts=0 for one recurring DPTR)\n", iters - 1);
			goto done;
		}
		printf("test: cache reuse verified (1 register, %d reuse)\n", iters - 1);
	}
	printf("test: PASS (%d iter(s), %d distinct sink(s), status=%d)\n",
	       iters, distinct, expect);
	ret = 0;

done:
	nkvx_front_fini(front);
	for (int b = 0; b < distinct; b++) {
		free(sinks[b]);
	}
	free(sinks);
	free(inbuf);
	return ret;
}
