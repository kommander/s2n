/*
 * Copyright 2014 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

#include <sys/param.h>

#include <errno.h>
#include <s2n.h>

#include "error/s2n_errno.h"

#include "tls/s2n_cipher_suites.h"
#include "tls/s2n_connection.h"
#include "tls/s2n_record.h"
#include "tls/s2n_resume.h"
#include "tls/s2n_alerts.h"
#include "tls/s2n_tls.h"

#include "stuffer/s2n_stuffer.h"

#include "utils/s2n_safety.h"
#include "utils/s2n_socket.h"
#include "utils/s2n_random.h"

/* From RFC5246 7.4 */
#define TLS_HELLO_REQUEST       0
#define TLS_CLIENT_HELLO        1
#define TLS_SERVER_HELLO        2
#define TLS_SERVER_CERT         11
#define TLS_SERVER_KEY          12
#define TLS_SERVER_CERT_REQ     13
#define TLS_SERVER_HELLO_DONE   14
#define TLS_CLIENT_CERT         11  /* Same as SERVER_CERT */
#define TLS_CLIENT_CERT_VERIFY  15
#define TLS_CLIENT_KEY          16
#define TLS_CLIENT_FINISHED     20
#define TLS_SERVER_FINISHED     20  /* Same as CLIENT_FINISHED */
#define TLS_SERVER_CERT_STATUS  22

struct s2n_handshake_action {
    uint8_t record_type;
    uint8_t message_type;
    char writer;                /* 'S' or 'C' for server or client, 'B' for both */
    int (*handler[2]) (struct s2n_connection * conn);
};

/* Client and Server handlers for each message type we support.  
 * See http://www.iana.org/assignments/tls-parameters/tls-parameters.xhtml#tls-parameters-7 for the list of handshake message types
 */
static struct s2n_handshake_action state_machine[] = {
    /* message_type_t           = {Record type   Message type     Writer S2N_SERVER                S2N_CLIENT }  */
    [CLIENT_HELLO]              = {TLS_HANDSHAKE, TLS_CLIENT_HELLO, 'C', {s2n_client_hello_recv, s2n_client_hello_send}}, 
    [SERVER_HELLO]              = {TLS_HANDSHAKE, TLS_SERVER_HELLO, 'S', {s2n_server_hello_send, s2n_server_hello_recv}}, 
    [SERVER_CERT]               = {TLS_HANDSHAKE, TLS_SERVER_CERT, 'S', {s2n_server_cert_send, s2n_server_cert_recv}},
    [SERVER_CERT_STATUS]        = {TLS_HANDSHAKE, TLS_SERVER_CERT_STATUS, 'S', {s2n_server_status_send, s2n_server_status_recv}},
    [SERVER_KEY]                = {TLS_HANDSHAKE, TLS_SERVER_KEY, 'S', {s2n_server_key_send, s2n_server_key_recv}},
    [SERVER_CERT_REQ]           = {TLS_HANDSHAKE, TLS_SERVER_CERT_REQ, 'S', {NULL, NULL}},
    [SERVER_HELLO_DONE]         = {TLS_HANDSHAKE, TLS_SERVER_HELLO_DONE, 'S', {s2n_server_done_send, s2n_server_done_recv}}, 
    [CLIENT_CERT]               = {TLS_HANDSHAKE, TLS_CLIENT_CERT, 'C', {NULL, NULL}},
    [CLIENT_KEY]                = {TLS_HANDSHAKE, TLS_CLIENT_KEY, 'C', {s2n_client_key_recv, s2n_client_key_send}},
    [CLIENT_CERT_VERIFY]        = {TLS_HANDSHAKE, TLS_CLIENT_CERT_VERIFY, 'C', {NULL, NULL}},
    [CLIENT_CHANGE_CIPHER_SPEC] = {TLS_CHANGE_CIPHER_SPEC, 0, 'C', {s2n_client_ccs_recv, s2n_client_ccs_send}},
    [CLIENT_FINISHED]           = {TLS_HANDSHAKE, TLS_CLIENT_FINISHED, 'C', {s2n_client_finished_recv, s2n_client_finished_send}},
    [SERVER_CHANGE_CIPHER_SPEC] = {TLS_CHANGE_CIPHER_SPEC, 0, 'S', {s2n_server_ccs_send, s2n_server_ccs_recv}}, 
    [SERVER_FINISHED]           = {TLS_HANDSHAKE, TLS_SERVER_FINISHED, 'S', {s2n_server_finished_send, s2n_server_finished_recv}},
    [APPLICATION_DATA]          = {TLS_APPLICATION_DATA, 0, 'B', {NULL, NULL}}
};

