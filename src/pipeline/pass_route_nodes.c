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
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
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

    /* Cloud Run hostname formats:
     * Old: service-name-REVHASH-LOCHASH.region.run.app (strip 2 segments)
     * New: service-name-PROJECTNUM.region.run.app (strip 1 segment)
     * Hash/project-num segments: short alphanumeric or all-numeric.
     * Never strip a segment that looks like a meaningful word (>3 chars, has letters). */
    for (int strip = 0; strip < 2; strip++) {
        char *last_dash = strrchr(tmp, '-');
        if (!last_dash) {
            break;
        }
        const char *seg = last_dash + 1;
        size_t slen = strlen(seg);
        if (slen > 12 || slen == 0) {
            break;
        }
        /* Check if segment is all-numeric (GCP project number) or short alphanumeric (hash) */
        int all_alnum = 1;
        int has_letter = 0;
        for (size_t si = 0; si < slen; si++) {
            if (!((seg[si] >= '0' && seg[si] <= '9') || (seg[si] >= 'a' && seg[si] <= 'z'))) {
                all_alnum = 0;
                break;
            }
            if (seg[si] >= 'a' && seg[si] <= 'z') {
                has_letter = 1;
            }
        }
        if (!all_alnum) {
            break;
        }
        /* Don't strip meaningful words (>=3 chars with letters like "api", "endpoint") */
        if (has_letter && slen >= 3) {
            break;
        }
        *last_dash = '\0';
    }

    snprintf(buf, (size_t)bufsz, "%s", tmp);
    return buf;
}

/* Phase 2: Match infra Route URLs to handler Route nodes by URL path + service name. */
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
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
            /* Skip infra/async/broker Route nodes — match only handler/prefix Routes */
            const char *hqn = handler_route->qualified_name;
            if (hqn &&
                (strstr(hqn, "__route__infra__") == hqn ||
                 strstr(hqn, "__route__pubsub__") == hqn ||
                 strstr(hqn, "__route__cloud_tasks__") == hqn ||
                 strstr(hqn, "__route__async__") == hqn ||
                 strstr(hqn, "__route__cloud_scheduler__") == hqn ||
                 strstr(hqn, "__route__kafka__") == hqn || strstr(hqn, "__route__sqs__") == hqn)) {
                continue;
            }
            /* Handler route must be in the matching service directory.
             * Prefix Routes from include_router have no file_path — match by path only. */
            int file_matches = (handler_route->file_path != NULL &&
                                strstr(handler_route->file_path, svc_name) != NULL);
            int is_prefix_route =
                (handler_route->qualified_name != NULL &&
                 strncmp(handler_route->qualified_name, "__route__ANY__", 14) == 0);
            if (!file_matches && !is_prefix_route) {
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

            /* Match: infra path contains handler path, handler path contains infra path,
             * OR handler is root "/" and service name matches (microservice pattern:
             * each service has one handler at "/", differentiated by hostname). */
            int path_match =
                (strlen(handler_path) > 1 && (strstr(infra_path, handler_path) != NULL ||
                                              strstr(handler_path, infra_path) != NULL));
            int root_svc_match = (strcmp(handler_path, "/") == 0);
            if (path_match || root_svc_match) {
                /* Find the handler Function that HANDLES this decorator Route */
                const cbm_gbuf_edge_t **fn_handles = NULL;
                int fn_hcount = 0;
                cbm_gbuf_find_edges_by_target_type(gb, handler_route->id, "HANDLES", &fn_handles,
                                                   &fn_hcount);
                for (int fh = 0; fh < fn_hcount; fh++) {
                    /* Create HANDLES: handler Function → infra Route
                     * This bridges the gap so DATA_FLOWS can find the handler */
                    cbm_gbuf_insert_edge(gb, fn_handles[fh]->source_id, infra->id, "HANDLES",
                                         "{\"source\":\"infra_match\"}");
                }
                matched++;
                break;
            }
        }
    }

    if (matched > 0) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", matched);
        cbm_log_info("pass.route_match", "infra_matched", buf);
    }
}

