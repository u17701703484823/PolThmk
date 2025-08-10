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

#ifndef __mod_h2__h2_mplx__
#define __mod_h2__h2_mplx__

/**
 * The stream multiplexer. It pushes buckets from the connection
 * thread to the stream threads and vice versa. It's thread-safe
 * to use.
 *
 * There is one h2_mplx instance for each h2_session, which sits on top
 * of a particular httpd conn_rec. Input goes from the connection to
 * the stream tasks. Output goes from the stream tasks to the connection,
 * e.g. the client.
 *
 * For each stream, there can be at most "H2StreamMaxMemSize" output bytes
 * queued in the multiplexer. If a task thread tries to write more
 * data, it is blocked until space becomes available.
 *
 * Naming Convention: 
 * "h2_mplx_m_" are methods only to be called by the main connection
 * "h2_mplx_s_" are method only to be called by a secondary connection
 * "h2_mplx_t_" are method only to be called by a task handler (can be master or secondary)
 */

struct apr_pool_t;
struct apr_thread_mutex_t;
struct apr_thread_cond_t;
struct h2_bucket_beam;
struct h2_config;
struct h2_ihash_t;
struct h2_stream;
struct h2_request;
struct apr_thread_cond_t;
struct h2_workers;
struct h2_iqueue;

#include <apr_queue.h>

typedef struct h2_mplx h2_mplx;

struct h2_mplx {
    long id;
    conn_rec *c;
    apr_pool_t *pool;
    struct h2_stream *stream0;      /* the main connection */
    server_rec *s;                  /* server for master conn */

    unsigned int event_pending;
    unsigned int aborted;
    unsigned int is_registered;     /* is registered at h2_workers */

    struct h2_ihash_t *streams;     /* all streams currently processing */
    struct h2_ihash_t *shold;       /* all streams done with task ongoing */
    struct h2_ihash_t *spurge;      /* all streams done, ready for destroy */
    
    struct h2_iqueue *q;            /* all stream ids that need to be started */
    struct h2_ififo *readyq;        /* all stream ids ready for output */
        
    int max_streams;        /* max # of concurrent streams */
    int max_stream_started; /* highest stream id that started processing */
    int streams_active;       /* # of tasks being processed from this mplx */
    int limit_active;       /* current limit on active tasks, dynamic */
    int max_active;         /* max, hard limit # of active tasks in a process */
    
    apr_time_t last_mood_change; /* last time, we worker limit changed */
    apr_interval_time_t mood_update_interval; /* how frequent we update at most */
    int irritations_since; /* irritations (>0) or happy events (<0) since last mood change */

    apr_thread_mutex_t *lock;
    struct apr_thread_cond_t *added_output;
    struct apr_thread_cond_t *join_wait;
    
    apr_size_t stream_max_mem;
    
    apr_pool_t *spare_io_pool;
    apr_array_header_t *spare_c2; /* spare secondary connections */

    apr_pollset_t *pollset;

    struct h2_workers *workers;
};

/*******************************************************************************
 * From the main connection processing: h2_mplx_m_*
 ******************************************************************************/

apr_status_t h2_mplx_m_child_init(apr_pool_t *pool, server_rec *s);

/**
 * Create the multiplexer for the given HTTP2 session. 
 * Implicitly has reference count 1.
 */
h2_mplx *h2_mplx_m_create(struct h2_stream *stream0, server_rec *s, apr_pool_t *master,
                          struct h2_workers *workers);

/**
 * Decreases the reference counter of this mplx and waits for it
 * to reached 0, destroy the mplx afterwards.
 * This is to be called from the thread that created the mplx in
 * the first place.
 * @param m the mplx to be released and destroyed
 * @param wait condition var to wait on for ref counter == 0
 */ 
void h2_mplx_m_release_and_join(h2_mplx *m, struct apr_thread_cond_t *wait);

/**
 * Shut down the multiplexer gracefully. Will no longer schedule new streams
 * but let the ongoing ones finish normally.
 * @return the highest stream id being/been processed
 */
int h2_mplx_m_shutdown(h2_mplx *m);

/**
 * Notifies mplx that a stream has been completely handled on the main
 * connection and is ready for cleanup.
 * 
 * @param m the mplx itself
 * @param stream the stream ready for cleanup
 */
apr_status_t h2_mplx_m_stream_cleanup(h2_mplx *m, struct h2_stream *stream);

apr_status_t h2_mplx_m_keep_active(h2_mplx *m, struct h2_stream *stream);

/**
 * Process a stream request.
 * 
 * @param m the multiplexer
 * @param read_to_process
 * @param input_pending
 * @param cmp the stream priority compare function
 */
apr_status_t h2_mplx_c1_process(h2_mplx *m,
                                struct h2_iqueue *read_to_process,
                                h2_stream_get_fn *get_stream,
                                h2_stream_pri_cmp_fn *cmp,
                                struct h2_session *session);

apr_status_t h2_mplx_c1_fwd_input(h2_mplx *m, struct h2_iqueue *input_pending,
                                  h2_stream_get_fn *get_stream,
                                  struct h2_session *session);


/**
 * Stream priorities have changed, reschedule pending requests.
 * 
 * @param m the multiplexer
 * @param cmp the stream priority compare function
 * @param ctx context data for the compare function
 */
apr_status_t h2_mplx_m_reprioritize(h2_mplx *m, h2_stream_pri_cmp_fn *cmp,
                                    struct h2_session *session);

typedef apr_status_t stream_ev_callback(void *ctx, struct h2_stream *stream);

/**
 * Poll the primary connection for input and the active streams for output.
 * Invoke the callback for any stream where an event happened.
 */
apr_status_t h2_mplx_c1_poll(h2_mplx *m, apr_interval_time_t timeout,
                            stream_ev_callback *on_stream_input,
                            stream_ev_callback *on_stream_output,
                            void *on_ctx);

int h2_mplx_m_awaits_data(h2_mplx *m);

typedef int h2_mplx_stream_cb(struct h2_stream *s, void *ctx);

apr_status_t h2_mplx_m_stream_do(h2_mplx *m, h2_mplx_stream_cb *cb, void *ctx);

apr_status_t h2_mplx_m_client_rst(h2_mplx *m, int stream_id);

/*******************************************************************************
 * From a secondary connection processing: h2_mplx_s_*
 ******************************************************************************/
apr_status_t h2_mplx_s_pop_c2(h2_mplx *m, conn_rec **out_c);
void h2_mplx_s_c2_done(conn_rec *c, conn_rec **out_c);

/**
 * Opens the output for a secondary (stream processing) connection to the mplx.
 */
apr_status_t h2_mplx_t_out_open(h2_mplx *mplx, conn_rec *c2);

/**
 * Get the stream that belongs to the given task.
 */
struct h2_stream *h2_mplx_t_stream_get(h2_mplx *m, int stream_id);


#endif /* defined(__mod_h2__h2_mplx__) */
