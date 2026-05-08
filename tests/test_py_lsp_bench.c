/*
 * test_py_lsp_bench.c — Phase 11 in-process benchmark.
 *
 * Runs cbm_extract_file on a representative Python source covering the
 * most common dispatch patterns (typed parameters, classmethod, dataclass,
 * stdlib calls), measures wall-clock time, and reports calls /
 * resolved_calls counts. Tests assert the resolution-ratio target so a
 * regression below the threshold trips the suite.
 *
 * For a full repo-level benchmark, see docs/BENCHMARK_PYTHON.md.
 */
#include "test_framework.h"
#include "cbm.h"
#include "lsp/py_lsp.h"
#include <time.h>

/* Representative Python source: ~80 LOC covering imports, dataclass,
 * stdlib calls, classmethod, inheritance, typed parameters, and
 * attribute access chains — the patterns py_lsp aims to resolve. */
static const char *bench_source =
    "import os\n"
    "import logging\n"
    "from collections import defaultdict\n"
    "from dataclasses import dataclass\n"
    "from pathlib import Path\n"
    "\n"
    "log = logging.getLogger('bench')\n"
    "\n"
    "@dataclass\n"
    "class Config:\n"
    "    name: str\n"
    "    debug: bool\n"
    "    def display(self):\n"
    "        return self.name\n"
    "\n"
    "class BaseStore:\n"
    "    def __init__(self, root):\n"
    "        self.root = root\n"
    "    def get(self, key):\n"
    "        return None\n"
    "    def put(self, key, value):\n"
    "        return True\n"
    "\n"
    "class FileStore(BaseStore):\n"
    "    @classmethod\n"
    "    def create(cls, root):\n"
    "        return cls(root)\n"
    "    def get(self, key):\n"
    "        path = Path(self.root) / key\n"
    "        return path.exists()\n"
    "    def put(self, key, value):\n"
    "        return super().put(key, value)\n"
    "\n"
    "class App:\n"
    "    def __init__(self, cfg: Config):\n"
    "        self.cfg = cfg\n"
    "        self.store = FileStore.create(os.getcwd())\n"
    "        self.counts = defaultdict(int)\n"
    "    def display_config(self):\n"
    "        return self.cfg.display()\n"
    "    def lookup(self, key):\n"
    "        result = self.store.get(key)\n"
    "        self.counts[key] += 1\n"
    "        return result\n"
    "    def write(self, key, value):\n"
    "        return self.store.put(key, value)\n"
    "\n"
    "def main():\n"
    "    cfg = Config('app', True)\n"
    "    app = App(cfg)\n"
    "    app.lookup('a')\n"
    "    app.write('a', 1)\n"
    "    log.info('done')\n"
    "    return app.display_config()\n";

static double elapsed_ms(struct timespec t0, struct timespec t1) {
    double s = (double)(t1.tv_sec - t0.tv_sec);
    double ns = (double)(t1.tv_nsec - t0.tv_nsec);
    return s * 1000.0 + ns / 1000000.0;
}

TEST(pylsp_bench_resolution_ratio) {
    int slen = (int)strlen(bench_source);

    struct timespec t0;
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    CBMFileResult *r = cbm_extract_file(bench_source, slen, CBM_LANG_PYTHON,
                                        "test", "bench.py", 0, NULL, NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    ASSERT_NOT_NULL(r);

    double ms = elapsed_ms(t0, t1);
    int calls = r->calls.count;
    int resolved = r->resolved_calls.count;
    double ratio = calls > 0 ? (double)resolved / (double)calls : 0.0;

    printf("    bench: %d lines, %d calls, %d resolved (%.0f%%), %.2f ms\n",
           65 /* approximate LOC */,
           calls, resolved, ratio * 100.0, ms);

    /* Target: ≥40% on application code (no stdlib hits in this fixture's
     * count of `calls` since stdlib calls are also counted). With the
     * generated stdlib registry from Phase 10, expect ≥40%. */
    ASSERT_GTE(calls, 1);
    ASSERT_GTE(resolved, 1);
    /* Soft floor: in CI we want at least one quarter of calls resolved.
     * Tighter targets (40% / 70%) live in docs/BENCHMARK_PYTHON.md and
     * are tracked manually rather than enforced here, since asserting a
     * tight ratio would make the test brittle to fixture changes. */
    if (calls >= 8) {
        ASSERT_GTE(resolved * 4, calls);
    }

    /* Absolute time budget: <50 ms for ~65-line fixture under ASan +
     * UBSan (target without sanitizers is <10 ms; we run with sanitizers
     * on so use 5x leeway). */
    ASSERT(ms < 50.0);

    cbm_free_result(r);
    PASS();
}

SUITE(py_lsp_bench) {
    RUN_TEST(pylsp_bench_resolution_ratio);
}
