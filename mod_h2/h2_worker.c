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

#include <apr_thread_cond.h>

#include <httpd.h>
#include <http_core.h>
#include <http_log.h>

#include "h2_private.h"
#include "h2_mplx.h"
#include "h2_task.h"
#include "h2_worker.h"

static void *execute(apr_thread_t *thread, void *wctx)
{
    h2_worker *worker = (h2_worker *)wctx;
    apr_status_t status = APR_SUCCESS;
    const int n = 1000000;
    (void)thread;
    
    /* Furthermore, other code might want to see the socket for
     * this connection. Allocate one without further function...
     */
    status = apr_socket_create(&worker->socket,
                               APR_INET, SOCK_STREAM,
                               APR_PROTO_TCP, worker->pool);
    if (status != APR_SUCCESS) {
        ap_log_perror(APLOG_MARK, APLOG_ERR, status, worker->pool,
                      "h2_worker(%d): alloc socket", worker->id);
        worker->worker_done(worker, worker->ctx);
        return NULL;
    }
    
    worker->task = NULL;
    worker->current = NULL;
    while (!worker->aborted) {
        
        if (worker->current) {
            status = APR_SUCCESS;
            worker->task = h2_mplx_pop_task(worker->current);
            for (int i = 0; !worker->aborted && worker->task; ++i) {
                
                h2_task_do(worker->task, worker);
                
                apr_thread_cond_signal(h2_worker_get_cond(worker));
                
                if (i >= n) {
                    /* Do a maximum of n tasks per given mplx before giving
                     * other connections a chance. */
                    worker->task = NULL;
                    status = APR_EAGAIN;
                    break;
                }
                worker->task = h2_mplx_pop_task(worker->current);
            }
            
            worker->task = NULL;
            worker->current = worker->mplx_done(worker, worker->current,
                                                status, worker->ctx);
        }
        
        if (!worker->current) {
            status = worker->get_next(worker, &worker->current, worker->ctx);
        }
    }

    if (worker->socket) {
        apr_socket_close(worker->socket);
        worker->socket = NULL;
    }
    
    worker->worker_done(worker, worker->ctx);
    return NULL;
}

h2_worker *h2_worker_create(int id,
                            apr_pool_t *parent_pool,
                            apr_threadattr_t *attr,
                            h2_worker_mplx_next_fn *get_next,
                            h2_worker_mplx_done_fn *mplx_done,
                            h2_worker_done_fn *worker_done,
                            void *ctx)
{
    apr_allocator_t *allocator = NULL;
    apr_pool_t *pool = NULL;
    
    apr_status_t status = apr_allocator_create(&allocator);
    if (status != APR_SUCCESS) {
        return NULL;
    }
    
    status = apr_pool_create_ex(&pool, parent_pool, NULL, allocator);
    if (status != APR_SUCCESS) {
        return NULL;
    }
    
    h2_worker *w = apr_pcalloc(pool, sizeof(h2_worker));
    if (w) {
        APR_RING_ELEM_INIT(w, link);
        
        w->id = id;
        w->pool = pool;
        w->bucket_alloc = apr_bucket_alloc_create(pool);

        w->get_next = get_next;
        w->mplx_done = mplx_done;
        w->worker_done = worker_done;
        w->ctx = ctx;
        
        status = apr_thread_cond_create(&w->io, w->pool);
        if (status != APR_SUCCESS) {
            return NULL;
        }
        
        apr_thread_create(&w->thread, attr, execute, w, pool);
    }
    return w;
}

apr_status_t h2_worker_destroy(h2_worker *worker)
{
    if (worker->io) {
        apr_thread_cond_destroy(worker->io);
        worker->io = NULL;
    }
    if (worker->pool) {
        apr_allocator_t *allocator = apr_pool_allocator_get(worker->pool);
        apr_pool_destroy(worker->pool);
        /* worker is gone */
        if (allocator) {
            apr_allocator_destroy(allocator);
        }
    }
    return APR_SUCCESS;
}

int h2_worker_get_id(h2_worker *worker)
{
    return worker->id;
}

void h2_worker_abort(h2_worker *worker)
{
    worker->aborted = 1;
}

int h2_worker_is_aborted(h2_worker *worker)
{
    return worker->aborted;
}

apr_thread_t *h2_worker_get_thread(h2_worker *worker)
{
    return worker->thread;
}

apr_thread_cond_t *h2_worker_get_cond(h2_worker *worker)
{
    return worker->io;
}

apr_socket_t *h2_worker_get_socket(h2_worker *worker)
{
    return worker->socket;
}

apr_pool_t *h2_worker_get_pool(h2_worker *worker)
{
    return worker->pool;
}

apr_bucket_alloc_t *h2_worker_get_bucket_alloc(h2_worker *worker)
{
    return worker->bucket_alloc;
}

