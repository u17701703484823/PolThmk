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

#ifndef __mod_h2__h2_h2__
#define __mod_h2__h2_h2__

/**
 * List of ALPN protocol identifiers that we suport in cleartext
 * negotiations. NULL terminated.
 */
extern const char *h2_clear_protos[];

/**
 * List of ALPN protocol identifiers that we support in TLS encrypted 
 * negotiations. NULL terminated.
 */
extern const char *h2_tls_protos[];

/**
 * The magic PRIamble of RFC 7540 that is always sent when starting
 * a h2 communication.
 */
extern const char *H2_MAGIC_TOKEN;

#define H2_ERR_NO_ERROR             (0x00)
#define H2_ERR_PROTOCOL_ERROR       (0x01)
#define H2_ERR_INTERNAL_ERROR       (0x02)
#define H2_ERR_FLOW_CONTROL_ERROR   (0x03)
#define H2_ERR_SETTINGS_TIMEOUT     (0x04)
#define H2_ERR_STREAM_CLOSED        (0x05)
#define H2_ERR_FRAME_SIZE_ERROR     (0x06)
#define H2_ERR_REFUSED_STREAM       (0x07)
#define H2_ERR_CANCEL               (0x08)
#define H2_ERR_COMPRESSION_ERROR    (0x09)
#define H2_ERR_CONNECT_ERROR        (0x0a)
#define H2_ERR_ENHANCE_YOUR_CALM    (0x0b)
#define H2_ERR_INADEQUATE_SECURITY  (0x0c)
#define H2_ERR_HTTP_1_1_REQUIRED    (0x0d)

/* Maximum number of padding bytes in a frame, rfc7540 */
#define H2_MAX_PADLEN               256

/**
 * Provide a user readable description of the HTTP/2 error code-
 * @param h2_error http/2 error code, as in rfc 7540, ch. 7
 * @return textual description of code or that it is unknown.
 */
const char *h2_h2_err_description(int h2_error);

/*
 * One time, post config intialization.
 */
apr_status_t h2_h2_init(apr_pool_t *pool, server_rec *s);

/* Is the connection a TLS connection?
 */
int h2_h2_is_tls(conn_rec *c);

/* Register apache hooks for h2 protocol
 */
void h2_h2_register_hooks(void);

/**
 * Check if the given connection fulfills the requirements as configured.
 * @param c the connection
 * @param require_all != 0 iff any missing connection properties make
 *    the test fail. For example, a cipher might not have been selected while
 *    the handshake is still ongoing.
 * @return != 0 iff connection requirements are met
 */
int h2_is_acceptable_connection(conn_rec *c, int require_all);

/**
 * Check if the "direct" HTTP/2 mode of protocol handling is enabled
 * for the given connection.
 * @param c the connection to check
 * @return != 0 iff direct mode is enabled
 */
int h2_allows_h2_direct(conn_rec *c);

/**
 * Check if the "Upgrade" HTTP/1.1 mode of protocol switching is enabled
 * for the given connection.
 * @param c the connection to check
 * @return != 0 iff Upgrade switching is enabled
 */
int h2_allows_h2_upgrade(conn_rec *c);


#endif /* defined(__mod_h2__h2_h2__) */
