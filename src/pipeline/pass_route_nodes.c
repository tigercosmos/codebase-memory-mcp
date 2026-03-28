/*
 * pass_route_nodes.c — Create Route nodes for HTTP_CALLS/ASYNC_CALLS edges.
 *
 * After parallel resolve merges edges into the main gbuf, HTTP_CALLS and
 * ASYNC_CALLS edges have url_path/method/broker in properties but point to
 * the library function (e.g., requests.get). This pass:
 *
 *   1. Scans all HTTP_CALLS/ASYNC_CALLS edges
 *   2. Extracts url_path from edge properties
 *   3. Creates Route nodes with deterministic QNs (__route__METHOD__/path)
 *   4. Re-targets edges from library function → Route node
 *
 * Route nodes are the rendezvous point for cross-service communication:
 *   Service A: checkout() → HTTP_CALLS → Route("POST /api/orders")
 *   Service B: create_order() → HANDLES → Route("POST /api/orders")
 */
#include "pipeline/pipeline_internal.h"
#include "graph_buffer/graph_buffer.h"
#include "foundation/log.h"

#include <stdio.h>
#include <string.h>

/* Extract a JSON string value by key from properties.
 * Returns pointer into buf (caller provides buffer). NULL if not found. */
static const char *json_extract(const char *json, const char *key, char *buf, int bufsz) {
    if (!json || !key) {
        return NULL;
    }
    /* Build "key":" pattern */
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char *start = strstr(json, pattern);
    if (!start) {
        return NULL;
    }
    start += strlen(pattern);
    const char *end = strchr(start, '"');
    if (!end || end == start) {
        return NULL;
    }
    int len = (int)(end - start);
    if (len >= bufsz) {
        len = bufsz - 1;
    }
    memcpy(buf, start, (size_t)len);
    buf[len] = '\0';
    return buf;
}

/* Visitor context for edge scanning */
typedef struct {
    cbm_gbuf_t *gb;
    int created;
} route_ctx_t;

static void route_edge_visitor(const cbm_gbuf_edge_t *edge, void *userdata) {
    route_ctx_t *ctx = (route_ctx_t *)userdata;

    /* Only process HTTP_CALLS and ASYNC_CALLS */
    if (strcmp(edge->type, "HTTP_CALLS") != 0 && strcmp(edge->type, "ASYNC_CALLS") != 0) {
        return;
    }

    /* Extract url_path from properties */
    char url_buf[512];
    const char *url = json_extract(edge->properties_json, "url_path", url_buf, sizeof(url_buf));
    if (!url || !url[0]) {
        return;
    }

    /* Extract method or broker */
    char method_buf[16];
    char broker_buf[64];
    const char *method =
        json_extract(edge->properties_json, "method", method_buf, sizeof(method_buf));
    const char *broker =
        json_extract(edge->properties_json, "broker", broker_buf, sizeof(broker_buf));

    /* Build Route QN */
    char route_qn[CBM_ROUTE_QN_SIZE];
    if (strcmp(edge->type, "HTTP_CALLS") == 0) {
        snprintf(route_qn, sizeof(route_qn), "__route__%s__%s", method ? method : "ANY", url);
    } else {
        snprintf(route_qn, sizeof(route_qn), "__route__%s__%s", broker ? broker : "async", url);
    }

    /* Build properties for Route node */
    char route_props[256];
    if (method) {
        snprintf(route_props, sizeof(route_props), "{\"method\":\"%s\"}", method);
    } else if (broker) {
        snprintf(route_props, sizeof(route_props), "{\"broker\":\"%s\"}", broker);
    } else {
        snprintf(route_props, sizeof(route_props), "{}");
    }

    /* Create or find Route node (deduped by QN) */
    cbm_gbuf_upsert_node(ctx->gb, "Route", url, route_qn, "", 0, 0, route_props);
    ctx->created++;

    /* Note: we do NOT re-target the edge here because modifying edges during
     * iteration is unsafe. The edge stays pointing to the library function.
     * The httplink URL matching pass will create the Route→handler HANDLES
     * edge separately. The caller→Route edge is created by pass_calls for
     * the sequential path; for the parallel path, the caller→library edge
     * with url_path in properties is sufficient for query_graph to find
     * the Route via: caller → HTTP_CALLS(url_path="/api/x") + Route("/api/x"). */
}

