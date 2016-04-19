/*
 * Copyright (c) 2015 Cisco and/or its affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* for htonl */
#include <arpa/inet.h>
/* for send */
#include <sys/socket.h>
#include "msghandler.h"

#define VERSION_STRING "TestVersion 0.000001"

void vl_api_rpc_call_main_thread (void *fp, u8 * data, u32 data_length);


static void vpp_get_version_rpc_callback(void *args)
{
    ASSERT(os_get_cpu_number() == 0);
    char * vpe_api_get_version (void);
    char **pversion = args;

    clib_warning("MAIN THREAD: %p (%p)", *pversion, pversion);
    // FIXME: undefined reference to vpe_api_get_version() ?!
    *pversion = vpe_api_get_version();
}

// warning: passing reference to local static buffer
// do not call get version if the response was not encoded to buffer yet
static int protobuf_req_get_version(protobuf_client_t *client, VppResponse *resp)
{
    VppVersionResp msg = VPP_VERSION_RESP__INIT;
    //static char *version = NULL;
    static uint8_t *payload = NULL;

    resp->type = RESPONSE_TYPE__VPP_VERSION;

    // if version payload is already prepared
    if (payload != NULL) {
        resp->payload.len = vec_len(payload);
        resp->payload.data = payload;
        resp->has_payload = 1;
        return 0;
    }

    // TODO: call vpp main thread to get vpp version & wait for response
    //vl_api_rpc_call_main_thread (vpp_get_version_rpc_callback, (u8 *)&version, sizeof(&version));

    msg.version = VERSION_STRING;

    size_t packed_size = vpp_version_resp__get_packed_size(&msg);
    vec_validate(payload, packed_size - 1);
    vpp_version_resp__pack(&msg, payload);
    _vec_len(payload) = packed_size;

    resp->payload.len = vec_len(payload);
    resp->payload.data = payload;
    resp->has_payload = 1;
    return 0;
}

int protobuf_handle_request(protobuf_client_t *client, VppRequest *req)
{
    protobuf_main_t *pbmain = &protobuf_main;
    VppResponse resp = VPP_RESPONSE__INIT;
    resp.id = req->id;

    switch(req->type) {
        case REQUEST_TYPE__GET_VERSION:
            clib_warning("received get version request from %s:%d", client->address, client->port);
            resp.retcode = protobuf_req_get_version(client, &resp);
            // response references static buffer!
            break;
        default:
            clib_warning("unknown request type %d from %s:%d", req->type, client->address, client->port);
            resp.retcode = -1;   // FIXME
            break;
    }

    size_t packed_size = vpp_response__get_packed_size(&resp);
    // FIXME: write buffer may not be empty
    vec_validate(client->buf_write, packed_size);
    vpp_response__pack(&resp, client->buf_write);
    _vec_len(client->buf_write) = packed_size;

    ssize_t sent = 0;
    uint32_t ps = htonl(packed_size);

    clib_warning("prepared response");

    // FIXME & TODO: this needs to be handled in write event
    while (1) {
        sent = send(client->fd, &ps, sizeof(ps), 0);
        if (sent <= 0) {
            if (errno == EAGAIN) {
                usleep(100000);
                continue;
            }

            clib_warning("error sending response to %s:%d for message id %d (%s)", client->address, client->port, req->id, strerror(errno));
            protobuf_client_disconnect(client);
            return -1; // FIXME
        }
        break;
    }

    clib_warning("sent size: %d", packed_size);

    ssize_t total_sent = 0;
    while (total_sent < packed_size) {
        sent = send(client->fd, client->buf_write + total_sent, packed_size - total_sent, 0);
        if (sent <= 0) {
            if (errno == EAGAIN) {
                usleep(100000);
                continue;
            }

            clib_warning("error sending response to %s:%d for message id %d (%s)", client->address, client->port, req->id, strerror(errno));
            protobuf_client_disconnect(client);
            return -1; // FIXME
        }

        total_sent += sent;
        if (total_sent < packed_size)
            usleep(100000); // sleep 100 ms
    }

    clib_warning("sent whole message");

    return 0;
}