/* We support 5 different ordering of messages, depending on what is being negotiated. There's also a dummy "INITIAL" handshake
 * that everything starts out as until we know better.
 */
static message_type_t handshakes[16][16] = {
    [INITIAL] = 
    {CLIENT_HELLO, SERVER_HELLO},

    [NEGOTIATED | RESUME ] =
    {CLIENT_HELLO, SERVER_HELLO, SERVER_CHANGE_CIPHER_SPEC,
     SERVER_FINISHED, CLIENT_CHANGE_CIPHER_SPEC, CLIENT_FINISHED, APPLICATION_DATA},

    [NEGOTIATED | FULL_HANDSHAKE ] =
    {CLIENT_HELLO, SERVER_HELLO, SERVER_CERT, SERVER_HELLO_DONE,
     CLIENT_KEY, CLIENT_CHANGE_CIPHER_SPEC, CLIENT_FINISHED,
     SERVER_CHANGE_CIPHER_SPEC, SERVER_FINISHED, APPLICATION_DATA},

    [NEGOTIATED | FULL_HANDSHAKE | PERFECT_FORWARD_SECRECY ] =
    {CLIENT_HELLO, SERVER_HELLO, SERVER_CERT, SERVER_KEY,
     SERVER_HELLO_DONE, CLIENT_KEY, CLIENT_CHANGE_CIPHER_SPEC, CLIENT_FINISHED,
     SERVER_CHANGE_CIPHER_SPEC, SERVER_FINISHED, APPLICATION_DATA},

    [NEGOTIATED | OCSP_STATUS ] =
    {CLIENT_HELLO, SERVER_HELLO, SERVER_CERT, SERVER_CERT_STATUS,
     SERVER_HELLO_DONE, CLIENT_KEY, CLIENT_CHANGE_CIPHER_SPEC, CLIENT_FINISHED,
     SERVER_CHANGE_CIPHER_SPEC, SERVER_FINISHED, APPLICATION_DATA},

    [NEGOTIATED | FULL_HANDSHAKE | PERFECT_FORWARD_SECRECY | OCSP_STATUS ] =
    {CLIENT_HELLO, SERVER_HELLO, SERVER_CERT, SERVER_CERT_STATUS,
     SERVER_KEY, SERVER_HELLO_DONE, CLIENT_KEY, CLIENT_CHANGE_CIPHER_SPEC,
     CLIENT_FINISHED, SERVER_CHANGE_CIPHER_SPEC, SERVER_FINISHED, APPLICATION_DATA}
};

#define ACTIVE_MESSAGE( conn ) handshakes[ (conn)->handshake.handshake_type ][ (conn)->handshake.message_number ]
#define PREVIOUS_MESSAGE( conn ) handshakes[ (conn)->handshake.handshake_type ][ (conn)->handshake.message_number - 1 ]

#define ACTIVE_STATE( conn ) state_machine[ ACTIVE_MESSAGE( (conn) ) ]
#define PREVIOUS_STATE( conn ) state_machine[ PREVIOUS_MESSAGE( (conn) ) ]

/* Used in our test cases */
message_type_t s2n_conn_get_current_message_type(struct s2n_connection *conn)
{
    return ACTIVE_MESSAGE(conn);
}

static int s2n_advance_message(struct s2n_connection *conn)
{
    char this = 'S';
    if (conn->mode == S2N_CLIENT) {
        this = 'C';
    }

    /* Actually advance the message number */
    conn->handshake.message_number++;

    /* If optimized io hasn't been enabled or if the caller started out with a corked socket,
     * we don't mess with it
     */
    if (!conn->corked_io || s2n_socket_was_corked(conn)) {
        return 0;
    }

    /* Are we changing I/O directions */
    if (ACTIVE_STATE(conn).writer == PREVIOUS_STATE(conn).writer) {
        return 0;
    }

    /* We're the new writer */
    if (ACTIVE_STATE(conn).writer == this) {
        if (conn->managed_io && conn->corked_io) {
            /* Set TCP_CORK/NOPUSH */
            GUARD(s2n_socket_write_cork(conn));
        }

        return 0;
    }

    /* We're the new reader, or we reached the "B" writer stage indicating that
       we're at the application data stage  - uncork the data */
    if (conn->managed_io && conn->corked_io) {
        GUARD(s2n_socket_write_uncork(conn));
    }

    return 0;
}