/* Phase 2a: Ensure all functions with route_path properties have Route+HANDLES edges.
 * During incremental indexing, only changed files get Route nodes from extraction.
 * This pass scans ALL Function/Method nodes and creates missing Route+HANDLES. */
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static void ensure_decorator_routes(cbm_gbuf_t *gb) {
    const char *labels[] = {"Function", "Method"};
    int created = 0;

    for (int li = 0; li < 2; li++) {
        const cbm_gbuf_node_t **nodes = NULL;
        int count = 0;
        if (cbm_gbuf_find_by_label(gb, labels[li], &nodes, &count) != 0) {
            continue;
        }
        for (int i = 0; i < count; i++) {
            const cbm_gbuf_node_t *func = nodes[i];
            if (!func->properties_json) {
                continue;
            }
            /* Extract route_path from properties */
            const char *rp = strstr(func->properties_json, "\"route_path\":\"");
            if (!rp) {
                continue;
            }
            rp += 14; /* skip "route_path":" */
            const char *rp_end = strchr(rp, '"');
            if (!rp_end || rp_end <= rp) {
                continue;
            }
            int plen = (int)(rp_end - rp);
            char path[256];
            if (plen >= (int)sizeof(path)) {
                continue;
            }
            memcpy(path, rp, (size_t)plen);
            path[plen] = '\0';
            if (path[0] != '/') {
                continue;
            }

            /* Extract route_method */
            const char *rm = strstr(func->properties_json, "\"route_method\":\"");
            char method[16] = "ANY";
            if (rm) {
                rm += 16;
                const char *rm_end = strchr(rm, '"');
                if (rm_end && rm_end > rm && (rm_end - rm) < 16) {
                    memcpy(method, rm, (size_t)(rm_end - rm));
                    method[rm_end - rm] = '\0';
                }
            }

            /* Check if Route node already exists */
            char route_qn[CBM_ROUTE_QN_SIZE];
            snprintf(route_qn, sizeof(route_qn), "__route__%s__%s", method, path);
            const cbm_gbuf_node_t *existing = cbm_gbuf_find_by_qn(gb, route_qn);

            /* Create Route if missing */
            char rprops[256];
            snprintf(rprops, sizeof(rprops), "{\"method\":\"%s\",\"source\":\"decorator\"}",
                     method);
            int64_t route_id = cbm_gbuf_upsert_node(
                gb, "Route", path, route_qn, func->file_path ? func->file_path : "", 0, 0, rprops);

            /* Check if HANDLES edge already exists */
            if (existing) {
                const cbm_gbuf_edge_t **existing_handles = NULL;
                int eh_count = 0;
                cbm_gbuf_find_edges_by_target_type(gb, route_id, "HANDLES", &existing_handles,
                                                   &eh_count);
                int already_handled = 0;
                for (int eh = 0; eh < eh_count; eh++) {
                    if (existing_handles[eh]->source_id == func->id) {
                        already_handled = 1;
                        break;
                    }
                }
                if (already_handled) {
                    continue;
                }
            }

            char hprops[512];
            snprintf(hprops, sizeof(hprops), "{\"handler\":\"%s\"}",
                     func->qualified_name ? func->qualified_name : "");
            cbm_gbuf_insert_edge(gb, func->id, route_id, "HANDLES", hprops);
            created++;
        }
    }

    if (created > 0) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", created);
        cbm_log_info("pass.ensure_decorator_routes", "created", buf);
    }
}

