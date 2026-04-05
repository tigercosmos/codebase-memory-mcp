/*
 * profile.c — Activatable profiling implementation.
 */
#include "foundation/profile.h"
#include "foundation/log.h"
#include "foundation/compat.h"

#include <stdio.h>
#include <stdlib.h>

bool cbm_profile_active = false;

void cbm_profile_init(void) {
    const char *env = getenv("CBM_PROFILE");
    if (env && env[0] != '\0' && env[0] != '0') {
        cbm_profile_active = true;
    }
}

void cbm_profile_enable(void) {
    cbm_profile_active = true;
}

void cbm_profile_now(struct timespec *ts) {
    cbm_clock_gettime(CLOCK_MONOTONIC, ts);
}

void cbm_profile_log_elapsed(const char *phase, const char *sub,
                             const struct timespec *start, long items) {
    struct timespec now;
    cbm_clock_gettime(CLOCK_MONOTONIC, &now);

    long us = (long)(now.tv_sec - start->tv_sec) * 1000000L +
              ((long)now.tv_nsec - (long)start->tv_nsec) / 1000L;
    long ms = us / 1000;

    char ms_buf[32], us_buf[32], items_buf[32], rate_buf[32];
    snprintf(ms_buf, sizeof(ms_buf), "%ld", ms);
    snprintf(us_buf, sizeof(us_buf), "%ld", us);

    if (items > 0 && us > 0) {
        long rate = (long)((double)items * 1000000.0 / (double)us);
        snprintf(items_buf, sizeof(items_buf), "%ld", items);
        snprintf(rate_buf, sizeof(rate_buf), "%ld", rate);
        cbm_log_info("prof", "phase", phase, "sub", sub,
                     "ms", ms_buf, "us", us_buf,
                     "items", items_buf, "rate_per_s", rate_buf);
    } else if (items > 0) {
        snprintf(items_buf, sizeof(items_buf), "%ld", items);
        cbm_log_info("prof", "phase", phase, "sub", sub,
                     "ms", ms_buf, "us", us_buf, "items", items_buf);
    } else {
        cbm_log_info("prof", "phase", phase, "sub", sub,
                     "ms", ms_buf, "us", us_buf);
    }
}