int s2n_conn_set_handshake_type(struct s2n_connection *conn)
{
    /* A handshake type has been negotiated */
    conn->handshake.handshake_type = NEGOTIATED;

    if (s2n_is_caching_enabled(conn->config)) {
        if (!s2n_resume_from_cache(conn)) {
            return 0;
        }

        if (conn->mode == S2N_SERVER) {
            struct s2n_blob session_id = {.data = conn->session_id,.size = S2N_TLS_SESSION_ID_MAX_LEN };

            /* Generate a new session id */
            GUARD(s2n_get_public_random_data(&session_id));
            conn->session_id_len = S2N_TLS_SESSION_ID_MAX_LEN;
        }
    }

    /* If we get this far, it's a full handshake */
    conn->handshake.handshake_type |= FULL_HANDSHAKE;

    if (conn->secure.cipher_suite->key_exchange_alg->flags & S2N_KEY_EXCHANGE_EPH) {
        conn->handshake.handshake_type |= PERFECT_FORWARD_SECRECY;
    }

    if (s2n_server_can_send_ocsp(conn)) {
        conn->handshake.handshake_type |= OCSP_STATUS;
    }

    return 0;
}

static int s2n_conn_update_handshake_hashes(struct s2n_connection *conn, struct s2n_blob *data)
{
    GUARD(s2n_hash_update(&conn->handshake.md5, data->data, data->size));
    GUARD(s2n_hash_update(&conn->handshake.sha1, data->data, data->size));
    GUARD(s2n_hash_update(&conn->handshake.sha256, data->data, data->size));
    GUARD(s2n_hash_update(&conn->handshake.sha384, data->data, data->size));

    return 0;
}

/* Writing is relatively straight forward, simply write each message out as a record,
 * we may fragment a message across multiple records, but we never coalesce multiple
 * messages into single records. 
 * Precondition: secure outbound I/O has already been flushed
 */
static int handshake_write_io(struct s2n_connection *conn)
{
    uint8_t record_type = ACTIVE_STATE(conn).record_type;
    s2n_blocked_status blocked = S2N_NOT_BLOCKED;

    /* Populate handshake.io with header/payload for the current state, once.
     * Check wiped instead of s2n_stuffer_data_available to differentiate between the initial call
     * to handshake_write_io and a repeated call after an EWOULDBLOCK.
     */
    if (conn->handshake.io.wiped == 1) {
        if (record_type == TLS_HANDSHAKE) {
            GUARD(s2n_handshake_write_header(conn, ACTIVE_STATE(conn).message_type));
        }
        GUARD(ACTIVE_STATE(conn).handler[conn->mode] (conn));
        if (record_type == TLS_HANDSHAKE) {
            GUARD(s2n_handshake_finish_header(conn));
        }
    }

    /* Write the handshake data to records in fragment sized chunks */
    struct s2n_blob out;
    while (s2n_stuffer_data_available(&conn->handshake.io) > 0) {
        int max_payload_size;
        GUARD((max_payload_size = s2n_record_max_write_payload_size(conn)));
        out.size = MIN(s2n_stuffer_data_available(&conn->handshake.io), max_payload_size);

        out.data = s2n_stuffer_raw_read(&conn->handshake.io, out.size);
        notnull_check(out.data);

        /* Make the actual record */
        GUARD(s2n_record_write(conn, record_type, &out));

        /* MD5 and SHA sum the handshake data too */
        if (record_type == TLS_HANDSHAKE) {
            GUARD(s2n_conn_update_handshake_hashes(conn, &out));
        }

        /* Actually send the record. We could block here. Assume the caller will call flush before coming back. */
        GUARD(s2n_flush(conn, &blocked));
    }

    /* We're done sending the last record, reset everything */
    GUARD(s2n_stuffer_wipe(&conn->out));
    GUARD(s2n_stuffer_wipe(&conn->handshake.io));

    /* Advance the state machine */
    GUARD(s2n_advance_message(conn));

    return 0;
}