/* Phase 2b: Connect prefix Routes to decorator handler Functions.
 * For each prefix Route (__route__ANY__/path), find the CALLS edge leading to it
 * (from the registering file), derive the service directory, then find decorator
 * Routes in that directory tree and create HANDLES from their handler Functions
 * to the prefix Route. This bridges include_router → decorator → handler. */
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static void connect_prefix_to_decorators(cbm_gbuf_t *gb) {
    const cbm_gbuf_node_t **routes = NULL;
    int route_count = 0;
    if (cbm_gbuf_find_by_label(gb, "Route", &routes, &route_count) != 0) {
        return;
    }

    int connected = 0;

    for (int ri = 0; ri < route_count; ri++) {
        const cbm_gbuf_node_t *prefix_route = routes[ri];
        /* Only process prefix Routes from include_router */
        if (!prefix_route->qualified_name ||
            strncmp(prefix_route->qualified_name, "__route__ANY__/", 15) != 0) {
            continue;
        }

        /* Find the CALLS edge TO this prefix Route (from the registering function) */
        const cbm_gbuf_edge_t **calls_in = NULL;
        int calls_count = 0;
        cbm_gbuf_find_edges_by_target_type(gb, prefix_route->id, "CALLS", &calls_in, &calls_count);
        if (calls_count == 0) {
            continue;
        }

        /* Get the registering file's directory as the service root */
        const cbm_gbuf_node_t *registrar = cbm_gbuf_find_by_id(gb, calls_in[0]->source_id);
        if (!registrar || !registrar->file_path) {
            continue;
        }
        /* Extract directory: "docker-images/cloud-runs/svc/main.py" →
         * "docker-images/cloud-runs/svc/" */
        const char *last_slash = strrchr(registrar->file_path, '/');
        if (!last_slash) {
            continue;
        }
        int dir_len = (int)(last_slash - registrar->file_path) + 1;

        /* Convert prefix path to directory segments for file path matching.
         * Prefix "/bq/bulk/ranked" → "bq/bulk/ranked" (skip leading /) */
        const char *prefix_path = prefix_route->name;
        const char *prefix_segs =
            (prefix_path && prefix_path[0] == '/') ? prefix_path + 1 : prefix_path;

        /* Find Function/Method nodes whose file path contains the prefix segments
         * AND has route_path in properties (AST-extracted decorator route). */
        const cbm_gbuf_node_t **funcs = NULL;
        int func_count = 0;
        cbm_gbuf_find_by_label(gb, "Function", &funcs, &func_count);

        for (int fi = 0; fi < func_count; fi++) {
            const cbm_gbuf_node_t *func = funcs[fi];
            if (!func->file_path) {
                continue;
            }
            /* Must be in the same service directory */
            if (strncmp(func->file_path, registrar->file_path, (size_t)dir_len) != 0) {
                continue;
            }
            /* Must have route_path in properties (AST-extracted decorator route) */
            if (!func->properties_json || !strstr(func->properties_json, "\"route_path\"")) {
                continue;
            }
            /* File path must contain the prefix path segments.
             * E.g., prefix "/bq/bulk/ranked" → file must contain "bq/bulk/ranked" */
            if (prefix_segs && prefix_segs[0] && !strstr(func->file_path, prefix_segs)) {
                continue;
            }
            /* Create HANDLES: handler Function → prefix Route */
            char props[256];
            snprintf(props, sizeof(props), "{\"source\":\"prefix_decorator_bridge\"}");
            cbm_gbuf_insert_edge(gb, func->id, prefix_route->id, "HANDLES", props);
            connected++;
        }
    }

    if (connected > 0) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", connected);
        cbm_log_info("pass.prefix_bridge", "connected", buf);
    }
}

/* Phase 3: Create DATA_FLOWS edges by linking callers through Route to handlers.
 * For each HTTP_CALLS/ASYNC_CALLS edge (caller → Route), find the HANDLES edge
 * (handler → Route) and create DATA_FLOWS (caller → handler) with route context. */
/* Check if a direct CALLS edge already exists between two nodes */
static int has_direct_call(const cbm_gbuf_t *gb, int64_t source, int64_t target) {
    const cbm_gbuf_edge_t **edges = NULL;
    int count = 0;
    cbm_gbuf_find_edges_by_source_type(gb, source, "CALLS", &edges, &count);
    for (int i = 0; i < count; i++) {
        if (edges[i]->target_id == target) {
            return 1;
        }
    }
    return 0;
}

/* Extract param_names from a node's properties_json.
 * Returns a comma-separated string in buf, or empty string. */
static void extract_param_names(const cbm_gbuf_node_t *node, char *buf, int bufsize) {
    buf[0] = '\0';
    if (!node || !node->properties_json) {
        return;
    }
    const char *p = strstr(node->properties_json, "\"param_names\":");
    if (!p) {
        return;
    }
    p = strchr(p, '[');
    if (!p) {
        return;
    }
    p++; /* skip '[' */
    const char *end = strchr(p, ']');
    if (!end || end <= p) {
        return;
    }
    int len = (int)(end - p);
    if (len >= bufsize) {
        len = bufsize - 1;
    }
    memcpy(buf, p, (size_t)len);
    buf[len] = '\0';
}

/* Extract the "args" JSON fragment from an edge's properties.
 * Returns pointer into the properties string (not copied). */
