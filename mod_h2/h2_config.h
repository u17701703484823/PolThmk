/* Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __mod_h2__h2_config_h__
#define __mod_h2__h2_config_h__

#include <http_config.h>

typedef enum {
    H2_CONF_ENABLED,
    H2_CONF_MAX_STREAMS,
    H2_CONF_MAX_HL_SIZE,
    H2_CONF_WIN_SIZE,
    H2_CONF_MIN_WORKERS,
    H2_CONF_MAX_WORKERS,
} h2_config_var_t;

/* Apache httpd module configuration for h2. */
typedef struct h2_config {
    const char *name;
    int h2_enabled;
    int h2_max_streams;
    int h2_max_hl_size;
    int h2_window_size;
    int h2_min_workers;
    int h2_max_workers;
} h2_config;


void *h2_config_create_dir(apr_pool_t *pool, char *x);
void *h2_config_create_svr(apr_pool_t *pool, server_rec *s);
void *h2_config_merge(apr_pool_t *pool, void *basev, void *addv);

apr_status_t h2_config_apply_header(h2_config *config, request_rec *r);

extern const command_rec h2_cmds[];

h2_config *h2_config_get(conn_rec *c);
h2_config *h2_config_sget(server_rec *s);

int h2_config_geti(h2_config *conf, h2_config_var_t var);

#endif /* __mod_h2__h2_config_h__ */

