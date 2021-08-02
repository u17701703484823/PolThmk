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

#include <assert.h>
#include <stdio.h>

#include <apr_strings.h>

#include <httpd.h>
#include <http_core.h>
#include <http_log.h>

#include <nghttp2/nghttp2.h>

#include "h2_private.h"
#include "h2_bucket.h"
#include "h2_util.h"
#include "h2_resp_head.h"


h2_resp_head *h2_resp_head_create(int stream_id,
                                  apr_status_t task_status,
                                  const char *http_status,
                                  apr_array_header_t *hlines,
                                  h2_bucket *data)
{
    apr_size_t nvlen = 1 + (hlines? hlines->nelts : 0);
    /* we allocate one block for the h2_resp_head and the array of
     * nghtt2_nv structures.
     */
    h2_resp_head *head = calloc(1, sizeof(h2_resp_head)
                                + (nvlen * sizeof(nghttp2_nv)));
    if (head == NULL) {
        return NULL;
    }
    
    head->stream_id = stream_id;
    head->task_status = task_status;
    head->http_status = http_status;
    head->data = data;
    
    if (hlines) {
        nghttp2_nv *nvs = (nghttp2_nv *)&head->nv;
        nvs->name = (uint8_t *)":status";
        nvs->namelen = strlen(":status");
        nvs->value = (uint8_t *)http_status;
        nvs->valuelen = strlen(http_status);
        
        int seen_clen = 0;
        for (int i = 0; i < hlines->nelts; ++i) {
            char *hline = ((char **)hlines->elts)[i];
            nghttp2_nv *nv = &nvs[i + 1];
            char *sep = strchr(hline, ':');
            if (!sep) {
                /* not valid format, abort */
                return NULL;
            }
            (*sep++) = '\0';
            nv->name = (uint8_t *)h2_strlwr(hline);
            nv->namelen = strlen(hline);
            while (*sep == ' ' || *sep == '\t') {
                ++sep;
            }
            if (*sep) {
                nv->value = (uint8_t *)sep;
                nv->valuelen = strlen(sep);
            }
            else {
                /* reached end of line, an empty header value */
                nv->value = (uint8_t *)"";
                nv->valuelen = 0;
            }
        }
        head->nvlen = nvlen;
    }
    return head;
}

void h2_resp_head_destroy(h2_resp_head *head)
{
    if (head->data) {
        h2_bucket_destroy(head->data);
        head->data = NULL;
    }
    free(head);
}
