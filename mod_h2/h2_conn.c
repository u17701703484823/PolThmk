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

#include <assert.h>

#include <ap_mpm.h>

#include <httpd.h>
#include <http_core.h>
#include <http_config.h>
#include <http_log.h>
#include <http_connection.h>
#include <http_protocol.h>
#include <http_request.h>

#include "h2_private.h"
#include "h2_config.h"
#include "h2_ctx.h"
#include "h2_session.h"
#include "h2_stream.h"
#include "h2_stream_set.h"
#include "h2_task.h"
#include "h2_workers.h"
#include "h2_conn.h"

static struct h2_workers *workers;

static apr_status_t h2_session_process(h2_session *session);
static void after_stream_opened_cb(h2_session *session,
                                h2_stream *stream, h2_task *task);
static apr_status_t before_stream_close_cb(h2_session *session,
                                           h2_stream *stream, h2_task *task,
                                           int wait);

static h2_mpm_type_t mpm_type = H2_MPM_UNKNOWN;
static module *mpm_module = NULL;

apr_status_t h2_conn_child_init(apr_pool_t *pool, server_rec *s)
{
    h2_config *config = h2_config_sget(s);
    apr_status_t status = APR_SUCCESS;
    int minw = h2_config_geti(config, H2_CONF_MIN_WORKERS);
    int maxw = h2_config_geti(config, H2_CONF_MAX_WORKERS);
    
    int max_threads_per_child = 0;
    ap_mpm_query(AP_MPMQ_MAX_THREADS, &max_threads_per_child);
    int threads_limit = 0;
    ap_mpm_query(AP_MPMQ_HARD_LIMIT_THREADS, &threads_limit);
    
    for (int i = 0; ap_loaded_modules[i]; ++i) {
        module *m = ap_loaded_modules[i];
        if (!strcmp("event.c", m->name)) {
            mpm_type = H2_MPM_EVENT;
            mpm_module = m;
        }
        else if (!strcmp("worker.c", m->name)) {
            mpm_type = H2_MPM_WORKER;
            mpm_module = m;
        }
    }
    
    ap_log_error(APLOG_MARK, APLOG_INFO, 0, s,
                 "h2_conn: child init with conf[%s]: "
                 "min_workers=%d, max_workers=%d, "
                 "mpm-threads=%d, mpm-threads-limit=%d, "
                 "mpm-type=%d(%s)",
                 config->name, config->min_workers, config->max_workers,
                 max_threads_per_child, threads_limit, mpm_type,
                 mpm_module? mpm_module->name : "unknown");
    
    if (minw <= 0) {
        minw = max_threads_per_child / 2;
    }
    if (maxw <= 0) {
        maxw = threads_limit / 2;
        if (maxw < minw) {
            maxw = minw;
        }
    }
    workers = h2_workers_create(s, pool, minw, maxw);
    h2_workers_set_max_idle_secs(
        workers, h2_config_geti(config, H2_CONF_MAX_WORKER_IDLE_SECS));
    return status;
}

h2_mpm_type_t h2_conn_mpm_type() {
    return mpm_type;
}

module *h2_conn_mpm_module() {
    return mpm_module;
}

apr_status_t h2_conn_rprocess(request_rec *r)
{
    h2_config *config = h2_config_rget(r);
    
    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, r, "h2_conn_process start");
    if (!workers) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "workers not initialized");
        return APR_EGENERAL;
    }
    
    h2_session *session = h2_session_rcreate(r, config);
    if (!session) {
        return APR_EGENERAL;
    }
    
    return h2_session_process(session);
}

apr_status_t h2_conn_main(conn_rec *c)
{
    h2_config *config = h2_config_get(c);
    
    ap_log_cerror(APLOG_MARK, APLOG_DEBUG, 0, c, "h2_conn_main start");
    if (!workers) {
        ap_log_cerror(APLOG_MARK, APLOG_ERR, 0, c, "workers not initialized");
        return APR_EGENERAL;
    }
    
    h2_session *session = h2_session_create(c, config);
    if (!session) {
        return APR_EGENERAL;
    }
    
    return h2_session_process(session);
}