/* Extract URL path from full URL: "https://host/path/" → "/path/" */
static const char *url_path(const char *url) {
    if (!url) {
        return NULL;
    }
    const char *scheme_end = strstr(url, "://");
    if (!scheme_end) {
        return url; /* Already a path */
    }
    const char *path = strchr(scheme_end + 3, '/');
    return path ? path : "/";
}

/* Extract service name from Cloud Run URL hostname.
 * "my-svc-ab12cd34ef-uc.a.run.app/path" → "my-svc" */
static const char *extract_service_name(const char *url, char *buf, int bufsz) {
    if (!url) {
        return NULL;
    }
    const char *scheme_end = strstr(url, "://");
    if (!scheme_end) {
        return NULL;
    }
    const char *host_start = scheme_end + 3;
    /* Service name is everything before the revision hash.
     * Pattern: service-name-HASH-HASH.region.run.app
     * Heuristic: find the longest prefix of dash-separated words
     * before a segment that looks like a hash (short alphanumeric). */
    const char *end = host_start;
    while (*end && *end != '.' && *end != '/') {
        end++;
    }
    /* Copy full hostname part before first dot */
    int hlen = (int)(end - host_start);
    if (hlen <= 0 || hlen >= bufsz) {
        return NULL;
    }

    /* Walk backward from end, stripping hash-like segments.
     * Cloud Run format: name-REVHASH-LOCHASH.region.run.app */
    char tmp[256];
    if (hlen >= (int)sizeof(tmp)) {
        return NULL;
    }
    memcpy(tmp, host_start, (size_t)hlen);
    tmp[hlen] = '\0';

    /* Cloud Run hostname format: service-name-REVHASH-LOCHASH.region.run.app
     * Strip last two dash-separated segments (revision + location hashes).
     * Hash segments are typically 2-12 alphanumeric characters. */
    for (int strip = 0; strip < 2; strip++) {
        char *last_dash = strrchr(tmp, '-');
        if (last_dash && strlen(last_dash + 1) <= 12) {
            *last_dash = '\0';
        }
    }

    snprintf(buf, (size_t)bufsz, "%s", tmp);
    return buf;
}

/* Phase 2: Match infra Route URLs to handler Route nodes by URL path + service name. */
static void match_infra_routes(cbm_gbuf_t *gb) {
    /* Collect infra Routes (from YAML) and handler Routes (from Python decorators) */
    const cbm_gbuf_node_t **all_routes = NULL;
    int route_count = 0;
    if (cbm_gbuf_find_by_label(gb, "Route", &all_routes, &route_count) != 0 || route_count == 0) {
        return;
    }

    int matched = 0;

    for (int i = 0; i < route_count; i++) {
        const cbm_gbuf_node_t *infra = all_routes[i];
        /* Only process infra routes (full URLs) */
        if (!infra->qualified_name || strncmp(infra->qualified_name, "__route__infra__", 16) != 0) {
            continue;
        }
        if (!infra->name || !strstr(infra->name, "://")) {
            continue;
        }

        const char *infra_path = url_path(infra->name);
        char svc_buf[128];
        const char *svc_name = extract_service_name(infra->name, svc_buf, sizeof(svc_buf));
        if (!infra_path || !svc_name) {
            continue;
        }

        /* Find handler Routes whose file_path contains the service name
         * and whose route path matches the infra URL path */
        for (int j = 0; j < route_count; j++) {
            const cbm_gbuf_node_t *handler_route = all_routes[j];
            /* Skip infra routes */
            if (handler_route->qualified_name &&
                strncmp(handler_route->qualified_name, "__route__", 9) == 0) {
                continue;
            }
            /* Handler route must be in the matching service directory */
            if (!handler_route->file_path || !strstr(handler_route->file_path, svc_name)) {
                continue;
            }

            /* Check if the infra URL path matches the handler route path.
             * Handler route name is like "POST /path" — extract the path part. */
            const char *handler_name = handler_route->name;
            if (!handler_name) {
                continue;
            }
            /* Skip method prefix: "POST /path" → "/path" */
            const char *handler_path = strchr(handler_name, '/');
            if (!handler_path) {
                continue;
            }

            /* Match: infra path starts with handler path or handler path contains infra path */
            if (strstr(infra_path, handler_path) != NULL ||
                strstr(handler_path, infra_path) != NULL) {
                /* Create HANDLES edge: infra Route → handler Route (connecting them) */
                cbm_gbuf_insert_edge(gb, infra->id, handler_route->id, "HANDLES",
                                     "{\"source\":\"infra_match\"}");
                matched++;
                break; /* One match per infra route is enough */
            }
        }
    }

    if (matched > 0) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", matched);
        cbm_log_info("pass.route_match", "infra_matched", buf);
    }
}