static const char *find_args_in_props(const char *props) {
    if (!props) {
        return NULL;
    }
    const char *p = strstr(props, "\"args\":[");
    if (!p) {
        return NULL;
    }
    return p + 7; /* skip "args":[ , points to first { or ] */
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static void create_data_flows(cbm_gbuf_t *gb) {
    const cbm_gbuf_node_t **routes = NULL;
    int route_count = 0;
    if (cbm_gbuf_find_by_label(gb, "Route", &routes, &route_count) != 0 || route_count == 0) {
        return;
    }

    int flows = 0;
    int skipped = 0;

    for (int ri = 0; ri < route_count; ri++) {
        const cbm_gbuf_node_t *route = routes[ri];

        /* Collect caller edges (HTTP_CALLS + ASYNC_CALLS → Route) */
        const cbm_gbuf_edge_t **http_edges = NULL;
        int http_count = 0;
        cbm_gbuf_find_edges_by_target_type(gb, route->id, "HTTP_CALLS", &http_edges, &http_count);

        const cbm_gbuf_edge_t **async_edges = NULL;
        int async_count = 0;
        cbm_gbuf_find_edges_by_target_type(gb, route->id, "ASYNC_CALLS", &async_edges,
                                           &async_count);

        /* Collect caller edge references (need properties for arg mapping) */
        struct {
            int64_t source_id;
            const char *props;
            const char *edge_type;
        } caller_edges[64];
        int n_callers = 0;
        for (int ei = 0; ei < http_count && n_callers < 64; ei++) {
            caller_edges[n_callers].source_id = http_edges[ei]->source_id;
            caller_edges[n_callers].props = http_edges[ei]->properties_json;
            caller_edges[n_callers].edge_type = "HTTP_CALLS";
            n_callers++;
        }
        for (int ei = 0; ei < async_count && n_callers < 64; ei++) {
            caller_edges[n_callers].source_id = async_edges[ei]->source_id;
            caller_edges[n_callers].props = async_edges[ei]->properties_json;
            caller_edges[n_callers].edge_type = "ASYNC_CALLS";
            n_callers++;
        }

        /* Collect handler nodes: direct HANDLES → Route, plus HANDLES on
         * endpoint Routes reachable via INFRA_MAPS (topic → endpoint → handler) */
        const cbm_gbuf_edge_t **handles_edges = NULL;
        int handles_count = 0;
        cbm_gbuf_find_edges_by_target_type(gb, route->id, "HANDLES", &handles_edges,
                                           &handles_count);

        /* Also follow INFRA_MAPS: topic Route → endpoint Route → HANDLES
         * This bridges async topic Routes to their push endpoint handlers. */
        const cbm_gbuf_edge_t **infra_edges = NULL;
        int infra_count = 0;
        cbm_gbuf_find_edges_by_source_type(gb, route->id, "INFRA_MAPS", &infra_edges, &infra_count);

        /* Extra handlers found via INFRA_MAPS chain */
        int64_t extra_handlers[32];
        int n_extra = 0;
        for (int ie = 0; ie < infra_count; ie++) {
            int64_t endpoint_id = infra_edges[ie]->target_id;
            /* Find HANDLES edges on the endpoint Route */
            const cbm_gbuf_edge_t **ep_handles = NULL;
            int ep_hcount = 0;
            cbm_gbuf_find_edges_by_target_type(gb, endpoint_id, "HANDLES", &ep_handles, &ep_hcount);
            for (int eh = 0; eh < ep_hcount && n_extra < 32; eh++) {
                extra_handlers[n_extra++] = ep_handles[eh]->source_id;
            }
        }

        for (int ci = 0; ci < n_callers; ci++) {
            for (int hi = 0; hi < handles_count; hi++) {
                int64_t caller_id = caller_edges[ci].source_id;
                int64_t handler_id = handles_edges[hi]->source_id;

                if (caller_id == handler_id) {
                    continue;
                }

                /* Skip if direct CALLS edge already exists */
                if (has_direct_call(gb, caller_id, handler_id)) {
                    skipped++;
                    continue;
                }

                /* Build value mapping: caller args → handler params */
                const char *args_json = find_args_in_props(caller_edges[ci].props);

                const cbm_gbuf_node_t *handler_node = cbm_gbuf_find_by_id(gb, handler_id);
                char handler_params[512];
                extract_param_names(handler_node, handler_params, sizeof(handler_params));

                /* Build DATA_FLOWS properties with actual value mapping */
                char props[2048];
                int n = snprintf(
                    props, sizeof(props), "{\"via\":\"%s\",\"route\":\"%s\",\"edge_type\":\"%s\"",
                    route->name ? route->name : "",
                    route->qualified_name ? route->qualified_name : "", caller_edges[ci].edge_type);

                if (n > 0 && (size_t)n < sizeof(props) - 100) {
                    size_t pos = (size_t)n;

                    /* Include handler param_names */
                    if (handler_params[0]) {
                        int w = snprintf(props + pos, sizeof(props) - pos,
                                         ",\"handler_params\":[%s]", handler_params);
                        if (w > 0) {
                            pos += (size_t)w;
                        }
                    }

                    /* Include caller args (copy from source edge) */
                    if (args_json) {
                        int w = snprintf(props + pos, sizeof(props) - pos, ",\"caller_args\":[%.*s",
                                         400, args_json);
                        if (w > 0) {
                            pos += (size_t)w;
                            /* Find closing ] in the copied fragment */
                            char *close = strchr(props + (pos - (size_t)w) + 14, ']');
                            if (close && close < props + sizeof(props) - 2) {
                                pos = (size_t)(close - props) + 1;
                            }
                        }
                    }

                    if (pos < sizeof(props) - 1) {
                        props[pos] = '}';
                        props[pos + 1] = '\0';
                    }
                }
                cbm_gbuf_insert_edge(gb, caller_id, handler_id, "DATA_FLOWS", props);
                flows++;
            }

            /* Also check handlers found via INFRA_MAPS chain */
            for (int xi = 0; xi < n_extra; xi++) {
                int64_t caller_id = caller_edges[ci].source_id;
                int64_t handler_id = extra_handlers[xi];
                if (caller_id == handler_id) {
                    continue;
                }
                if (has_direct_call(gb, caller_id, handler_id)) {
                    skipped++;
                    continue;
                }
                const char *args_json = find_args_in_props(caller_edges[ci].props);
                const cbm_gbuf_node_t *handler_node = cbm_gbuf_find_by_id(gb, handler_id);
                char handler_params[512];
                extract_param_names(handler_node, handler_params, sizeof(handler_params));

                char props[2048];
                int n = snprintf(props, sizeof(props),
                                 "{\"via\":\"%s\",\"route\":\"%s\",\"edge_type\":\"%s\","
                                 "\"via_infra\":true",
                                 route->name ? route->name : "",
                                 route->qualified_name ? route->qualified_name : "",
                                 caller_edges[ci].edge_type);
                if (n > 0 && (size_t)n < sizeof(props) - 100) {
                    size_t pos = (size_t)n;
                    if (handler_params[0]) {
                        int w = snprintf(props + pos, sizeof(props) - pos,
                                         ",\"handler_params\":[%s]", handler_params);
                        if (w > 0) {
                            pos += (size_t)w;
                        }
                    }
                    if (args_json) {
                        int w = snprintf(props + pos, sizeof(props) - pos, ",\"caller_args\":[%.*s",
                                         400, args_json);
                        if (w > 0) {
                            pos += (size_t)w;
                            char *close = strchr(props + (pos - (size_t)w) + 14, ']');
                            if (close && close < props + sizeof(props) - 2) {
                                pos = (size_t)(close - props) + 1;
                            }
                        }
                    }
                    if (pos < sizeof(props) - 1) {
                        props[pos] = '}';
                        props[pos + 1] = '\0';
                    }
                }
                cbm_gbuf_insert_edge(gb, caller_id, handler_id, "DATA_FLOWS", props);
                flows++;
            }
        }
    }

    if (flows > 0 || skipped > 0) {
        char buf1[16];
        char buf2[16];
        snprintf(buf1, sizeof(buf1), "%d", flows);
        snprintf(buf2, sizeof(buf2), "%d", skipped);
        cbm_log_info("pass.data_flows", "created", buf1, "skipped_has_call", buf2);
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

    /* Phase 2a: ensure all functions with route_path have Route+HANDLES.
     * Handles incremental mode where unchanged files don't re-extract. */
    ensure_decorator_routes(gb);

    /* Phase 2b: connect prefix Routes to decorator handler Functions.
     * Must run BEFORE match_infra_routes so infra matching can find
     * HANDLES edges on prefix Routes for the bridge. */
    connect_prefix_to_decorators(gb);

    /* Phase 2b: match infra Routes to handler Routes by URL path */
    match_infra_routes(gb);

    /* Phase 3: create DATA_FLOWS edges through Routes */
    create_data_flows(gb);
}