apr_status_t h2_session_process(h2_session *session)
{
    apr_status_t status = APR_SUCCESS;
    /* Start talking to the client. Apart from protocol meta data,
     * we mainly will see new http/2 streams opened by the client, which
     * basically are http requests we need to dispatch.
     *
     * There will be bursts of new streams, to be served concurrently,
     * followed by long pauses of no activity.
     *
     * Since the purpose of http/2 is to allow siumultaneous streams, we
     * need to dispatch the handling of each stream into a separate worker
     * thread, keeping this thread open for sending responses back as
     * soon as they arrive.
     * At the same time, we need to continue reading new frames from
     * our client, which may be meta (WINDOWS_UPDATEs, PING, SETTINGS) or
     * new streams.
     *
     * As long as we have streams open in this session, we cannot really rest
     * since there are two conditions to wait on: 1. new data from the client,
     * 2. new data from the open streams to send back.
     *
     * Only when we have no more streams open, can we do a blocking read
     * on our connection.
     *
     * TODO: implement graceful GO_AWAY after configurable idle time
     */
    
    /* Once we have our own. remove the standard timeout handling */
    /* ap_remove_input_filter_byhandle(session->c->input_filters, "reqtimeout"); */

    h2_session_set_stream_open_cb(session, after_stream_opened_cb);
    h2_session_set_stream_close_cb(session, before_stream_close_cb);
    
    status = h2_session_start(session);
    if (status != APR_SUCCESS) {
        ap_log_cerror(APLOG_MARK, APLOG_ERR, status, session->c,
                      "h2_session(%ld): startup", session->id);
        h2_session_destroy(session);
        return status;
    }
    
    apr_interval_time_t wait_micros = 0;
    static const int MAX_WAIT_MICROS = 200 * 1000;
    
    while (!h2_session_is_done(session)) {
        int have_written = 0;
        int have_read = 0;
        
        status = h2_session_write(session, wait_micros);
        if (status == APR_SUCCESS) {
            have_written = 1;
            wait_micros = 0;
        }
        else if (status == APR_EAGAIN) {
            /* nop */
        }
        else if (status == APR_TIMEUP) {
            wait_micros *= 2;
            if (wait_micros > MAX_WAIT_MICROS) {
                wait_micros = MAX_WAIT_MICROS;
            }
            ap_log_cerror( APLOG_MARK, APLOG_TRACE1, status, session->c,
                          "timeout waiting %f ms", wait_micros/1000.0);
        }
        else {
            ap_log_cerror( APLOG_MARK, APLOG_WARNING, status, session->c,
                          "h2_session(%ld): error writing, terminating",
                          session->id);
            h2_session_abort(session, status);
            break;
        }

        int got_streams = !h2_stream_set_is_empty(session->streams);
        status = h2_session_read(session, got_streams?
                                 APR_NONBLOCK_READ : APR_BLOCK_READ);
        switch (status) {
            case APR_SUCCESS:
                /* successful read, reset our idle timers */
                have_read = 1;
                wait_micros = 0;
                break;
            case APR_EAGAIN:
                break;
            case APR_EOF:
            case APR_ECONNABORTED:
                ap_log_cerror( APLOG_MARK, APLOG_INFO, status, session->c,
                              "h2_session(%ld): reading",
                              session->id);
                h2_session_abort(session, status);
                break;
            default:
                ap_log_cerror( APLOG_MARK, APLOG_WARNING, status, session->c,
                              "h2_session(%ld): error reading, terminating",
                              session->id);
                h2_session_abort(session, status);
                break;
        }
        
        if (!have_read && !have_written) {
            /* Nothing to read or write, we may have sessions, but
             * the have no data yet ready to be delivered. Slowly
             * back off to give others a chance to do their work.
             */
            if (wait_micros == 0) {
                wait_micros = 10;
            }
        }
    }
    
    ap_log_cerror( APLOG_MARK, APLOG_DEBUG, status, session->c,
                  "h2_session(%ld): done", session->id);
    
    h2_session_close(session);
    h2_session_destroy(session);
    
    return DONE;
}

static void after_stream_opened_cb(h2_session *session,
                                h2_stream *stream, h2_task *task)
{
    apr_status_t status = h2_workers_schedule(workers, task);
    if (status != APR_SUCCESS) {
        ap_log_cerror(APLOG_MARK, APLOG_ERR, status, session->c,
                      "scheduling task(%ld-%d)",
                      session->id, stream->id);
    }
}

static apr_status_t before_stream_close_cb(h2_session *session,
                                           h2_stream *stream, h2_task *task,
                                           int wait)
{
    apr_status_t status = APR_SUCCESS;
    if (task) {
        h2_task_abort(task);
        status = h2_workers_join(workers, task, wait);
        if (status != APR_SUCCESS && status != APR_EAGAIN) {
            ap_log_cerror( APLOG_MARK, APLOG_WARNING, status, session->c,
                          "h2_session: error join task(%s)",
                          h2_task_get_id(task));
        }
    }
    return status;
}


static void fix_event_conn(conn_rec *c, conn_rec *master);

h2_conn *h2_conn_create(const char *id, conn_rec *master, apr_pool_t *parent)
{
    assert(master);
    apr_status_t status = APR_SUCCESS;
    apr_pool_t *pool;
    
    status = apr_pool_create(&pool, parent);
    if (status == APR_SUCCESS) {
        /* Setup a apache connection record for this stream.
         * General idea is borrowed from mod_spdy::slave_connection.cc,
         * partly replaced with some more modern calls to ap infrastructure.
         *
         * Here, we are tasting some sweet, internal knowledge, e.g. that
         * the core module is storing the connection socket as its config.
         * "ap_run_create_connection() needs a real socket as it tries to
         * detect local and client address information and fails if it is
         * unable to get it.
         * In case someone ever replaces these core hooks, this will probably
         * break miserably.
         */
        h2_conn *conn = apr_pcalloc(pool, sizeof(*conn));
        conn->id = id;
        conn->pool = pool;
        apr_pool_tag(pool, id);

        conn->bucket_alloc = apr_bucket_alloc_create(conn->pool);
        conn->socket = ap_get_module_config(master->conn_config,
                                            &core_module);
        conn->master = master;
        
        /* Not sure about the scoreboard handle. Reusing the one from the main
         * connection could make sense, but I do not know enough to tell...
         */
        conn->c = ap_run_create_connection(conn->pool, conn->master->base_server,
                                           conn->socket,
                                           conn->master->id^((long)conn->pool), 
                                           conn->master->sbh,
                                           conn->bucket_alloc);
        if (conn->c == NULL) {
            ap_log_perror(APLOG_MARK, APLOG_ERR, APR_ENOMEM, conn->pool,
                          "h2_task: creating conn");
            return NULL;
        }
        
        return conn;
    }
    return NULL;
}

