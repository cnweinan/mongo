/*-
 * Public Domain 2014-2018 MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */
#include "test_util.h"

#include <signal.h>

#define	MAXKEY		10000
#define	PERIOD		60

static WT_CONNECTION *conn;
static uint64_t worker, worker_busy, verify, verify_busy;
static u_int workers, uris;
static bool done = false;
static bool verbose = false;
static char *uri_list[750];

static void
uri_init(void)
{
	WT_CURSOR *cursor;
	WT_SESSION *session;
	u_int i, key;
	char buf[128];

	for (i = 0; i < uris; ++i)
		if (uri_list[i] == NULL) {
			testutil_check(
			    __wt_snprintf(buf, sizeof(buf), "table:%u", i));
			uri_list[i] = dstrdup(buf);
		}

	testutil_check(conn->open_session(conn, NULL, NULL, &session));

	/* Initialize the file contents. */
	for (i = 0; i < uris; ++i) {
		testutil_check(__wt_snprintf(buf, sizeof(buf),
		    "key_format=S,value_format=S,"
		    "allocation_size=4K,leaf_page_max=32KB,"));
		testutil_check(session->create(session, uri_list[i], buf));
		testutil_check(session->open_cursor(
		    session, uri_list[i], NULL, NULL, &cursor));
		for (key = 1; key < MAXKEY; ++key) {
			testutil_check(__wt_snprintf(
			    buf, sizeof(buf), "key:%020u", key));
			cursor->set_key(cursor, buf);
			cursor->set_value(cursor, buf);
			testutil_check(cursor->insert(cursor));
		}
		testutil_check(cursor->close(cursor));
	}

	/* Create a checkpoint we can use for readonly handles. */
	testutil_check(session->checkpoint(session, NULL));

	testutil_check(session->close(session, NULL));
}

static void
uri_teardown(void)
{
	u_int i;

	for (i = 0; i < WT_ELEMENTS(uri_list); ++i)
		free(uri_list[i]);
}

static void
op(WT_SESSION *session, WT_RAND_STATE *rnd, WT_CURSOR **cpp)
{
	WT_CURSOR *cursor;
	WT_DECL_RET;
	u_int i, key;
	char buf[128];
	bool readonly;

	/* Close any open cursor in the slot we're about to reuse. */
	if (*cpp != NULL) {
		testutil_check((*cpp)->close(*cpp));
		*cpp = NULL;
	}

	cursor = NULL;
	readonly = __wt_random(rnd) % 2 == 0;

	/* Loop to open an object handle. */
	for (i = __wt_random(rnd) % uris; !done; __wt_yield()) {
		/* Use a checkpoint handle for 50% of reads. */
		ret = session->open_cursor(session, uri_list[i], NULL,
		    readonly && (i % 2 == 0) ?
		    "checkpoint=WiredTigerCheckpoint" : NULL, &cursor);
		if (ret != EBUSY) {
			testutil_check(ret);
			break;
		}
		(void)__wt_atomic_add64(&worker_busy, 1);
	}
	if (cursor == NULL)
		return;

	/* Operate on some number of key/value pairs. */
	for (key = 1;
	    !done && key < MAXKEY; key += __wt_random(rnd) % 37, __wt_yield()) {
		testutil_check(
		    __wt_snprintf(buf, sizeof(buf), "key:%020u", key));
		cursor->set_key(cursor, buf);
		if (readonly)
			testutil_check(cursor->search(cursor));
		else {
			cursor->set_value(cursor, buf);
			testutil_check(cursor->insert(cursor));
		}
	}

	/* Close the cursor half the time, otherwise cache it. */
	if (__wt_random(rnd) % 2 == 0)
		testutil_check(cursor->close(cursor));
	else
		*cpp = cursor;

	(void)__wt_atomic_add64(&worker, 1);
}

static void *
wthread(void *arg)
{
	WT_CURSOR *cursor_list[10];
	WT_RAND_STATE rnd;
	WT_SESSION *session;
	u_int next;

	(void)arg;

	memset(cursor_list, 0, sizeof(cursor_list));

	testutil_check(conn->open_session(conn, NULL, NULL, &session));
	__wt_random_init_seed((WT_SESSION_IMPL *)session, &rnd);

	for (next = 0; !done;) {
		if (++next == WT_ELEMENTS(cursor_list))
			next = 0;
		op(session, &rnd, &cursor_list[next]);
	}

	return (NULL);
}

static void *
vthread(void *arg)
{
	WT_CURSOR *cursor_list[10];
	WT_DECL_RET;
	WT_RAND_STATE rnd;
	WT_SESSION *session;
	u_int i, next;

	(void)arg;

	memset(cursor_list, 0, sizeof(cursor_list));

	testutil_check(conn->open_session(conn, NULL, NULL, &session));
	__wt_random_init_seed((WT_SESSION_IMPL *)session, &rnd);

	for (next = 0; !done;) {
		if (++next == WT_ELEMENTS(cursor_list))
			next = 0;
		op(session, &rnd, &cursor_list[next]);

		while (!done) {
			i = __wt_random(&rnd) % uris;
			ret = session->verify(session, uri_list[i], NULL);
			if (ret == EBUSY) {
				(void)__wt_atomic_add64(&verify_busy, 1);
				continue;
			}

			testutil_check(ret);
			(void)__wt_atomic_add64(&verify, 1);
			break;
		}
	}

	return (NULL);
}