/* Phase 3: Create DATA_FLOWS edges by linking callers through Route to handlers.
 * For each HTTP_CALLS/ASYNC_CALLS edge (caller → Route), find the HANDLES edge
 * (handler → Route) and create DATA_FLOWS (caller → handler) with route context. */
static void create_data_flows(cbm_gbuf_t *gb) {
    const cbm_gbuf_node_t **routes = NULL;
    int route_count = 0;
    if (cbm_gbuf_find_by_label(gb, "Route", &routes, &route_count) != 0 || route_count == 0) {
        return;
    }

    int flows = 0;

    /* For each Route node, find callers (HTTP_CALLS/ASYNC_CALLS → Route)
     * and handlers (HANDLES → Route), then create DATA_FLOWS links. */
    for (int ri = 0; ri < route_count; ri++) {
        const cbm_gbuf_node_t *route = routes[ri];

        /* Find HTTP_CALLS → Route */
        const cbm_gbuf_edge_t **http_edges = NULL;
        int http_count = 0;
        cbm_gbuf_find_edges_by_target_type(gb, route->id, "HTTP_CALLS", &http_edges, &http_count);

        /* Find ASYNC_CALLS → Route */
        const cbm_gbuf_edge_t **async_edges = NULL;
        int async_count = 0;
        cbm_gbuf_find_edges_by_target_type(gb, route->id, "ASYNC_CALLS", &async_edges,
                                           &async_count);

        /* Find HANDLES → Route */
        const cbm_gbuf_edge_t **handles_edges = NULL;
        int handles_count = 0;
        cbm_gbuf_find_edges_by_target_type(gb, route->id, "HANDLES", &handles_edges,
                                           &handles_count);

        /* Collect caller IDs */
        int64_t callers[64];
        int n_callers = 0;
        for (int ei = 0; ei < http_count && n_callers < 64; ei++) {
            callers[n_callers++] = http_edges[ei]->source_id;
        }
        for (int ei = 0; ei < async_count && n_callers < 64; ei++) {
            callers[n_callers++] = async_edges[ei]->source_id;
        }

        /* Collect handler IDs */
        int64_t handlers[16];
        int n_handlers = 0;
        for (int ei = 0; ei < handles_count && n_handlers < 16; ei++) {
            handlers[n_handlers++] = handles_edges[ei]->source_id;
        }

        /* Create DATA_FLOWS: each caller → each handler through this Route */
        for (int ci = 0; ci < n_callers; ci++) {
            for (int hi = 0; hi < n_handlers; hi++) {
                if (callers[ci] == handlers[hi]) {
                    continue; /* skip self-links */
                }
                char props[512];
                snprintf(props, sizeof(props), "{\"via_route\":\"%s\",\"route_qn\":\"%s\"}",
                         route->name ? route->name : "",
                         route->qualified_name ? route->qualified_name : "");
                cbm_gbuf_insert_edge(gb, callers[ci], handlers[hi], "DATA_FLOWS", props);
                flows++;
            }
        }
    }

    if (flows > 0) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", flows);
        cbm_log_info("pass.data_flows", "created", buf);
    }
}

void cbm_pipeline_create_route_nodes(cbm_gbuf_t *gb) {
    if (!gb) {
        return;
    }

    route_ctx_t ctx = {.gb = gb, .created = 0};
    cbm_gbuf_foreach_edge(gb, route_edge_visitor, &ctx);

    if (ctx.created > 0) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", ctx.created);
        cbm_log_info("pass.route_nodes", "created", buf);
    }

    /* Phase 2: match infra Routes to handler Routes by URL path */
    match_infra_routes(gb);

    /* Phase 3: create DATA_FLOWS edges through Routes */
    create_data_flows(gb);
}