void h2_conn_destroy(h2_conn *conn)
{
    if (conn->c) {
        /* more to do? */
        conn->c = NULL;
    }
    if (conn->pool) {
        apr_pool_destroy(conn->pool);
        /* conn is gone */
    }
}

apr_status_t h2_conn_prep(h2_conn *conn, apr_thread_t *thd)
{
    assert(conn);
   
    /* This works for mpm_worker so far. Other mpm modules have 
     * different needs, unfortunately. The most interesting one 
     * being mpm_event...
     */
    switch (h2_conn_mpm_type()) {
        case H2_MPM_WORKER:
            /* all fine */
            break;
        case H2_MPM_EVENT: 
            fix_event_conn(conn->c, conn->master);
            break;
        default:
            /* fingers crossed */
            break;
    }
    
    ap_log_perror(APLOG_MARK, APLOG_TRACE3, 0, conn->pool,
                  "h2_conn(%s): created from master %ld",
                  conn->id, conn->master->id);
    
    conn->c->current_thread = thd;
    
    /* Furthermore, other code might want to see the socket for
     * this connection. Allocate one without further function...
     */
    apr_status_t status = apr_socket_create(&conn->socket,
                                            APR_INET, SOCK_STREAM,
                                            APR_PROTO_TCP, conn->pool);
    if (status != APR_SUCCESS) {
        ap_log_cerror(APLOG_MARK, APLOG_ERR, status, conn->c,
                      "h2_conn(%s): alloc socket", conn->id);
        return status;
    }
    
    ap_set_module_config(conn->c->conn_config, &core_module, 
                         conn->socket);
    return status;
}

apr_status_t h2_conn_process(h2_conn *conn)
{
    assert(conn);
    assert(conn->c);
    
    ap_process_connection(conn->c, conn->socket);
    
    if (conn->socket) {
        apr_socket_close(conn->socket);
        conn->socket = NULL;
    }
    
    return APR_SUCCESS;
}

#define H2_EVENT_HACK   1

#if H2_EVENT_HACK

/* This is an internal mpm event.c struct which is disguised
 * as a conn_state_t so that mpm_event can have special connection
 * state information without changing the struct seen on the outside.
 *
 * For our task connections we need to create a new beast of this type
 * and fill it with enough meaningful things that mpm_event reads and
 * starts processing out task request.
 */
typedef struct event_conn_state_t event_conn_state_t;
struct event_conn_state_t {
    /** APR_RING of expiration timeouts */
    APR_RING_ENTRY(event_conn_state_t) timeout_list;
    /** the expiration time of the next keepalive timeout */
    apr_time_t expiration_time;
    /** connection record this struct refers to */
    conn_rec *c;
    /** request record (if any) this struct refers to */
    request_rec *r;
    /** is the current conn_rec suspended?  (disassociated with
     * a particular MPM thread; for suspend_/resume_connection
     * hooks)
     */
    int suspended;
    /** memory pool to allocate from */
    apr_pool_t *p;
    /** bucket allocator */
    apr_bucket_alloc_t *bucket_alloc;
    /** poll file descriptor information */
    apr_pollfd_t pfd;
    /** public parts of the connection state */
    conn_state_t pub;
};
APR_RING_HEAD(timeout_head_t, event_conn_state_t);

static void fix_event_conn(conn_rec *c, conn_rec *master) 
{
    event_conn_state_t *master_cs = ap_get_module_config(master->conn_config, 
                                                         h2_conn_mpm_module());
    event_conn_state_t *cs = apr_pcalloc(c->pool, sizeof(event_conn_state_t));
    cs->bucket_alloc = apr_bucket_alloc_create(c->pool);
    
    ap_set_module_config(c->conn_config, h2_conn_mpm_module(), cs);
    
    cs->c = c;
    cs->r = NULL;
    cs->p = master_cs->p;
    cs->pfd = master_cs->pfd;
    cs->pub = master_cs->pub;
    cs->pub.state = CONN_STATE_READ_REQUEST_LINE;
    
    c->cs = &(cs->pub);
}

#else /*if H2_EVENT_HACK */

static int warned = 0;
static void fix_event_conn(conn_rec *c, conn_rec *master) 
{
    if (!warned) {
        ap_log_cerror(APLOG_MARK, APLOG_WARNING, 0, master,
                      "running mod_h2 in an unhacked mpm_event server "
                      "will most likely not work");
        warned = 1;
    }
}

#endif /*if H2_EVENT_HACK */
