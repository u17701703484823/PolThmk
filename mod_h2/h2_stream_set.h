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

#ifndef __mod_h2__h2_stream_set__
#define __mod_h2__h2_stream_set__

/**
 * A set of h2_stream instances. Allows lookup by stream id
 * and other criteria.
 */

typedef h2_stream *h2_stream_set_match_fn(void *ctx, h2_stream *stream);
typedef int h2_stream_set_iter_fn(void *ctx, h2_stream *stream);

typedef struct h2_stream_set h2_stream_set;


h2_stream_set *h2_stream_set_create(apr_pool_t *pool);

void h2_stream_set_destroy(h2_stream_set *sp);

void h2_stream_set_term(h2_stream_set *sp);

apr_status_t h2_stream_set_add(h2_stream_set *sp, h2_stream *stream);

h2_stream *h2_stream_set_get(h2_stream_set *sp, int stream_id);

h2_stream *h2_stream_set_remove(h2_stream_set *sp,h2_stream *stream);

void h2_stream_set_remove_all(h2_stream_set *sp);

void h2_stream_set_destroy_all(h2_stream_set *sp);

int h2_stream_set_is_empty(h2_stream_set *sp);

apr_size_t h2_stream_set_size(h2_stream_set *sp);

h2_stream *h2_stream_set_find(h2_stream_set *sp,
                              h2_stream_set_match_fn *match, void *ctx);

void h2_stream_set_iter(h2_stream_set *sp,
                        h2_stream_set_iter_fn *iter, void *ctx);

#endif /* defined(__mod_h2__h2_stream_set__) */
