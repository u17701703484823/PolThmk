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

#ifndef __mod_h2__h2_from_h1__
#define __mod_h2__h2_from_h1__

/**
 * h2_from_h1 parses a HTTP/1.1 response into
 * - response status
 * - a list of header values
 * - a series of bytes that represent the response body alone, without
 *   any meta data, such as inserted by chunked transfer encoding.
 *
 * All data is allocated from the connection memory pool. Body data
 * is passed "through" into the given h2_bucket(s) and will not
 * cause allocations.
 *
 * Again, see comments in h2_request: ideally we would take the headers
 * and status from the httpd structures instead of parsing them here, but
 * we need to have all handlers and filters involved in request/response
 * processing, so this seems to be the way for now.
 */

typedef enum {
    H2_RESP_ST_STATUS_LINE, /* parsing http/1 status line */
    H2_RESP_ST_HEADERS,     /* parsing http/1 response headers */
    H2_RESP_ST_BODY,        /* transferring response body */
    H2_RESP_ST_DONE         /* complete response converted */
} h2_from_h1_state_t;

struct h2_bucket;
struct h2_response;
typedef struct h2_from_h1 h2_from_h1;

typedef void h2_from_h1_state_change_cb(struct h2_from_h1 *resp,
                                         h2_from_h1_state_t prevstate,
                                         void *cb_ctx);

h2_from_h1 *h2_from_h1_create(int stream_id, apr_pool_t *pool);
apr_status_t h2_from_h1_destroy(h2_from_h1 *response);

void h2_from_h1_set_state_change_cb(h2_from_h1 *from_h1,
                                     h2_from_h1_state_change_cb *callback,
                                     void *cb_ctx);

apr_status_t h2_from_h1_http_convert(h2_from_h1 *from_h1,
                                      struct h2_bucket *bucket,
                                      const char *data, apr_size_t len,
                                      apr_size_t *pconsumed);

struct h2_response *h2_from_h1_get_response(h2_from_h1 *from_h1);

h2_from_h1_state_t h2_from_h1_get_state(h2_from_h1 *from_h1);

#endif /* defined(__mod_h2__h2_from_h1__) */