/*
 * Returns:
 *  1  - more data is needed to complete the handshake message.
 *  0  - we read the whole handshake message.
 * -1  - error processing the handshake message.
 */
static int read_full_handshake_message(struct s2n_connection *conn, uint8_t * message_type)
{
    uint32_t current_handshake_data = s2n_stuffer_data_available(&conn->handshake.io);
    if (current_handshake_data < TLS_HANDSHAKE_HEADER_LENGTH) {
        /* The message may be so badly fragmented that we don't even read the full header, take
         * what we can and then continue to the next record read iteration. 
         */
        if (s2n_stuffer_data_available(&conn->in) < (TLS_HANDSHAKE_HEADER_LENGTH - current_handshake_data)) {
            GUARD(s2n_stuffer_copy(&conn->in, &conn->handshake.io, s2n_stuffer_data_available(&conn->in)));
            return 1;
        }

        /* Get the remainder of the header */
        GUARD(s2n_stuffer_copy(&conn->in, &conn->handshake.io, (TLS_HANDSHAKE_HEADER_LENGTH - current_handshake_data)));
    }

    uint32_t handshake_message_length;
    GUARD(s2n_handshake_parse_header(conn, message_type, &handshake_message_length));

    if (handshake_message_length > S2N_MAXIMUM_HANDSHAKE_MESSAGE_LENGTH) {
        S2N_ERROR(S2N_ERR_BAD_MESSAGE);
    }

    uint32_t bytes_to_take = handshake_message_length - s2n_stuffer_data_available(&conn->handshake.io);
    bytes_to_take = MIN(bytes_to_take, s2n_stuffer_data_available(&conn->in));

    /* If the record is handshake data, add it to the handshake buffer */
    GUARD(s2n_stuffer_copy(&conn->in, &conn->handshake.io, bytes_to_take));

    /* If we have the whole handshake message, then success */
    if (s2n_stuffer_data_available(&conn->handshake.io) == handshake_message_length) {
        struct s2n_blob handshake;
        handshake.data = conn->handshake.io.blob.data;
        handshake.size = TLS_HANDSHAKE_HEADER_LENGTH + handshake_message_length;

        notnull_check(handshake.data);

        /* MD5 and SHA sum the handshake data too */
        GUARD(s2n_conn_update_handshake_hashes(conn, &handshake));

        return 0;
    }

    /* We don't have the whole message, so we'll need to go again */
    GUARD(s2n_stuffer_reread(&conn->handshake.io));

    return 1;
}

/* Reading is a little more complicated than writing as the TLS RFCs allow content
 * types to be interleaved at the record layer. We may get an alert message
 * during the handshake phase, or messages of types that we don't support (e.g.
 * HEARTBEAT messages), or during renegotiations we may even get application
 * data messages that need to be handled by the application. The latter is punted
 * for now (s2n does support renegotiations).
 */
