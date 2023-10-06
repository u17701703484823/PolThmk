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
#include <apr_base64.h>
#include <apr_strings.h>

#include <httpd.h>
#include <http_core.h>
#include <http_config.h>
#include <http_log.h>

#include "h2_private.h"
#include "h2_bucket_eoc.h"
#include "h2_bucket_eos.h"
#include "h2_config.h"
#include "h2_h2.h"
#include "h2_mplx.h"
#include "h2_response.h"
#include "h2_stream.h"
#include "h2_stream_set.h"
#include "h2_from_h1.h"
#include "h2_task.h"
#include "h2_session.h"
#include "h2_util.h"
#include "h2_version.h"
#include "h2_workers.h"

static int frame_print(const nghttp2_frame *frame, char *buffer, size_t maxlen);

static int h2_session_status_from_apr_status(apr_status_t rv)
{
    if (rv == APR_SUCCESS) {
        return NGHTTP2_NO_ERROR;
    }
    else if (APR_STATUS_IS_EAGAIN(rv)) {
        return NGHTTP2_ERR_WOULDBLOCK;
    }
    else if (APR_STATUS_IS_EOF(rv)) {
            return NGHTTP2_ERR_EOF;
    }
    return NGHTTP2_ERR_PROTO;
}

static int stream_open(h2_session *session, int stream_id)
{
    h2_stream * stream;
    apr_pool_t *stream_pool;
    if (session->aborted) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    
    if (session->spare) {
        stream_pool = session->spare;
        session->spare = NULL;
    }
    else {
        apr_pool_create(&stream_pool, session->pool);
    }
    
    stream = h2_stream_create(stream_id, stream_pool, session);
    stream->state = H2_STREAM_ST_OPEN;
    
    h2_stream_set_add(session->streams, stream);
    if (stream->id > session->max_stream_received) {
        session->max_stream_received = stream->id;
    }
    
    ap_log_cerror(APLOG_MARK, APLOG_DEBUG, 0, session->c,
                  "h2_session: stream(%ld-%d): opened",
                  session->id, stream_id);
    
    return 0;
}

/**
 * Determine the importance of streams when scheduling tasks.
 * - if both stream depend on the same one, compare weights
 * - if one stream is closer to the root, prioritize that one
 * - if both are on the same level, use the weight of their root
 *   level ancestors
 */
static int spri_cmp(int sid1, nghttp2_stream *s1, 
                    int sid2, nghttp2_stream *s2, h2_session *session)
{
    nghttp2_stream *p1, *p2;
    
    p1 = nghttp2_stream_get_parent(s1);
    p2 = nghttp2_stream_get_parent(s2);
    
    if (p1 == p2) {
        int32_t w1, w2;
        
        w1 = nghttp2_stream_get_weight(s1);
        w2 = nghttp2_stream_get_weight(s2);
        return w2 - w1;
    }
    else if (!p1) {
        /* stream 1 closer to root */
        return -1;
    }
    else if (!p2) {
        /* stream 2 closer to root */
        return 1;
    }
    return spri_cmp(sid1, p1, sid2, p2, session);
}

static int stream_pri_cmp(int sid1, int sid2, void *ctx)
{
    h2_session *session = ctx;
    nghttp2_stream *s1, *s2;
    
    s1 = nghttp2_session_find_stream(session->ngh2, sid1);
    s2 = nghttp2_session_find_stream(session->ngh2, sid2);

    if (s1 == s2) {
        return 0;
    }
    else if (!s1) {
        return 1;
    }
    else if (!s2) {
        return -1;
    }
    return spri_cmp(sid1, s1, sid2, s2, session);
}

static apr_status_t stream_end_headers(h2_session *session,
                                       h2_stream *stream, int eos)
{
    (void)session;
    return h2_stream_schedule(stream, eos, stream_pri_cmp, session);
}

/*
 * Callback when nghttp2 wants to send bytes back to the client.
 */
static ssize_t send_cb(nghttp2_session *ngh2,
                       const uint8_t *data, size_t length,
                       int flags, void *userp)
{
    h2_session *session = (h2_session *)userp;
    apr_status_t status;
    
    (void)ngh2;
    (void)flags;
    status = h2_conn_io_write(&session->io, (const char *)data, length);
    if (status == APR_SUCCESS) {
        return length;
    }
    if (APR_STATUS_IS_EAGAIN(status)) {
        return NGHTTP2_ERR_WOULDBLOCK;
    }
    ap_log_cerror(APLOG_MARK, APLOG_DEBUG, status, session->c,
                  "h2_session: send error");
    return h2_session_status_from_apr_status(status);
}

static int on_invalid_frame_recv_cb(nghttp2_session *ngh2,
                                    const nghttp2_frame *frame,
                                    int error, void *userp)
{
    h2_session *session = (h2_session *)userp;
    (void)ngh2;
    
    if (session->aborted) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    if (APLOGctrace2(session->c)) {
        char buffer[256];
        
        frame_print(frame, buffer, sizeof(buffer)/sizeof(buffer[0]));
        ap_log_cerror(APLOG_MARK, APLOG_TRACE2, 0, session->c,
                      "h2_session: callback on_invalid_frame_recv error=%d %s",
                      error, buffer);
    }
    return 0;
}

static int on_data_chunk_recv_cb(nghttp2_session *ngh2, uint8_t flags,
                                 int32_t stream_id,
                                 const uint8_t *data, size_t len, void *userp)
{
    int rv;
    h2_session *session = (h2_session *)userp;
    h2_stream * stream;
    apr_status_t status;
    
    (void)flags;
    if (session->aborted) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    stream = h2_stream_set_get(session->streams, stream_id);
    if (!stream) {
        ap_log_cerror(APLOG_MARK, APLOG_ERR, 0, session->c,
                      APLOGNO(02919) 
                      "h2_session:  stream(%ld-%d): on_data_chunk for unknown stream",
                      session->id, (int)stream_id);
        rv = nghttp2_submit_rst_stream(ngh2, NGHTTP2_FLAG_NONE, stream_id,
                                       NGHTTP2_INTERNAL_ERROR);
        if (nghttp2_is_fatal(rv)) {
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }
        return 0;
    }
    
    status = h2_stream_write_data(stream, (const char *)data, len);
    ap_log_cerror(APLOG_MARK, APLOG_TRACE1, status, session->c,
                  "h2_stream(%ld-%d): written DATA, length %d",
                  session->id, stream_id, (int)len);
    if (status != APR_SUCCESS) {
        rv = nghttp2_submit_rst_stream(ngh2, NGHTTP2_FLAG_NONE, stream_id,
                                       H2_STREAM_RST(stream, H2_ERR_INTERNAL_ERROR));
        if (nghttp2_is_fatal(rv)) {
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }
    }
    return 0;
}

