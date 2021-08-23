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

#ifndef mod_h2_h2_private_h
#define mod_h2_h2_private_h

#include <nghttp2/nghttp2.h>

extern module AP_MODULE_DECLARE_DATA h2_module;

APLOG_USE_MODULE(h2);

#define PROTO_H2_14          "h2-14"
#define PROTO_H2C_14         "h2c-14"

#define H2_HEADER_METHOD     ":method"
#define H2_HEADER_METHOD_LEN 7
#define H2_HEADER_SCHEME     ":scheme"
#define H2_HEADER_SCHEME_LEN 7
#define H2_HEADER_AUTH       ":authority"
#define H2_HEADER_AUTH_LEN   10
#define H2_HEADER_PATH       ":path"
#define H2_HEADER_PATH_LEN   5
#define H2_CRLF             "\r\n"

static const apr_off_t BLOCKSIZE      = 8 * 1024;
static const apr_off_t DATA_BLOCKSIZE = 8 * 1024;

#endif
