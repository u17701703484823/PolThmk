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

#ifndef __mod_h2__h2_task_queue__
#define __mod_h2__h2_task_queue__

struct h2_task;

typedef struct h2_task_queue h2_task_queue;

struct h2_task_queue {
    APR_RING_ENTRY(h2_task_queue) link;
    APR_RING_HEAD(h2_tasks, h2_task) tasks;
    long id;
};

h2_task_queue *h2_tq_create(long id, apr_pool_t *pool);

void h2_tq_destroy(h2_task_queue *q);

int h2_tq_empty(h2_task_queue *q);

void h2_tq_add(h2_task_queue *q, struct h2_task *task);

h2_task *h2_tq_pop(h2_task_queue *q);

/*******************************************************************************
 * Queue Manipulation.
 ******************************************************************************/

/**
 * The magic pointer value that indicates the head of a h2_task_queue list
 * @param  b The queue list
 * @return The magic pointer value
 */
#define H2_TQ_LIST_SENTINEL(b)	APR_RING_SENTINEL((b), h2_task_queue, link)

/**
 * Determine if the queue list is empty
 * @param b The list to check
 * @return true or false
 */
#define H2_TQ_LIST_EMPTY(b)	APR_RING_EMPTY((b), h2_task_queue, link)

/**
 * Return the first queue in a list
 * @param b The list to query
 * @return The first queue in the list
 */
#define H2_TQ_LIST_FIRST(b)	APR_RING_FIRST(b)

/**
 * Return the last queue in a list
 * @param b The list to query
 * @return The last queue int he list
 */
#define H2_TQ_LIST_LAST(b)	APR_RING_LAST(b)

/**
 * Insert a single queue at the front of a list
 * @param b The list to add to
 * @param e The queue to insert
 */
#define H2_TQ_LIST_INSERT_HEAD(b, e) do {				\
h2_task_queue *ap__b = (e);                                        \
APR_RING_INSERT_HEAD((b), ap__b, h2_task_queue, link);	\
} while (0)

/**
 * Insert a single queue at the end of a list
 * @param b The list to add to
 * @param e The queue to insert
 */
#define H2_TQ_LIST_INSERT_TAIL(b, e) do {				\
h2_task_queue *ap__b = (e);					\
APR_RING_INSERT_TAIL((b), ap__b, h2_task_queue, link);	\
} while (0)

/**
 * Get the next queue in the list
 * @param e The current queue
 * @return The next queue
 */
#define H2_TQ_NEXT(e)	APR_RING_NEXT((e), link)
/**
 * Get the previous queue in the list
 * @param e The current queue
 * @return The previous queue
 */
#define H2_TQ_PREV(e)	APR_RING_PREV((e), link)

/**
 * Remove a queue from its list
 * @param e The queue to remove
 */
#define H2_TQ_REMOVE(e)	APR_RING_REMOVE((e), link)


#define H2_TQ_EMPTY(e)	H2_TASK_LIST_EMPTY(&(e)->tasks)

#endif /* defined(__mod_h2__h2_task_queue__) */
