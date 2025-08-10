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

#ifndef __mod_h2__h2_c1_io__
#define __mod_h2__h2_c1_io__

struct h2_config;
struct h2_session;

/* h2_io is the basic handler of a httpd connection. It keeps two brigades,
 * one for input, one for output and works with the installed connection
 * filters.
 * The read is done via a callback function, so that input can be processed
 * directly without copying.
 */
typedef struct {
    conn_rec *c;
    apr_bucket_brigade *output;

    int is_tls;
    apr_time_t cooldown_usecs;
    apr_int64_t warmup_size;
    
    apr_size_t write_size;
    apr_time_t last_write;
    apr_int64_t bytes_read;
    apr_int64_t bytes_written;
    
    int buffer_output;
    apr_size_t flush_threshold;
    unsigned int is_flushed : 1;
    
    char *scratch;
    apr_size_t ssize;
    apr_size_t slen;
} h2_c1_io;

apr_status_t h2_c1_io_init(h2_c1_io *io, conn_rec *c, server_rec *s);

/**
 * Append data to the buffered output.
 * @param buf the data to append
 * @param length the length of the data to append
 */
apr_status_t h2_c1_io_write(h2_c1_io *io,
                         const char *buf,
                         size_t length);

apr_status_t h2_c1_io_pass(h2_c1_io *io, apr_bucket_brigade *bb);

/**
 * Pass any buffered data on to the connection output filters.
 * @param io the connection io
 * @param flush if a flush bucket should be appended to any output
 */
apr_status_t h2_c1_io_flush(h2_c1_io *io);

/**
 * Check if the buffered amount of data needs flushing.
 */
int h2_c1_io_needs_flush(h2_c1_io *io);

struct h2_session;

typedef struct h2_c1_filter_ctx_t h2_c1_filter_ctx_t;

h2_c1_filter_ctx_t *h2_c1_filter_ctx_t_create(struct h2_session *session);

apr_status_t h2_c1_filter_input(ap_filter_t* filter,
                                apr_bucket_brigade* brigade,
                                ap_input_mode_t mode,
                                apr_read_type_e block,
                                apr_off_t readbytes);

#endif /* defined(__mod_h2__h2_c1_io__) */