static int before_frame_send_cb(nghttp2_session *ngh2,
                                const nghttp2_frame *frame,
                                void *userp)
{
    h2_session *session = (h2_session *)userp;
    (void)ngh2;

    if (session->aborted) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    /* Set the need to flush output when we have added one of the 
     * following frame types */
    switch (frame->hd.type) {
        case NGHTTP2_RST_STREAM:
        case NGHTTP2_WINDOW_UPDATE:
        case NGHTTP2_PUSH_PROMISE:
        case NGHTTP2_PING:
        case NGHTTP2_GOAWAY:
            session->flush = 1;
            break;
        default:
            break;

    }
    if (APLOGctrace2(session->c)) {
        char buffer[256];
        frame_print(frame, buffer, sizeof(buffer)/sizeof(buffer[0]));
        ap_log_cerror(APLOG_MARK, APLOG_DEBUG, 0, session->c,
                      "h2_session(%ld): before_frame_send %s", 
                      session->id, buffer);
    }
    return 0;
}

static int on_frame_send_cb(nghttp2_session *ngh2,
                            const nghttp2_frame *frame,
                            void *userp)
{
    h2_session *session = (h2_session *)userp;
    (void)ngh2;
    if (APLOGctrace2(session->c)) {
        char buffer[256];
        frame_print(frame, buffer, sizeof(buffer)/sizeof(buffer[0]));
        ap_log_cerror(APLOG_MARK, APLOG_DEBUG, 0, session->c,
                      "h2_session(%ld): on_frame_send %s", 
                      session->id, buffer);
    }
    return 0;
}

static int on_frame_not_send_cb(nghttp2_session *ngh2,
                                const nghttp2_frame *frame,
                                int lib_error_code, void *userp)
{
    h2_session *session = (h2_session *)userp;
    (void)ngh2;
    
    if (APLOGctrace2(session->c)) {
        char buffer[256];
        
        frame_print(frame, buffer, sizeof(buffer)/sizeof(buffer[0]));
        ap_log_cerror(APLOG_MARK, APLOG_DEBUG, 0, session->c,
                      "h2_session: callback on_frame_not_send error=%d %s",
                      lib_error_code, buffer);
    }
    return 0;
}

static apr_status_t stream_destroy(h2_session *session, 
                                   h2_stream *stream,
                                   uint32_t error_code) 
{
    if (!error_code) {
        ap_log_cerror(APLOG_MARK, APLOG_DEBUG, 0, session->c,
                      "h2_stream(%ld-%d): handled, closing", 
                      session->id, (int)stream->id);
        if (stream->id > session->max_stream_handled) {
            session->max_stream_handled = stream->id;
        }
    }
    else {
        ap_log_cerror(APLOG_MARK, APLOG_DEBUG, 0, session->c,
                      "h2_stream(%ld-%d): closing with err=%d %s", 
                      session->id, (int)stream->id, (int)error_code,
                      h2_h2_err_description(error_code));
        h2_stream_rst(stream, error_code);
    }
    
    return h2_conn_io_writeb(&session->io,
                             h2_bucket_eos_create(session->c->bucket_alloc, 
                                                  stream));
}

static int on_stream_close_cb(nghttp2_session *ngh2, int32_t stream_id,
                              uint32_t error_code, void *userp)
{
    h2_session *session = (h2_session *)userp;
    h2_stream *stream;
    
    (void)ngh2;
    if (session->aborted) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    stream = h2_stream_set_get(session->streams, stream_id);
    if (stream) {
        stream_destroy(session, stream, error_code);
    }
    
    if (error_code) {
        ap_log_cerror(APLOG_MARK, APLOG_DEBUG, 0, session->c,
                      "h2_stream(%ld-%d): close error %d",
                      session->id, (int)stream_id, error_code);
    }
    
    return 0;
}

static int on_begin_headers_cb(nghttp2_session *ngh2,
                               const nghttp2_frame *frame, void *userp)
{
    /* This starts a new stream. */
    int rv;
    (void)ngh2;
    rv = stream_open((h2_session *)userp, frame->hd.stream_id);
    if (rv != NGHTTP2_ERR_CALLBACK_FAILURE) {
      /* on_header_cb or on_frame_recv_cb will dectect that stream
         does not exist and submit RST_STREAM. */
      return 0;
    }
    return NGHTTP2_ERR_CALLBACK_FAILURE;
}

static int on_header_cb(nghttp2_session *ngh2, const nghttp2_frame *frame,
                        const uint8_t *name, size_t namelen,
                        const uint8_t *value, size_t valuelen,
                        uint8_t flags,
                        void *userp)
{
    h2_session *session = (h2_session *)userp;
    h2_stream * stream;
    apr_status_t status;
    
    (void)ngh2;
    (void)flags;
    if (session->aborted) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    stream = h2_stream_set_get(session->streams,
                                           frame->hd.stream_id);
    if (!stream) {
        ap_log_cerror(APLOG_MARK, APLOG_ERR, 0, session->c,
                      APLOGNO(02920) 
                      "h2_session:  stream(%ld-%d): on_header for unknown stream",
                      session->id, (int)frame->hd.stream_id);
        return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    }
    
    status = h2_stream_write_header(stream,
                                               (const char *)name, namelen,
                                               (const char *)value, valuelen);
    if (status != APR_SUCCESS) {
        return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    }
    return 0;
}

/**
 * nghttp2 session has received a complete frame. Most, it uses
 * for processing of internal state. HEADER and DATA frames however
 * we need to handle ourself.
 */