static void
on_alarm(int signo)
{
	(void)signo;				/* Unused parameter */

	done = true;
}

static void
sweep_stats(void)
{
	static const int list[] = {
		WT_STAT_CONN_CURSOR_SWEEP_BUCKETS,
		WT_STAT_CONN_CURSOR_SWEEP_CLOSED,
		WT_STAT_CONN_CURSOR_SWEEP_EXAMINED,
		WT_STAT_CONN_CURSOR_SWEEP,
		WT_STAT_CONN_DH_SWEEP_REF,
		WT_STAT_CONN_DH_SWEEP_CLOSE,
		WT_STAT_CONN_DH_SWEEP_REMOVE,
		WT_STAT_CONN_DH_SWEEP_TOD,
		WT_STAT_CONN_DH_SWEEPS,
		WT_STAT_CONN_DH_SESSION_SWEEPS,
		-1
	};
	WT_SESSION *session;
	WT_CURSOR *cursor;
	uint64_t value;
	int i;
	const char *desc, *pvalue;

	testutil_check(conn->open_session(conn, NULL, NULL, &session));
	testutil_check(session->open_cursor(
	    session, "statistics:", NULL, NULL, &cursor));
	for (i = 0;; ++i) {
		if (list[i] == -1)
			break;
		cursor->set_key(cursor, list[i]);
		testutil_check(cursor->search(cursor));
		testutil_check(
		    cursor->get_value(cursor, &desc, &pvalue, &value));
		printf("\t" "%s=%s\n", desc, pvalue);
	}
}

static void
run(bool config_cache)
{
	pthread_t idlist[1000];
	u_int i, j;
	char buf[256], home[256];

	done = false;

	testutil_work_dir_from_path(
	    home, sizeof(home), "WT_TEST.wt4333_handle_locks");
	testutil_make_work_dir(home);

	testutil_check(__wt_snprintf(buf, sizeof(buf),
	    "create"
	    ", cache_cursors=%s"
	    ", cache_size=5GB"
	    ", checkpoint_sync=true"
	    ", eviction=(threads_max=5)"
	    ", file_manager=("
	    "close_handle_minimum=1,close_idle_time=1,close_scan_interval=1)"
	    ", mmap=true"
	    ", session_max=%u"
	    ", statistics=(all)",
	    config_cache ? "true" : "false",
	    workers + 100));
	testutil_check(wiredtiger_open(home, NULL, buf, &conn));

	printf("%s: %d seconds, cache_cursors=%s, %u workers, %u files\n",
	    progname, PERIOD, config_cache ? "true" : "false", workers,  uris);

	uri_init();

	/* 75% readers, 25% writers. */
	for (i = 0; i < workers; ++i)
		testutil_check(pthread_create(&idlist[i], NULL, wthread, NULL));
	testutil_check(pthread_create(&idlist[i], NULL, vthread, NULL));
	++i;

	(void)alarm(PERIOD);

	for (j = 0; j < i; ++j)
		testutil_check(pthread_join(idlist[j], NULL));

	printf(
	    "\t" "worker %" PRIu64
	    ", worker_busy %" PRIu64
	    ", verify %" PRIu64
	    ", verify_busy %" PRIu64
	    "\n",
	    worker, worker_busy, verify, verify_busy);

	if (verbose)
		sweep_stats();

	testutil_check(conn->close(conn, NULL));
}

int
main(int argc, char *argv[])
{
	static const struct {
		u_int workers;
		u_int uris;
	} runs[] = {
		{  1,   1},
		{  8,   1},
		{ 16,   1},
		{ 16,   WT_ELEMENTS(uri_list)},
		{200, 100},
		{300, 100},
		{200, WT_ELEMENTS(uri_list)},
		{600, WT_ELEMENTS(uri_list)},
	};
	u_int i, n;
	int ch;
	bool run_long;

	(void)testutil_set_progname(argv);

	run_long = false;
	while ((ch = __wt_getopt(argv[0], argc, argv, "av")) != EOF) {
		switch (ch) {
		case 'a':
			run_long = true;
			break;
		case 'v':
			verbose = true;
			break;
		default:
			fprintf(stderr, "usage: %s -a", argv[0]);
			return (EXIT_FAILURE);
		}
	}

	/* Ignore unless requested */
	if (!run_long &&
	    !testutil_is_flag_set("TESTUTIL_ENABLE_LONG_TESTS"))
		return (EXIT_SUCCESS);

	(void)signal(SIGALRM, on_alarm);

	/*
	 * This test takes 2 minutes per slot in the runs table, only do the
	 * first 2 and last 2 slots, unless specifically requested.
	 */
	n = WT_ELEMENTS(runs);
	for (i = 0; i < 2; ++i) {
		workers = runs[i].workers;
		uris = runs[i].uris;
		run(true);
		run(false);
	}
	if (!run_long)
		i = n - 2;
	for (; i < n; ++i) {
		workers = runs[i].workers;
		uris = runs[i].uris;
		run(true);
		run(false);
	}

	uri_teardown();

	return (EXIT_SUCCESS);
}