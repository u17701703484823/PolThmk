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

#ifndef __mod_h2__h2_mplx__
#define __mod_h2__h2_mplx__

/**
 * The stream multiplexer. It pushes buckets from the connection
 * thread to the stream task threads and vice versa. It's thread-safe
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
 * Writing input is never blocked. In order to use flow control on the input,
 * the mplx can be polled for input data consumption.
 */

struct apr_pool_t;
struct apr_thread_mutex_t;
struct apr_thread_cond_t;
struct h2_config;
struct h2_response;
struct h2_task;
struct h2_io_set;
struct apr_thread_cond_t;
struct h2_workers;
struct h2_stream_set;
struct h2_task_queue;

#include "h2_io.h"

typedef struct h2_mplx h2_mplx;

struct h2_mplx {
    long id;
    APR_RING_ENTRY(h2_mplx) link;
    volatile apr_uint32_t refs;
    conn_rec *c;
    apr_pool_t *pool;
    apr_bucket_alloc_t *bucket_alloc;

    struct h2_task_queue *q;
    struct h2_io_set *stream_ios;
    struct h2_io_set *ready_ios;
    
    apr_thread_mutex_t *lock;
    struct apr_thread_cond_t *added_output;
    
    int aborted;
    apr_size_t stream_max_mem;
    
    struct h2_workers *workers;
};

/*******************************************************************************
 * Object lifecycle and information.
 ******************************************************************************/

/**
 * Create the multiplexer for the given HTTP2 session.
 */
h2_mplx *h2_mplx_create(conn_rec *c, apr_pool_t *master, 
                        struct h2_workers *workers);

void h2_mplx_release(h2_mplx *m);
void h2_mplx_reference(h2_mplx *m);

/**
 * Get the id of the multiplexer, same as the session id it belongs to.
 */
long h2_mplx_get_id(h2_mplx *mplx);

/**
 * Get the memory pool used by the multiplexer itself.
 */
apr_pool_t *h2_mplx_get_pool(h2_mplx *mplx);

/**
 * Get the main connection this multiplexer works for.
 */
conn_rec *h2_mplx_get_conn(h2_mplx *mplx);

/**
 * Aborts the multiplexer. It will answer all future invocation with
 * APR_ECONNABORTED, leading to early termination of ongoing tasks.
 */
void h2_mplx_abort(h2_mplx *mplx);

/*******************************************************************************
 * IO lifetime of streams.
 ******************************************************************************/
/**
 * Prepares the multiplexer to handle in-/output on the given stream id.
 */
apr_status_t h2_mplx_open_io(h2_mplx *mplx, int stream_id);

/**
 * Ends handling of in-/ouput on the given stream id.
 */
void h2_mplx_close_io(h2_mplx *mplx, int stream_id);

/* Return != 0 iff the multiplexer has data for the given stream. 
 */
int h2_mplx_out_has_data_for(h2_mplx *m, int stream_id);

/**
 * Waits on output data from any stream in this session to become available. 
 * Returns APR_TIMEUP if no data arrived in the given time.
 */
apr_status_t h2_mplx_out_trywait(h2_mplx *m, apr_interval_time_t timeout,
                                 struct apr_thread_cond_t *iowait);

apr_status_t h2_mplx_join_task(h2_mplx *m, struct h2_task *task, int wait);

/*******************************************************************************
 * Stream processing.
 ******************************************************************************/

/**
 * Perform the task on the given stream.
 */
apr_status_t h2_mplx_do_task(h2_mplx *mplx, struct h2_task *task);

struct h2_task *h2_mplx_pop_task(h2_mplx *mplx);

/*******************************************************************************
 * Input handling of streams.
 ******************************************************************************/

/**
 * Reads a buckets for the given stream_id. Will return ARP_EAGAIN when
 * called with APR_NONBLOCK_READ and no data present. Will return APR_EOF
 * when the end of the stream input has been reached.
 * The condition passed in will be used for blocking/signalling and will
 * be protected by the mplx's own mutex.
 */
apr_status_t h2_mplx_in_read(h2_mplx *m, apr_read_type_e block,
                             int stream_id, apr_bucket_brigade *bb,
                             struct apr_thread_cond_t *iowait);

/**
 * Appends data to the input of the given stream. Storage of input data is
 * not subject to flow control.
 */
apr_status_t h2_mplx_in_write(h2_mplx *mplx, int stream_id, 
                              apr_bucket_brigade *bb);

/**
 * Closes the input for the given stream_id.
 */
apr_status_t h2_mplx_in_close(h2_mplx *m, int stream_id);

/**
 * Returns != 0 iff the input for the given stream has been closed. There
 * could still be data queued, but it can be read without blocking.
 */
int h2_mplx_in_has_eos_for(h2_mplx *m, int stream_id);

