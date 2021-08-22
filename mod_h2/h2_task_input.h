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

#ifndef __mod_h2__h2_task_input__
#define __mod_h2__h2_task_input__

/* h2_task_input places the HEADER+DATA, formatted in HTTP/1.1, into
 * a bucket brigade. The brigade is setup as the input brigade for our
 * pseudo httpd conn_rec that is handling a specific h2_task.
 */
struct h2_bucket;
struct h2_mplx;

typedef struct h2_task_input h2_task_input;

h2_task_input *h2_task_input_create(apr_pool_t *pool,
                                    int session_id, int stream_id,
                                    struct h2_bucket *data, int eos,
                                    struct h2_mplx *mplx);
void h2_task_input_destroy(h2_task_input *input);

apr_status_t h2_task_input_read(h2_task_input *input,
                                  ap_filter_t* filter,
                                  apr_bucket_brigade* brigade,
                                  ap_input_mode_t mode,
                                  apr_read_type_e block,
                                  apr_off_t readbytes);

#endif /* defined(__mod_h2__h2_task_input__) */
