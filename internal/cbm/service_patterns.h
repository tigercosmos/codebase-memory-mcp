/*
 * service_patterns.h — Allowlists for HTTP clients, async dispatch, and config accessors.
 *
 * Used during call resolution to classify CALLS edges as:
 *   HTTP_CALLS  — synchronous HTTP client calls
 *   ASYNC_CALLS — async message/task dispatch
 *   CONFIGURES  — config/env access
 *
 * Lookup is O(1) via hash table initialized once at startup.
 */
#ifndef CBM_SERVICE_PATTERNS_H
#define CBM_SERVICE_PATTERNS_H

/* Edge type returned by pattern match. */
typedef enum {
    CBM_SVC_NONE = 0,   /* Not a service pattern — use normal CALLS */
    CBM_SVC_HTTP = 1,   /* Synchronous HTTP client call */
    CBM_SVC_ASYNC = 2,  /* Async dispatch (message broker, task queue) */
    CBM_SVC_CONFIG = 3, /* Config/env accessor */
} cbm_svc_kind_t;

/* Initialize the pattern lookup tables. Call once at startup. Thread-safe after init. */
void cbm_service_patterns_init(void);

/* Check if a resolved QN contains a known service library identifier.
 * Returns the pattern kind, or CBM_SVC_NONE if no match.
 * Matches on library name substrings in the QN (e.g., "requests" in
 * "project.venv.requests.api.get"). Import-alias transparent. */
cbm_svc_kind_t cbm_service_pattern_match(const char *resolved_qn);

/* Get the HTTP method from the callee name suffix (e.g., ".get" → "GET").
 * Returns NULL if method cannot be inferred. */
const char *cbm_service_pattern_http_method(const char *callee_name);

/* Get the broker name for an async QN (e.g., "pubsub" from a Pub/Sub QN).
 * Returns NULL if not an async pattern. */
const char *cbm_service_pattern_broker(const char *resolved_qn);

#endif /* CBM_SERVICE_PATTERNS_H */