static int handshake_read_io(struct s2n_connection *conn)
{
    uint8_t record_type;
    int isSSLv2;

    GUARD(s2n_read_full_record(conn, &record_type, &isSSLv2));

    if (isSSLv2) {
        if (ACTIVE_MESSAGE(conn) != CLIENT_HELLO) {
            S2N_ERROR(S2N_ERR_BAD_MESSAGE);
        }

        /* Add the message to our handshake hashes */
        struct s2n_blob hashed = {.data = conn->header_in.blob.data + 2,.size = 3 };
        GUARD(s2n_conn_update_handshake_hashes(conn, &hashed));

        hashed.data = conn->in.blob.data;
        hashed.size = s2n_stuffer_data_available(&conn->in);
        GUARD(s2n_conn_update_handshake_hashes(conn, &hashed));

        /* Handle an SSLv2 client hello */
        GUARD(s2n_stuffer_copy(&conn->in, &conn->handshake.io, s2n_stuffer_data_available(&conn->in)));
        GUARD(s2n_sslv2_client_hello_recv(conn));
        GUARD(s2n_stuffer_wipe(&conn->handshake.io));

        /* We're done with the record, wipe it */
        GUARD(s2n_stuffer_wipe(&conn->header_in));
        GUARD(s2n_stuffer_wipe(&conn->in));
        conn->in_status = ENCRYPTED;

        /* Advance the state machine */
        GUARD(s2n_advance_message(conn));
    }

    /* Now we have a record, but it could be a partial fragment of a message, or it might
     * contain several messages.
     */
    if (record_type == TLS_APPLICATION_DATA) {
        S2N_ERROR(S2N_ERR_BAD_MESSAGE);
    } else if (record_type == TLS_CHANGE_CIPHER_SPEC) {
        if (s2n_stuffer_data_available(&conn->in) != 1) {
            S2N_ERROR(S2N_ERR_BAD_MESSAGE);
        }

        GUARD(s2n_stuffer_copy(&conn->in, &conn->handshake.io, s2n_stuffer_data_available(&conn->in)));
        GUARD(ACTIVE_STATE(conn).handler[conn->mode] (conn));
        GUARD(s2n_stuffer_wipe(&conn->handshake.io));

        /* We're done with the record, wipe it */
        GUARD(s2n_stuffer_wipe(&conn->header_in));
        GUARD(s2n_stuffer_wipe(&conn->in));
        conn->in_status = ENCRYPTED;

        /* Advance the state machine */
        GUARD(s2n_advance_message(conn));

        return 0;
    } else if (record_type != TLS_HANDSHAKE) {
        if (record_type == TLS_ALERT) {
            GUARD(s2n_process_alert_fragment(conn));
        }

        /* Ignore record types that we don't support */

        /* We're done with the record, wipe it */
        GUARD(s2n_stuffer_wipe(&conn->header_in));
        GUARD(s2n_stuffer_wipe(&conn->in));
        conn->in_status = ENCRYPTED;
        return 0;
    }

    /* Record is a handshake message */
    while (s2n_stuffer_data_available(&conn->in)) {
        int r;
        uint8_t handshake_message_type;
        GUARD((r = read_full_handshake_message(conn, &handshake_message_type)));

        /* Do we need more data? */
        if (r == 1) {
            /* Break out of this inner loop, but since we're not changing the state, the
             * outer loop in s2n_handshake_io() will read another record. 
             */
            GUARD(s2n_stuffer_wipe(&conn->header_in));
            GUARD(s2n_stuffer_wipe(&conn->in));
            conn->in_status = ENCRYPTED;
            return 0;
        }

        if (handshake_message_type != ACTIVE_STATE(conn).message_type) {
            S2N_ERROR(S2N_ERR_BAD_MESSAGE);
        }

        /* Call the relevant handler */
        r = ACTIVE_STATE(conn).handler[conn->mode] (conn);
        GUARD(s2n_stuffer_wipe(&conn->handshake.io));

        if (r < 0) {
            GUARD(s2n_connection_kill(conn));

            return r;
        }

        /* Advance the state machine */
        GUARD(s2n_advance_message(conn));
    }

    /* We're done with the record, wipe it */
    GUARD(s2n_stuffer_wipe(&conn->header_in));
    GUARD(s2n_stuffer_wipe(&conn->in));
    conn->in_status = ENCRYPTED;

    return 0;
}

int s2n_negotiate(struct s2n_connection *conn, s2n_blocked_status * blocked)
{
    char this = 'S';
    if (conn->mode == S2N_CLIENT) {
        this = 'C';
    }

    while (ACTIVE_STATE(conn).writer != 'B') {

        /* Flush any pending I/O or alert messages */
        GUARD(s2n_flush(conn, blocked));

        if (ACTIVE_STATE(conn).writer == this) {
            *blocked = S2N_BLOCKED_ON_WRITE;
            GUARD(handshake_write_io(conn));
        } else {
            *blocked = S2N_BLOCKED_ON_READ;
            if (handshake_read_io(conn) < 0) {
                if (s2n_errno != S2N_ERR_BLOCKED && s2n_is_caching_enabled(conn->config) && conn->session_id_len) {
                    conn->config->cache_delete(conn->config->cache_delete_data, conn->session_id, conn->session_id_len);
                }

                return -1;
            }
        }

        /* If the handshake has just ended, free up memory */
        if (ACTIVE_STATE(conn).writer == 'B') {
            GUARD(s2n_stuffer_resize(&conn->handshake.io, 0));
        }
    }

    *blocked = S2N_NOT_BLOCKED;

    return 0;
}
