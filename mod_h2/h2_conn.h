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


#ifndef __mod_h2__h2_conn__
#define __mod_h2__h2_conn__

/* Process the connection that is now starting the HTTP/2
 * conversation. Return when the HTTP/2 session is done
 * and the connection will close.
 */
apr_status_t h2_conn_process(conn_rec *c);

/* Process the request that has been upgraded to a HTTP/2
 * conversation. Return when the HTTP/2 session is done
 * and the connection will close.
 */
apr_status_t h2_conn_rprocess(request_rec *r);

/* Initialize this child process for h2 connection work,
 * to be called once during child init before multi processing
 * starts.
 */
apr_status_t h2_conn_child_init(apr_pool_t *pool, server_rec *s);



#endif /* defined(__mod_h2__h2_conn__) */