static int on_frame_recv_cb(nghttp2_session *ng2s,
                            const nghttp2_frame *frame,
                            void *userp)
{
    int rv;
    h2_session *session = (h2_session *)userp;
    apr_status_t status = APR_SUCCESS;
    if (session->aborted) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    
    ++session->frames_received;
    ap_log_cerror(APLOG_MARK, APLOG_TRACE1, 0, session->c,
                  "h2_session(%ld): on_frame_rcv #%ld, type=%d", session->id,
                  (long)session->frames_received, frame->hd.type);
    switch (frame->hd.type) {
        case NGHTTP2_HEADERS: {
            int eos;
            h2_stream * stream = h2_stream_set_get(session->streams,
                                                   frame->hd.stream_id);
            if (stream == NULL) {
                ap_log_cerror(APLOG_MARK, APLOG_ERR, 0, session->c,
                              APLOGNO(02921) 
                              "h2_session:  stream(%ld-%d): HEADERS frame "
                              "for unknown stream", session->id,
                              (int)frame->hd.stream_id);
                rv = nghttp2_submit_rst_stream(ng2s, NGHTTP2_FLAG_NONE,
                                               frame->hd.stream_id,
                                               NGHTTP2_INTERNAL_ERROR);
                if (nghttp2_is_fatal(rv)) {
                    return NGHTTP2_ERR_CALLBACK_FAILURE;
                }
                return 0;
            }

            eos = (frame->hd.flags & NGHTTP2_FLAG_END_STREAM);
            status = stream_end_headers(session, stream, eos);

            break;
        }
        case NGHTTP2_DATA: {
            h2_stream * stream = h2_stream_set_get(session->streams,
                                                   frame->hd.stream_id);
            if (stream == NULL) {
                ap_log_cerror(APLOG_MARK, APLOG_ERR, 0, session->c,
                              APLOGNO(02922) 
                              "h2_session:  stream(%ld-%d): DATA frame "
                              "for unknown stream", session->id,
                              (int)frame->hd.stream_id);
                rv = nghttp2_submit_rst_stream(ng2s, NGHTTP2_FLAG_NONE,
                                               frame->hd.stream_id,
                                               NGHTTP2_INTERNAL_ERROR);
                if (nghttp2_is_fatal(rv)) {
                    return NGHTTP2_ERR_CALLBACK_FAILURE;
                }
                return 0;
            }
            break;
        }
        case NGHTTP2_PRIORITY: {
            session->reprioritize = 1;
            ap_log_cerror(APLOG_MARK, APLOG_TRACE1, 0, session->c,
                          "h2_session:  stream(%ld-%d): PRIORITY frame "
                          " weight=%d, dependsOn=%d, exclusive=%d", 
                          session->id, (int)frame->hd.stream_id,
                          frame->priority.pri_spec.weight,
                          frame->priority.pri_spec.stream_id,
                          frame->priority.pri_spec.exclusive);
            break;
        }
        default:
            if (APLOGctrace2(session->c)) {
                char buffer[256];
                
                frame_print(frame, buffer,
                            sizeof(buffer)/sizeof(buffer[0]));
                ap_log_cerror(APLOG_MARK, APLOG_TRACE2, 0, session->c,
                              "h2_session: on_frame_rcv %s", buffer);
            }
            break;
    }

    /* only DATA and HEADERS frame can bear END_STREAM flag.  Other
       frame types may have other flag which has the same value, so we
       have to check the frame type first.  */
    if ((frame->hd.type == NGHTTP2_DATA || frame->hd.type == NGHTTP2_HEADERS) &&
        frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
        h2_stream * stream = h2_stream_set_get(session->streams,
                                               frame->hd.stream_id);
        if (stream != NULL) {
            status = h2_stream_write_eos(stream);
            ap_log_cerror(APLOG_MARK, APLOG_DEBUG, status, session->c,
                          "h2_stream(%ld-%d): input closed",
                          session->id, (int)frame->hd.stream_id);
        }
    }
    
    if (status != APR_SUCCESS) {
        ap_log_cerror(APLOG_MARK, APLOG_ERR, status, session->c,
                      APLOGNO(02923) 
                      "h2_session: stream(%ld-%d): error handling frame",
                      session->id, (int)frame->hd.stream_id);
        rv = nghttp2_submit_rst_stream(ng2s, NGHTTP2_FLAG_NONE,
                                       frame->hd.stream_id,
                                       NGHTTP2_INTERNAL_ERROR);
        if (nghttp2_is_fatal(rv)) {
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }
        return 0;
    }
    
    return 0;
}

static apr_status_t pass_data(void *ctx, 
                              const char *data, apr_size_t length)
{
    return h2_conn_io_write(&((h2_session*)ctx)->io, data, length);
}


static char immortal_zeros[H2_MAX_PADLEN];