/**
 * Callback invoked for every stream that had input data read since
 * the last invocation.
 */
typedef void h2_mplx_consumed_cb(void *ctx, int stream_id, apr_size_t consumed);

/**
 * Invoke the callback for all streams that had bytes read since the last
 * call to this function. If no stream had input data consumed, the callback
 * is not invoked.
 * Returns APR_SUCCESS when an update happened, APR_EAGAIN if no update
 * happened.
 */
apr_status_t h2_mplx_in_update_windows(h2_mplx *m, 
                                       h2_mplx_consumed_cb *cb, void *ctx);

/*******************************************************************************
 * Output handling of streams.
 ******************************************************************************/

/**
 * Get a stream whose response is ready for submit. Will set response and
 * any out data available in stream. 
 * @param m the mplxer to get a response from
 * @param bb the brigade to place any existing repsonse body data into
 */
struct h2_stream *h2_mplx_next_submit(h2_mplx *m, 
                                      struct h2_stream_set *streams);

/**
 * Reads output data from the given stream. Will never block, but
 * return APR_EAGAIN until data arrives or the stream is closed.
 */
apr_status_t h2_mplx_out_read(h2_mplx *mplx, int stream_id, 
                              char *buffer, apr_size_t *plen, int *peos);

apr_status_t h2_mplx_out_readx(h2_mplx *mplx, int stream_id, 
                               h2_io_data_cb *cb, void *ctx, 
                               apr_size_t *plen, int *peos);

/**
 * Opens the output for the given stream with the specified response.
 */
apr_status_t h2_mplx_out_open(h2_mplx *mplx, int stream_id,
                              struct h2_response *response,
                              ap_filter_t* filter, apr_bucket_brigade *bb,
                              struct apr_thread_cond_t *iowait);

/**
 * Append the brigade to the stream output. Might block if amount
 * of bytes buffered reaches configured max.
 * @param stream_id the stream identifier
 * @param filter the apache filter context of the data
 * @param bb the bucket brigade to append
 * @param iowait a conditional used for block/signalling in h2_mplx
 */
apr_status_t h2_mplx_out_write(h2_mplx *mplx, int stream_id, 
                               ap_filter_t* filter, apr_bucket_brigade *bb,
                               struct apr_thread_cond_t *iowait);

/**
 * Closes the output stream. Readers of this stream will get all pending 
 * data and then only APR_EOF as result. 
 */
apr_status_t h2_mplx_out_close(h2_mplx *m, int stream_id);

/*******************************************************************************
 * h2_mplx list Manipulation.
 ******************************************************************************/

/**
 * The magic pointer value that indicates the head of a h2_mplx list
 * @param  b The mplx list
 * @return The magic pointer value
 */
#define H2_MPLX_LIST_SENTINEL(b)	APR_RING_SENTINEL((b), h2_mplx, link)

/**
 * Determine if the mplx list is empty
 * @param b The list to check
 * @return true or false
 */
#define H2_MPLX_LIST_EMPTY(b)	APR_RING_EMPTY((b), h2_mplx, link)

/**
 * Return the first mplx in a list
 * @param b The list to query
 * @return The first mplx in the list
 */
#define H2_MPLX_LIST_FIRST(b)	APR_RING_FIRST(b)

/**
 * Return the last mplx in a list
 * @param b The list to query
 * @return The last mplx int he list
 */
#define H2_MPLX_LIST_LAST(b)	APR_RING_LAST(b)

/**
 * Insert a single mplx at the front of a list
 * @param b The list to add to
 * @param e The mplx to insert
 */
#define H2_MPLX_LIST_INSERT_HEAD(b, e) do {				\
h2_mplx *ap__b = (e);                                        \
APR_RING_INSERT_HEAD((b), ap__b, h2_mplx, link);	\
} while (0)

/**
 * Insert a single mplx at the end of a list
 * @param b The list to add to
 * @param e The mplx to insert
 */
#define H2_MPLX_LIST_INSERT_TAIL(b, e) do {				\
h2_mplx *ap__b = (e);					\
APR_RING_INSERT_TAIL((b), ap__b, h2_mplx, link);	\
} while (0)

/**
 * Get the next mplx in the list
 * @param e The current mplx
 * @return The next mplx
 */
#define H2_MPLX_NEXT(e)	APR_RING_NEXT((e), link)
/**
 * Get the previous mplx in the list
 * @param e The current mplx
 * @return The previous mplx
 */
#define H2_MPLX_PREV(e)	APR_RING_PREV((e), link)

/**
 * Remove a mplx from its list
 * @param e The mplx to remove
 */
#define H2_MPLX_REMOVE(e)	APR_RING_REMOVE((e), link)


#endif /* defined(__mod_h2__h2_mplx__) */
