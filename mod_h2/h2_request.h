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

#ifndef __mod_h2__h2_request__
#define __mod_h2__h2_request__

/* h2_request is the transformer of HTTP2 streams into HTTP/1.1 internal
 * format that will be fed to various httpd input filters to finally
 * become a request_rec to be handled by soemone.
 *
 * Ideally, we would make a request_rec without serializing the headers
 * we have only to make someone else parse them back.
 */
struct h2_bucket;
struct h2_bucket_queue;
struct h2_to_h1;

typedef struct h2_request h2_request;

struct h2_request {
    int id;                 /* http2 stream id */
    apr_pool_t *pool;
    struct h2_to_h1 *to_h1; /* Converter to HTTP/1.1 format*/
    int started;            /* request line serialized */
    
    int chunked;
    apr_size_t remain_len;

    /* pseudo header values, see ch. 8.1.2.3 */
    const char *method;
    const char *path;
    const char *authority;
    const char *scheme;
};

h2_request *h2_request_create(int id, apr_pool_t *pool, 
                              struct h2_bucket_queue *bq);
void h2_request_destroy(h2_request *req);

apr_status_t h2_request_flush(h2_request *req);

apr_status_t h2_request_write_header(h2_request *req,
                                     const char *name, size_t nlen,
                                     const char *value, size_t vlen);

apr_status_t h2_request_write_data(h2_request *request,
                                   const char *data, size_t len);

apr_status_t h2_request_end_headers(h2_request *req);

apr_status_t h2_request_close(h2_request *req);

apr_status_t h2_request_rwrite(h2_request *req, request_rec *r);

#endif /* defined(__mod_h2__h2_request__) */
