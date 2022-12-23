/* Copyright 2015 greenbytes GmbH (https://www.greenbytes.de)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <apr_strings.h>
#include <httpd.h>
#include <http_core.h>
#include <http_log.h>

#include "h2_private.h"
#include "h2_alt_svc.h"
#include "h2_ctx.h"
#include "h2_config.h"
#include "h2_h2.h"
#include "h2_util.h"

static int h2_alt_svc_request_handler(request_rec *r);

void h2_alt_svc_register_hooks(void)
{
    ap_hook_handler(h2_alt_svc_request_handler, NULL, NULL, APR_HOOK_LAST);
}

/**
 * Parse an Alt-Svc specifier as described in "HTTP Alternative Services"
 * (https://tools.ietf.org/html/draft-ietf-httpbis-alt-svc-04)
 * with the following changes:
 * - do not percent encode token values
 * - do not use quotation marks
 */
h2_alt_svc *h2_alt_svc_parse(const char *s, apr_pool_t *pool) {
    const char *sep = strchr(s, '=');
    if (sep) {
        const char *alpn = apr_pstrndup(pool, s, sep - s);
        const char *host = NULL;
        int port = 0;
        s = sep + 1;
        sep = strchr(s, ':');  /* mandatory : */
        if (sep) {
            if (sep != s) {    /* optional host */
                host = apr_pstrndup(pool, s, sep - s);
            }
            s = sep + 1;
            if (*s) {          /* must be a port number */
                port = (int)apr_atoi64(s);
                if (port > 0 && port < (0x1 << 16)) {
                    h2_alt_svc *as = apr_pcalloc(pool, sizeof(*as));
                    as->alpn = alpn;
                    as->host = host;
                    as->port = port;
                    return as;
                }
            }
        }
    }
    return NULL;
}

#define h2_alt_svc_IDX(list, i) ((h2_alt_svc**)(list)->elts)[i]

static int h2_alt_svc_request_handler(request_rec *r)
{
    if (h2_ctx_is_active(r->connection)
        || h2_ctx_is_task(r->connection)) {
        return DECLINED;
    }
    
    h2_config *cfg = h2_config_rget(r);
    if (r->hostname && cfg && cfg->alt_svcs && cfg->alt_svcs->nelts > 0) {
        const char *alt_svc_used = apr_table_get(r->headers_in, "Alt-Svc-Used");
        if (!alt_svc_used /*|| (alt_svc_used[0] == '0')*/) {
            /* We have alt-svcs defined and client is not already using
             * one, announce the services that were configured and match. 
             * The security of this connection determines if we allow
             * other host names or ports only.
             */
            const char *alt_svc = "";
            const char *svc_ma = "";
            int secure = h2_h2_is_tls(r->connection);
            int ma = h2_config_geti(cfg, H2_CONF_ALT_SVC_MAX_AGE);
            if (ma >= 0) {
                svc_ma = apr_psprintf(r->pool, "; ma=%d", ma);
            }
            ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r,
                          "h2_alt_svc: announce %s for %s:%d", 
                          (secure? "secure" : "insecure"), 
                          r->hostname, (int)r->server->port);
            for (int i = 0; i < cfg->alt_svcs->nelts; ++i) {
                h2_alt_svc *as = h2_alt_svc_IDX(cfg->alt_svcs, i);
                const char *ahost = as->host;
                if (ahost && !apr_strnatcasecmp(ahost, r->hostname)) {
                    ahost = NULL;
                }
                if (secure || !ahost) {
                    alt_svc = apr_psprintf(r->pool, "%s%s%s=\"%s:%d\"%s", 
                                           alt_svc,
                                           (*alt_svc? ", " : ""), as->alpn,
                                           ahost? ahost : "", as->port,
                                           svc_ma);
                }
            }
            if (*alt_svc) {
                apr_table_set(r->headers_out, "Alt-Svc", alt_svc);
            }
        }
    }
    
    return DECLINED;
}

