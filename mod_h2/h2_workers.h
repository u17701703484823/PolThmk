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

#ifndef __mod_h2__h2_workers__
#define __mod_h2__h2_workers__

/* Thread pool specific to executing h2_tasks. Has a minimum and maximum 
 * number of workers it creates. Starts with minimum workers and adds
 * some on load, reduces the number again when idle.
 *
 */
struct apr_thread_mutex_t;
struct apr_thread_cond_t;
struct h2_mplx;
struct h2_task;
struct h2_task_queue;

typedef struct h2_workers h2_workers;

struct h2_workers {
    server_rec *s;
    apr_pool_t *pool;
    int aborted;
    
    int next_worker_id;
    int min_size;
    int max_size;
    
    apr_threadattr_t *thread_attr;
    
    APR_RING_HEAD(h2_worker_list, h2_worker) workers;
    APR_RING_HEAD(h2_mplx_list, h2_mplx) mplxs;
    
    int worker_count;
    volatile apr_uint32_t max_idle_secs;
    volatile apr_uint32_t idle_worker_count;
    
    struct apr_thread_mutex_t *lock;
    struct apr_thread_cond_t *mplx_added;
};


/* Create a worker pool with the given minimum and maximum number of
 * threads.
 */
h2_workers *h2_workers_create(server_rec *s, apr_pool_t *pool,
                              int min_size, int max_size);

/* Destroy the worker pool and all its threads. 
 */
void h2_workers_destroy(h2_workers *workers);

/**
 * Registers a h2_mplx for task scheduling. If this h2_mplx runs
 * out of tasks, it will be automatically be unregistered. Should
 * new tasks arrive, it needs to be registered again.
 */
apr_status_t h2_workers_register(h2_workers *workers, 
                                 struct h2_mplx *m, int single_task);

/**
 * Remove a h2_mplx from the worker registry.
 */
apr_status_t h2_workers_unregister(h2_workers *workers, 
                                   struct h2_mplx *m);

/**
 * Set the amount of seconds a h2_worker should wait for new tasks
 * before shutting down (if there are more than the minimum number of
 * workers).
 */
void h2_workers_set_max_idle_secs(h2_workers *workers, int idle_secs);

#endif /* defined(__mod_h2__h2_workers__) */