static int on_send_data_cb(nghttp2_session *ngh2, 
                           nghttp2_frame *frame, 
                           const uint8_t *framehd, 
                           size_t length, 
                           nghttp2_data_source *source, 
                           void *userp)
{
    apr_status_t status = APR_SUCCESS;
    h2_session *session = (h2_session *)userp;
    int stream_id = (int)frame->hd.stream_id;
    const unsigned char padlen = frame->data.padlen;
    int eos;
    h2_stream *stream;
    
    (void)ngh2;
    (void)source;
    if (session->aborted) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    
    stream = h2_stream_set_get(session->streams, stream_id);
    if (!stream) {
        ap_log_cerror(APLOG_MARK, APLOG_ERR, APR_NOTFOUND, session->c,
                      APLOGNO(02924) 
                      "h2_stream(%ld-%d): send_data",
                      session->id, (int)stream_id);
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    
    ap_log_cerror(APLOG_MARK, APLOG_TRACE2, 0, session->c,
                  "h2_stream(%ld-%d): send_data_cb for %ld bytes",
                  session->id, (int)stream_id, (long)length);
                  
    if (h2_conn_io_is_buffered(&session->io)) {
        status = h2_conn_io_write(&session->io, (const char *)framehd, 9);
        if (status == APR_SUCCESS) {
            if (padlen) {
                status = h2_conn_io_write(&session->io, (const char *)&padlen, 1);
            }
            
            if (status == APR_SUCCESS) {
                apr_size_t len = length;
                status = h2_stream_readx(stream, pass_data, session, 
                                         &len, &eos);
                if (status == APR_SUCCESS && len != length) {
                    status = APR_EINVAL;
                }
            }
            
            if (status == APR_SUCCESS && padlen) {
                if (padlen) {
                    status = h2_conn_io_write(&session->io, immortal_zeros, padlen);
                }
            }
        }
    }
    else {
        apr_bucket *b;
        char *header = apr_pcalloc(stream->pool, 10);
        memcpy(header, (const char *)framehd, 9);
        if (padlen) {
            header[9] = (char)padlen;
        }
        b = apr_bucket_pool_create(header, padlen? 10 : 9, 
                                   stream->pool, session->c->bucket_alloc);
        status = h2_conn_io_writeb(&session->io, b);
        
        if (status == APR_SUCCESS) {
            apr_size_t len = length;
            status = h2_stream_read_to(stream, session->io.output, &len, &eos);
            session->io.unflushed = 1;
            if (status == APR_SUCCESS && len != length) {
                status = APR_EINVAL;
            }
        }
            
        if (status == APR_SUCCESS && padlen) {
            b = apr_bucket_immortal_create(immortal_zeros, padlen, 
                                           session->c->bucket_alloc);
            status = h2_conn_io_writeb(&session->io, b);
        }
    }
    
    
    if (status == APR_SUCCESS) {
        stream->data_frames_sent++;
        h2_conn_io_consider_flush(&session->io);
        return 0;
    }
    else {
        ap_log_cerror(APLOG_MARK, APLOG_DEBUG, status, session->c,
                      APLOGNO(02925) 
                      "h2_stream(%ld-%d): failed send_data_cb",
                      session->id, (int)stream_id);
    }
    
    return h2_session_status_from_apr_status(status);
}

static ssize_t on_data_source_read_length_cb(nghttp2_session *session, 
                                             uint8_t frame_type, int32_t stream_id, 
                                             int32_t session_remote_window_size, 
                                             int32_t stream_remote_window_size, 
                                             uint32_t remote_max_frame_size, 
                                             void *user_data)
{
    /* DATA frames add 9 bytes header plus 1 byte for padlen and additional 
     * padlen bytes. Keep below TLS maximum record size.
     * TODO: respect pad bytes when we have that feature.
     */
    (void)session;
    (void)frame_type;
    (void)stream_id;
    (void)session_remote_window_size;
    (void)stream_remote_window_size;
    (void)remote_max_frame_size;
    (void)user_data;
    return (16*1024 - 10);
}

#define NGH2_SET_CALLBACK(callbacks, name, fn)\
nghttp2_session_callbacks_set_##name##_callback(callbacks, fn)

static apr_status_t init_callbacks(conn_rec *c, nghttp2_session_callbacks **pcb)
{
    int rv = nghttp2_session_callbacks_new(pcb);
    if (rv != 0) {
        ap_log_cerror(APLOG_MARK, APLOG_ERR, 0, c,
                      APLOGNO(02926) "nghttp2_session_callbacks_new: %s",
                      nghttp2_strerror(rv));
        return APR_EGENERAL;
    }
    
    NGH2_SET_CALLBACK(*pcb, send, send_cb);
    NGH2_SET_CALLBACK(*pcb, on_frame_recv, on_frame_recv_cb);
    NGH2_SET_CALLBACK(*pcb, on_invalid_frame_recv, on_invalid_frame_recv_cb);
    NGH2_SET_CALLBACK(*pcb, on_data_chunk_recv, on_data_chunk_recv_cb);
    NGH2_SET_CALLBACK(*pcb, before_frame_send, before_frame_send_cb);
    NGH2_SET_CALLBACK(*pcb, on_frame_send, on_frame_send_cb);
    NGH2_SET_CALLBACK(*pcb, on_frame_not_send, on_frame_not_send_cb);
    NGH2_SET_CALLBACK(*pcb, on_stream_close, on_stream_close_cb);
    NGH2_SET_CALLBACK(*pcb, on_begin_headers, on_begin_headers_cb);
    NGH2_SET_CALLBACK(*pcb, on_header, on_header_cb);
    NGH2_SET_CALLBACK(*pcb, send_data, on_send_data_cb);
    NGH2_SET_CALLBACK(*pcb, data_source_read_length, on_data_source_read_length_cb);
    
    return APR_SUCCESS;
}

static h2_session *h2_session_create_int(conn_rec *c,
                                         request_rec *r,
                                         h2_config *config, 
                                         h2_workers *workers)
{
    nghttp2_session_callbacks *callbacks = NULL;
    nghttp2_option *options = NULL;

    apr_pool_t *pool = NULL;
    apr_status_t status = apr_pool_create(&pool, r? r->pool : c->pool);
    h2_session *session;
    if (status != APR_SUCCESS) {
        return NULL;
    }

    session = apr_pcalloc(pool, sizeof(h2_session));
    if (session) {
        int rv;
        session->id = c->id;
        session->c = c;
        session->r = r;
        
        session->max_stream_count = h2_config_geti(config, H2_CONF_MAX_STREAMS);
        session->max_stream_mem = h2_config_geti(config, H2_CONF_STREAM_MAX_MEM);

        session->pool = pool;
        
        status = apr_thread_cond_create(&session->iowait, session->pool);
        if (status != APR_SUCCESS) {
            return NULL;
        }
        
        session->streams = h2_stream_set_create(session->pool);
        
        session->workers = workers;
        session->mplx = h2_mplx_create(c, session->pool, workers);
        
        h2_conn_io_init(&session->io, c);
        session->bbtmp = apr_brigade_create(session->pool, c->bucket_alloc);
        
        status = init_callbacks(c, &callbacks);
        if (status != APR_SUCCESS) {
            ap_log_cerror(APLOG_MARK, APLOG_ERR, status, c, APLOGNO(02927) 
                          "nghttp2: error in init_callbacks");
            h2_session_destroy(session);
            return NULL;
        }
        
        rv = nghttp2_option_new(&options);
        if (rv != 0) {
            ap_log_cerror(APLOG_MARK, APLOG_ERR, APR_EGENERAL, c,
                          APLOGNO(02928) "nghttp2_option_new: %s", 
                          nghttp2_strerror(rv));
            h2_session_destroy(session);
            return NULL;
        }

        nghttp2_option_set_peer_max_concurrent_streams(options, 
                                                       (uint32_t)session->max_stream_count);

        /* We need to handle window updates ourself, otherwise we
         * get flooded by nghttp2. */
        nghttp2_option_set_no_auto_window_update(options, 1);
        
        rv = nghttp2_session_server_new2(&session->ngh2, callbacks,
                                         session, options);
        nghttp2_session_callbacks_del(callbacks);
        nghttp2_option_del(options);
        
        if (rv != 0) {
            ap_log_cerror(APLOG_MARK, APLOG_ERR, APR_EGENERAL, c,
                          APLOGNO(02929) "nghttp2_session_server_new: %s",
                          nghttp2_strerror(rv));
            h2_session_destroy(session);
            return NULL;
        }
        
    }
    return session;
}

h2_session *h2_session_create(conn_rec *c, h2_config *config, 
                              h2_workers *workers)
{
    return h2_session_create_int(c, NULL, config, workers);
}

h2_session *h2_session_rcreate(request_rec *r, h2_config *config, 
                               h2_workers *workers)
{
    return h2_session_create_int(r->connection, r, config, workers);
}

void h2_session_destroy(h2_session *session)
{
    AP_DEBUG_ASSERT(session);
    if (session->mplx) {
        h2_mplx_release_and_join(session->mplx, session->iowait);
        session->mplx = NULL;
    }
    if (session->streams) {
        if (h2_stream_set_size(session->streams)) {
            ap_log_cerror(APLOG_MARK, APLOG_TRACE2, 0, session->c,
                          "h2_session(%ld): destroy, %d streams open",
                          session->id, (int)h2_stream_set_size(session->streams));
        }
        h2_stream_set_destroy(session->streams);
        session->streams = NULL;
    }
    if (session->ngh2) {
        nghttp2_session_del(session->ngh2);
        session->ngh2 = NULL;
    }
}

void h2_session_cleanup(h2_session *session)
{
    h2_session_destroy(session);
    if (session->pool) {
        apr_pool_destroy(session->pool);
    }
}

static apr_status_t h2_session_abort_int(h2_session *session, int reason)
{
    AP_DEBUG_ASSERT(session);
    if (!session->aborted) {
        session->aborted = 1;
        
        if (session->ngh2) {            
            if (NGHTTP2_ERR_EOF == reason) {
                /* This is our way of indication that the connection is
                 * gone. No use to send any GOAWAY frames. */
                nghttp2_session_terminate_session(session->ngh2, reason);
            }
            else if (!reason) {
                nghttp2_submit_goaway(session->ngh2, NGHTTP2_FLAG_NONE, 
                                      session->max_stream_received, 
                                      reason, NULL, 0);
                nghttp2_session_send(session->ngh2);
            }
            else {
                const char *err = nghttp2_strerror(reason);
                
                ap_log_cerror(APLOG_MARK, APLOG_DEBUG, 0, session->c,
                              "session(%ld): aborting session, reason=%d %s",
                              session->id, reason, err);
                
                /* The connection might still be there and we shut down
                 * with GOAWAY and reason information. */
                nghttp2_submit_goaway(session->ngh2, NGHTTP2_FLAG_NONE, 
                                      session->max_stream_received, 
                                      reason, (const uint8_t *)err, 
                                      strlen(err));
                nghttp2_session_send(session->ngh2);
            }
            h2_conn_io_flush(&session->io);
        }
        h2_mplx_abort(session->mplx);
    }
    return APR_SUCCESS;
}

apr_status_t h2_session_abort(h2_session *session, apr_status_t reason, int rv)
{
    AP_DEBUG_ASSERT(session);
    if (rv == 0) {
        rv = NGHTTP2_ERR_PROTO;
        switch (reason) {
            case APR_ENOMEM:
                rv = NGHTTP2_ERR_NOMEM;
                break;
            case APR_SUCCESS:                            /* all fine, just... */
            case APR_EOF:                         /* client closed its end... */
            case APR_TIMEUP:                          /* got bored waiting... */
                rv = 0;                            /* ...gracefully shut down */
                break;
            case APR_EBADF:        /* connection unusable, terminate silently */
            default:
                if (APR_STATUS_IS_ECONNABORTED(reason)
                    || APR_STATUS_IS_ECONNRESET(reason)
                    || APR_STATUS_IS_EBADF(reason)) {
                    rv = NGHTTP2_ERR_EOF;
                }
                break;
        }
    }
    return h2_session_abort_int(session, rv);
}

apr_status_t h2_session_start(h2_session *session, int *rv)
{
    apr_status_t status = APR_SUCCESS;
    h2_config *config;
    nghttp2_settings_entry settings[3];
    
    AP_DEBUG_ASSERT(session);
    /* Start the conversation by submitting our SETTINGS frame */
    *rv = 0;
    config = h2_config_get(session->c);
    if (session->r) {
        const char *s, *cs;
        apr_size_t dlen; 
        h2_stream * stream;

        /* better for vhost matching */
        config = h2_config_rget(session->r);
        
        /* 'h2c' mode: we should have a 'HTTP2-Settings' header with
         * base64 encoded client settings. */
        s = apr_table_get(session->r->headers_in, "HTTP2-Settings");
        if (!s) {
            ap_log_rerror(APLOG_MARK, APLOG_ERR, APR_EINVAL, session->r,
                          APLOGNO(02931) 
                          "HTTP2-Settings header missing in request");
            return APR_EINVAL;
        }
        cs = NULL;
        dlen = h2_util_base64url_decode(&cs, s, session->pool);
        
        if (APLOGrdebug(session->r)) {
            char buffer[128];
            h2_util_hex_dump(buffer, 128, (char*)cs, dlen);
            ap_log_rerror(APLOG_MARK, APLOG_DEBUG, 0, session->r,
                          "upgrading h2c session with HTTP2-Settings: %s -> %s (%d)",
                          s, buffer, (int)dlen);
        }
        
        *rv = nghttp2_session_upgrade(session->ngh2, (uint8_t*)cs, dlen, NULL);
        if (*rv != 0) {
            status = APR_EINVAL;
            ap_log_rerror(APLOG_MARK, APLOG_ERR, status, session->r,
                          APLOGNO(02932) "nghttp2_session_upgrade: %s", 
                          nghttp2_strerror(*rv));
            return status;
        }
        
        /* Now we need to auto-open stream 1 for the request we got. */
        *rv = stream_open(session, 1);
        if (*rv != 0) {
            status = APR_EGENERAL;
            ap_log_rerror(APLOG_MARK, APLOG_ERR, status, session->r,
                          APLOGNO(02933) "open stream 1: %s", 
                          nghttp2_strerror(*rv));
            return status;
        }
        
        stream = h2_stream_set_get(session->streams, 1);
        if (stream == NULL) {
            status = APR_EGENERAL;
            ap_log_rerror(APLOG_MARK, APLOG_ERR, status, session->r,
                          APLOGNO(02934) "lookup of stream 1");
            return status;
        }
        
        status = h2_stream_rwrite(stream, session->r);
        if (status != APR_SUCCESS) {
            return status;
        }
        status = stream_end_headers(session, stream, 1);
        if (status != APR_SUCCESS) {
            return status;
        }
    }

    settings[0].settings_id = NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS;
    settings[0].value = (uint32_t)session->max_stream_count;
    settings[1].settings_id = NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE;
    settings[1].value = h2_config_geti(config, H2_CONF_WIN_SIZE);
    settings[2].settings_id = NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE;
    settings[2].value = 64*1024;
    
    *rv = nghttp2_submit_settings(session->ngh2, NGHTTP2_FLAG_NONE,
                                 settings,
                                 sizeof(settings)/sizeof(settings[0]));
    if (*rv != 0) {
        status = APR_EGENERAL;
        ap_log_cerror(APLOG_MARK, APLOG_ERR, status, session->c,
                      APLOGNO(02935) "nghttp2_submit_settings: %s", 
                      nghttp2_strerror(*rv));
    }
    
    return status;
}

static int h2_session_want_write(h2_session *session)
{
    return nghttp2_session_want_write(session->ngh2);
}

typedef struct {
    h2_session *session;
    int resume_count;
} resume_ctx;

static int resume_on_data(void *ctx, h2_stream *stream) {
    resume_ctx *rctx = (resume_ctx*)ctx;
    h2_session *session = rctx->session;
    AP_DEBUG_ASSERT(session);
    AP_DEBUG_ASSERT(stream);
    
    if (h2_stream_is_suspended(stream)) {
        if (h2_mplx_out_has_data_for(stream->session->mplx, stream->id)) {
            int rv;
            h2_stream_set_suspended(stream, 0);
            ++rctx->resume_count;
            
            rv = nghttp2_session_resume_data(session->ngh2, stream->id);
            ap_log_cerror(APLOG_MARK, nghttp2_is_fatal(rv)?
                          APLOG_ERR : APLOG_DEBUG, 0, session->c,
                          APLOGNO(02936) 
                          "h2_stream(%ld-%d): resuming stream %s",
                          session->id, stream->id, nghttp2_strerror(rv));
        }
    }
    return 1;
}

static int h2_session_resume_streams_with_data(h2_session *session) {
    AP_DEBUG_ASSERT(session);
    if (!h2_stream_set_is_empty(session->streams)
        && session->mplx && !session->aborted) {
        resume_ctx ctx;
        
        ctx.session      = session;
        ctx.resume_count = 0;

        /* Resume all streams where we have data in the out queue and
         * which had been suspended before. */
        h2_stream_set_iter(session->streams, resume_on_data, &ctx);
        return ctx.resume_count;
    }
    return 0;
}

static void update_window(void *ctx, int stream_id, apr_size_t bytes_read)
{
    h2_session *session = (h2_session*)ctx;
    nghttp2_session_consume(session->ngh2, stream_id, bytes_read);
}

static apr_status_t h2_session_flush(h2_session *session) 
{
    session->flush = 0;
    return h2_conn_io_flush(&session->io);
}

static apr_status_t h2_session_update_windows(h2_session *session)
{
    return h2_mplx_in_update_windows(session->mplx, update_window, session);
}

apr_status_t h2_session_write(h2_session *session, apr_interval_time_t timeout)
{
    apr_status_t status = APR_EAGAIN;
    h2_stream *stream = NULL;
    
    AP_DEBUG_ASSERT(session);
    
    if (session->reprioritize) {
        h2_mplx_reprioritize(session->mplx, stream_pri_cmp, session);
        session->reprioritize = 0;
    }
    
    /* Check that any pending window updates are sent. */
    status = h2_session_update_windows(session);
    if (status != APR_SUCCESS && !APR_STATUS_IS_EAGAIN(status)) {
        return status;
    }
    
    if (h2_session_want_write(session)) {
        int rv;
        status = APR_SUCCESS;
        rv = nghttp2_session_send(session->ngh2);
        if (rv != 0) {
            ap_log_cerror( APLOG_MARK, APLOG_DEBUG, 0, session->c,
                          "h2_session: send: %s", nghttp2_strerror(rv));
            if (nghttp2_is_fatal(rv)) {
                h2_session_abort_int(session, rv);
                status = APR_ECONNABORTED;
            }
        }
    }
    
    /* If we have responses ready, submit them now. */
    while (!session->aborted 
           && (stream = h2_mplx_next_submit(session->mplx, session->streams)) != NULL) {
        status = h2_session_handle_response(session, stream);
    }
    
    if (!session->aborted && h2_session_resume_streams_with_data(session) > 0) {
    }
    
    if (!session->aborted && !session->flush && timeout > 0 
        && !h2_session_want_write(session)) {
        h2_session_flush(session);
        status = h2_mplx_out_trywait(session->mplx, timeout, session->iowait);

        if (status != APR_TIMEUP
            && h2_session_resume_streams_with_data(session) > 0) {
        }
        else {
            /* nothing happened to ongoing streams, do some house-keeping */
        }
    }
    
    if (h2_session_want_write(session)) {
        int rv;
        status = APR_SUCCESS;
        rv = nghttp2_session_send(session->ngh2);
        if (rv != 0) {
            ap_log_cerror( APLOG_MARK, APLOG_DEBUG, 0, session->c,
                          "h2_session: send2: %s", nghttp2_strerror(rv));
            if (nghttp2_is_fatal(rv)) {
                h2_session_abort_int(session, rv);
                status = APR_ECONNABORTED;
            }
        }
    }
    
    if (session->flush) {
        h2_session_flush(session);
    }
    
    return status;
}

h2_stream *h2_session_get_stream(h2_session *session, int stream_id)
{
    AP_DEBUG_ASSERT(session);
    return h2_stream_set_get(session->streams, stream_id);
}

/* h2_io_on_read_cb implementation that offers the data read
 * directly to the session for consumption.
 */
static apr_status_t session_receive(const char *data, apr_size_t len,
                                    apr_size_t *readlen, int *done,
                                    void *puser)
{
    h2_session *session = (h2_session *)puser;
    AP_DEBUG_ASSERT(session);
    if (len > 0) {
        ssize_t n = nghttp2_session_mem_recv(session->ngh2,
                                             (const uint8_t *)data, len);
        if (n < 0) {
            ap_log_cerror(APLOG_MARK, APLOG_DEBUG, APR_EGENERAL,
                          session->c,
                          "h2_session: nghttp2_session_mem_recv error %d",
                          (int)n);
            if (nghttp2_is_fatal((int)n)) {
                *done = 1;
                h2_session_abort_int(session, (int)n);
                return APR_EGENERAL;
            }
        }
        else {
            *readlen = n;
        }
    }
    return APR_SUCCESS;
}

apr_status_t h2_session_read(h2_session *session, apr_read_type_e block)
{
    AP_DEBUG_ASSERT(session);
    if (block == APR_BLOCK_READ) {
        /* before we do a blocking read, make sure that all our output
         * is send out. Otherwise we might deadlock. */
        h2_session_flush(session);
    }
    return h2_conn_io_read(&session->io, block, session_receive, session);
}

apr_status_t h2_session_close(h2_session *session)
{
    AP_DEBUG_ASSERT(session);
    if (!session->aborted) {
        h2_session_abort_int(session, 0);
    }
    ap_log_cerror(APLOG_MARK, APLOG_DEBUG, 0,session->c,
                  "h2_session: closing, writing eoc");
    h2_conn_io_writeb(&session->io,
                      h2_bucket_eoc_create(session->c->bucket_alloc, 
                                           session));
    return h2_conn_io_flush(&session->io);
}

static ssize_t stream_data_cb(nghttp2_session *ng2s,
                              int32_t stream_id,
                              uint8_t *buf,
                              size_t length,
                              uint32_t *data_flags,
                              nghttp2_data_source *source,
                              void *puser)
{
    h2_session *session = (h2_session *)puser;
    apr_size_t nread = length;
    int eos = 0;
    apr_status_t status;
    h2_stream *stream;
    AP_DEBUG_ASSERT(session);
    
    /* The session wants to send more DATA for the stream. We need
     * to find out how much of the requested length we can send without
     * blocking.
     * Indicate EOS when we encounter it or DEFERRED if the stream
     * should be suspended.
     * TODO: for handling of TRAILERS,  the EOF indication needs
     * to be aware of that.
     */
 
    (void)ng2s;
    (void)buf;
    (void)source;
    stream = h2_stream_set_get(session->streams, stream_id);
    if (!stream) {
        ap_log_cerror(APLOG_MARK, APLOG_ERR, 0, session->c,
                      APLOGNO(02937) 
                      "h2_stream(%ld-%d): data requested but stream not found",
                      session->id, (int)stream_id);
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    
    AP_DEBUG_ASSERT(!h2_stream_is_suspended(stream));
    
    status = h2_stream_prep_read(stream, &nread, &eos);
    if (nread) {
        *data_flags |=  NGHTTP2_DATA_FLAG_NO_COPY;
    }
    
    switch (status) {
        case APR_SUCCESS:
            break;
            
        case APR_ECONNRESET:
            return nghttp2_submit_rst_stream(ng2s, NGHTTP2_FLAG_NONE,
                stream->id, stream->rst_error);
            
        case APR_EAGAIN:
            /* If there is no data available, our session will automatically
             * suspend this stream and not ask for more data until we resume
             * it. Remember at our h2_stream that we need to do this.
             */
            nread = 0;
            h2_stream_set_suspended(stream, 1);
            ap_log_cerror(APLOG_MARK, APLOG_DEBUG, 0, session->c,
                          "h2_stream(%ld-%d): suspending stream",
                          session->id, (int)stream_id);
            return NGHTTP2_ERR_DEFERRED;
            
        case APR_EOF:
            nread = 0;
            eos = 1;
            break;
            
        default:
            nread = 0;
            ap_log_cerror(APLOG_MARK, APLOG_ERR, status, session->c,
                          APLOGNO(02938) "h2_stream(%ld-%d): reading data",
                          session->id, (int)stream_id);
            return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    
    if (eos) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    }
    
    return (ssize_t)nread;
}

typedef struct {
    nghttp2_nv *nv;
    size_t nvlen;
    size_t offset;
} nvctx_t;

static int submit_response(h2_session *session, h2_response *response)
{
    nghttp2_data_provider provider;
    int rv;
    
    memset(&provider, 0, sizeof(provider));
    provider.source.fd = response->stream_id;
    provider.read_callback = stream_data_cb;
    
    ap_log_cerror(APLOG_MARK, APLOG_TRACE1, 0, session->c,
                  "h2_stream(%ld-%d): submitting response %s",
                  session->id, response->stream_id, response->status);
    
    rv = nghttp2_submit_response(session->ngh2, response->stream_id,
                                 response->ngheader->nv, 
                                 response->ngheader->nvlen, &provider);
    
    if (rv != 0) {
        ap_log_cerror(APLOG_MARK, APLOG_ERR, 0, session->c,
                      APLOGNO(02939) "h2_stream(%ld-%d): submit_response: %s",
                      session->id, response->stream_id, nghttp2_strerror(rv));
    }
    else {
        ap_log_cerror(APLOG_MARK, APLOG_DEBUG, 0, session->c,
                      "h2_stream(%ld-%d): submitted response %s, rv=%d",
                      session->id, response->stream_id, 
                      response->status, rv);
    }
    return rv;
}

/* Start submitting the response to a stream request. This is possible
 * once we have all the response headers. The response body will be
 * read by the session using the callback we supply.
 */
apr_status_t h2_session_handle_response(h2_session *session, h2_stream *stream)
{
    apr_status_t status = APR_SUCCESS;
    int rv = 0;
    AP_DEBUG_ASSERT(session);
    AP_DEBUG_ASSERT(stream);
    AP_DEBUG_ASSERT(stream->response || stream->rst_error);
    
    if (stream->response && stream->response->ngheader) {
        rv = submit_response(session, stream->response);
    }
    else {
        rv = nghttp2_submit_rst_stream(session->ngh2, NGHTTP2_FLAG_NONE,
                                       stream->id, 
                                       H2_STREAM_RST(stream, H2_ERR_PROTOCOL_ERROR));
    }
    
    if (nghttp2_is_fatal(rv)) {
        status = APR_EGENERAL;
        h2_session_abort_int(session, rv);
        ap_log_cerror(APLOG_MARK, APLOG_ERR, status, session->c,
                      APLOGNO(02940) "submit_response: %s", 
                      nghttp2_strerror(rv));
    }
    return status;
}

apr_status_t h2_session_stream_destroy(h2_session *session, h2_stream *stream)
{
    apr_pool_t *pool = h2_stream_detach_pool(stream);

    h2_mplx_stream_done(session->mplx, stream->id, stream->rst_error);
    h2_stream_set_remove(session->streams, stream->id);
    h2_stream_destroy(stream);
    
    if (pool) {
        apr_pool_clear(pool);
        if (session->spare) {
            apr_pool_destroy(session->spare);
        }
        session->spare = pool;
    }
    return APR_SUCCESS;
}

int h2_session_is_done(h2_session *session)
{
    AP_DEBUG_ASSERT(session);
    return (session->aborted
            || !session->ngh2
            || (!nghttp2_session_want_read(session->ngh2)
                && !nghttp2_session_want_write(session->ngh2)));
}

static int frame_print(const nghttp2_frame *frame, char *buffer, size_t maxlen)
{
    char scratch[128];
    size_t s_len = sizeof(scratch)/sizeof(scratch[0]);
    
    switch (frame->hd.type) {
        case NGHTTP2_DATA: {
            return apr_snprintf(buffer, maxlen,
                                "DATA[length=%d, flags=%d, stream=%d, padlen=%d]",
                                (int)frame->hd.length, frame->hd.flags,
                                frame->hd.stream_id, (int)frame->data.padlen);
        }
        case NGHTTP2_HEADERS: {
            return apr_snprintf(buffer, maxlen,
                                "HEADERS[length=%d, hend=%d, stream=%d, eos=%d]",
                                (int)frame->hd.length,
                                !!(frame->hd.flags & NGHTTP2_FLAG_END_HEADERS),
                                frame->hd.stream_id,
                                !!(frame->hd.flags & NGHTTP2_FLAG_END_STREAM));
        }
        case NGHTTP2_PRIORITY: {
            return apr_snprintf(buffer, maxlen,
                                "PRIORITY[length=%d, flags=%d, stream=%d]",
                                (int)frame->hd.length,
                                frame->hd.flags, frame->hd.stream_id);
        }
        case NGHTTP2_RST_STREAM: {
            return apr_snprintf(buffer, maxlen,
                                "RST_STREAM[length=%d, flags=%d, stream=%d]",
                                (int)frame->hd.length,
                                frame->hd.flags, frame->hd.stream_id);
        }
        case NGHTTP2_SETTINGS: {
            if (frame->hd.flags & NGHTTP2_FLAG_ACK) {
                return apr_snprintf(buffer, maxlen,
                                    "SETTINGS[ack=1, stream=%d]",
                                    frame->hd.stream_id);
            }
            return apr_snprintf(buffer, maxlen,
                                "SETTINGS[length=%d, stream=%d]",
                                (int)frame->hd.length, frame->hd.stream_id);
        }
        case NGHTTP2_PUSH_PROMISE: {
            return apr_snprintf(buffer, maxlen,
                                "PUSH_PROMISE[length=%d, hend=%d, stream=%d]",
                                (int)frame->hd.length,
                                !!(frame->hd.flags & NGHTTP2_FLAG_END_HEADERS),
                                frame->hd.stream_id);
        }
        case NGHTTP2_PING: {
            return apr_snprintf(buffer, maxlen,
                                "PING[length=%d, ack=%d, stream=%d]",
                                (int)frame->hd.length,
                                frame->hd.flags&NGHTTP2_FLAG_ACK,
                                frame->hd.stream_id);
        }
        case NGHTTP2_GOAWAY: {
            size_t len = (frame->goaway.opaque_data_len < s_len)?
            frame->goaway.opaque_data_len : s_len-1;
            memcpy(scratch, frame->goaway.opaque_data, len);
            scratch[len+1] = '\0';
            return apr_snprintf(buffer, maxlen, "GOAWAY[error=%d, reason='%s']",
                                frame->goaway.error_code, scratch);
        }
        case NGHTTP2_WINDOW_UPDATE: {
            return apr_snprintf(buffer, maxlen,
                                "WINDOW_UPDATE[length=%d, stream=%d]",
                                (int)frame->hd.length, frame->hd.stream_id);
        }
        default:
            return apr_snprintf(buffer, maxlen,
                                "FRAME[type=%d, length=%d, flags=%d, stream=%d]",
                                frame->hd.type, (int)frame->hd.length,
                                frame->hd.flags, frame->hd.stream_id);
    }
}

