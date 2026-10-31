#if defined(_WIN32)
#include "net_compat.h"
#define SECURITY_WIN32
#include <windows.h>
#include <security.h>
#include <schannel.h>

#include "tls.h"
#include "proxy.h"
#include "logger.h"

struct win_tls_session {
    CtxtHandle context;
    SecPkgContext_StreamSizes sizes;
    char raw_buf[16384 + 512];   
    size_t raw_len;
    char plain_buf[16384];        
    size_t plain_len;
};

static CredHandle global_cred;
static int cred_initialized = 0;

int tls_init(void)
{
    if (cred_initialized) return 0;
    #ifndef SP_PROT_TLS1_3_CLIENT
    #define SP_PROT_TLS1_3_CLIENT 0x00002000
    #endif
    SCHANNEL_CRED cred_data = {0};
    cred_data.dwVersion = SCHANNEL_CRED_VERSION;
    cred_data.grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT | SP_PROT_TLS1_3_CLIENT;
    cred_data.dwFlags = SCH_CRED_AUTO_CRED_VALIDATION | SCH_CRED_USE_DEFAULT_CREDS;

    TimeStamp expiry;
    SECURITY_STATUS status = AcquireCredentialsHandleA(
        NULL,
        UNISP_NAME_A,             
        SECPKG_CRED_OUTBOUND,     
        NULL,
        &cred_data,
        NULL,
        NULL,
        &global_cred,
        &expiry
    );

    if (status != SEC_E_OK) return 1;

    cred_initialized = 1;
    return 0;
}

static ssize_t plain_read_wrapper(struct proxy_connection *conn, void *buf, size_t len)
{
    return recv(conn->upstream_fd,(char *)buf,(int)len,0);
}

static ssize_t plain_write_wrapper(struct proxy_connection *conn, void *buf, size_t len)
{
    return send(conn->upstream_fd,(char *)buf,(int)len,0);
}
void tls_cleanup(void)
{
    if (cred_initialized) {
        FreeCredentialsHandle(&global_cred);
        cred_initialized = 0;
    }
}

static ssize_t schannel_write_wrapper(struct proxy_connection *conn, void *buf, size_t len)
{
    struct win_tls_session *sess = (struct win_tls_session *)conn->upstream_ssl;

    if (len > sess->sizes.cbMaximumMessage) {
        len = sess->sizes.cbMaximumMessage;
    }

    size_t total_len = sess->sizes.cbHeader + len + sess->sizes.cbTrailer;
    char *packet = malloc(total_len);
    if (packet == NULL) return -1;

    memcpy(packet + sess->sizes.cbHeader, buf, len);

    SecBuffer buffers[4];
    buffers[0].BufferType = SECBUFFER_STREAM_HEADER;
    buffers[0].cbBuffer   = sess->sizes.cbHeader;
    buffers[0].pvBuffer   = packet;

    buffers[1].BufferType = SECBUFFER_DATA;
    buffers[1].cbBuffer   = len;
    buffers[1].pvBuffer   = packet + sess->sizes.cbHeader;

    buffers[2].BufferType = SECBUFFER_STREAM_TRAILER;
    buffers[2].cbBuffer   = sess->sizes.cbTrailer;
    buffers[2].pvBuffer   = packet + sess->sizes.cbHeader + len;

    buffers[3].BufferType = SECBUFFER_EMPTY;
    buffers[3].cbBuffer   = 0;
    buffers[3].pvBuffer   = NULL;

    SecBufferDesc desc = { SECBUFFER_VERSION, 4, buffers };

    SECURITY_STATUS status = EncryptMessage(&sess->context, 0, &desc, 0);
    if (status != SEC_E_OK) {
        free(packet);
        return -1;
    }

    int sent = send(conn->upstream_fd, packet, total_len, 0);
    free(packet);

    if (sent <= 0) return -1;

    return len;
}

static ssize_t schannel_read_wrapper(struct proxy_connection *conn, void *buf, size_t len)
{
    struct win_tls_session *sess = (struct win_tls_session *)conn->upstream_ssl;

    // 1. Serve leftover decrypted plaintext first
    if (sess->plain_len > 0) {
        size_t copy = (len < sess->plain_len) ? len : sess->plain_len;
        memcpy(buf, sess->plain_buf, copy);
        sess->plain_len -= copy;
        if (sess->plain_len > 0) {
            memmove(sess->plain_buf, sess->plain_buf + copy, sess->plain_len);
        }
        return copy;
    }

    // 2. Loop until at least one full TLS record is decrypted
    while (1)
    {
        if (sess->raw_len == 0) {
            int r = recv(conn->upstream_fd, sess->raw_buf, sizeof(sess->raw_buf), 0);
            if (r <= 0) return r;
            sess->raw_len = r;
        }

        SecBuffer in_buffers[4];
        in_buffers[0].BufferType = SECBUFFER_DATA;
        in_buffers[0].cbBuffer   = sess->raw_len;
        in_buffers[0].pvBuffer   = sess->raw_buf;
        in_buffers[1].BufferType = SECBUFFER_EMPTY;
        in_buffers[1].cbBuffer   = 0;
        in_buffers[1].pvBuffer   = NULL;
        in_buffers[2].BufferType = SECBUFFER_EMPTY;
        in_buffers[2].cbBuffer   = 0;
        in_buffers[2].pvBuffer   = NULL;
        in_buffers[3].BufferType = SECBUFFER_EMPTY;
        in_buffers[3].cbBuffer   = 0;
        in_buffers[3].pvBuffer   = NULL;

        SecBufferDesc in_desc = { SECBUFFER_VERSION, 4, in_buffers };

        SECURITY_STATUS status = DecryptMessage(&sess->context, &in_desc, 0, NULL);

        if (status == SEC_E_INCOMPLETE_MESSAGE) {
            int r = recv(conn->upstream_fd, sess->raw_buf + sess->raw_len,
                         sizeof(sess->raw_buf) - sess->raw_len, 0);
            if (r <= 0) return r;
            sess->raw_len += r;
            continue;
        }

        if (status == SEC_I_CONTEXT_EXPIRED) return 0;
        if (status != SEC_E_OK) return -1;

        SecBuffer *data_buf = NULL;
        SecBuffer *extra_buf = NULL;

        for (int i = 0; i < 4; i++) {
            if (in_buffers[i].BufferType == SECBUFFER_DATA && data_buf == NULL)
                data_buf = &in_buffers[i];
            if (in_buffers[i].BufferType == SECBUFFER_EXTRA && extra_buf == NULL)
                extra_buf = &in_buffers[i];
        }

        if (data_buf == NULL) return -1;

        // Shift leftover encrypted ciphertext to start of raw_buf
        if (extra_buf != NULL && extra_buf->cbBuffer > 0) {
            memmove(sess->raw_buf, extra_buf->pvBuffer, extra_buf->cbBuffer);
            sess->raw_len = extra_buf->cbBuffer;
        } else {
            sess->raw_len = 0;
        }

        // Copy decrypted plaintext
        size_t copy = (len < data_buf->cbBuffer) ? len : data_buf->cbBuffer;
        memcpy(buf, data_buf->pvBuffer, copy);

        if (data_buf->cbBuffer > copy) {
            sess->plain_len = data_buf->cbBuffer - copy;
            memcpy(sess->plain_buf, (char *)data_buf->pvBuffer + copy, sess->plain_len);
        }

        return copy;
    }
}


int tls_connect(struct proxy_connection *conn, const char *hostname, bool is_https)
{
    if(!is_https)
    {
        conn->read = plain_read_wrapper;
        conn->write = plain_write_wrapper;
        return 0;
    }
    struct win_tls_session *sess = calloc(1, sizeof(struct win_tls_session));
    if (sess == NULL) return 1;

    SecBuffer out_buf;
    out_buf.BufferType = SECBUFFER_TOKEN;
    out_buf.cbBuffer   = 0;
    out_buf.pvBuffer   = NULL;
    SecBufferDesc out_desc;
    out_desc.ulVersion = SECBUFFER_VERSION;
    out_desc.cBuffers  = 1;
    out_desc.pBuffers  = &out_buf;
    DWORD flags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | 
              ISC_REQ_CONFIDENTIALITY | ISC_REQ_ALLOCATE_MEMORY | 
              ISC_REQ_STREAM;

    DWORD attrs = 0;
    TimeStamp expiry;

    SECURITY_STATUS status = InitializeSecurityContextA(
        &global_cred,
        NULL,
        (SEC_CHAR *)hostname,
        flags,
        0, 0,
        NULL,
        0,
        &sess->context,
        &out_desc,
        &attrs,
        &expiry
    );
    if (out_buf.cbBuffer > 0 && out_buf.pvBuffer != NULL) {
        send(conn->upstream_fd, out_buf.pvBuffer, out_buf.cbBuffer, 0);
        FreeContextBuffer(out_buf.pvBuffer);
    }
    char in_buf[16384];
    int in_len = 0;

    while (status == SEC_I_CONTINUE_NEEDED || status == SEC_E_INCOMPLETE_MESSAGE)
    {
        if (in_len == 0 || status == SEC_E_INCOMPLETE_MESSAGE)
        {
            int r = recv(conn->upstream_fd, in_buf + in_len, sizeof(in_buf) - in_len, 0);
            if (r <= 0) {
                free(sess);
                return 1;
            }
            in_len += r;
        }
        SecBuffer in_buffers[2];
        in_buffers[0].BufferType = SECBUFFER_TOKEN;
        in_buffers[0].cbBuffer   = in_len;
        in_buffers[0].pvBuffer   = in_buf;
        in_buffers[1].BufferType = SECBUFFER_EMPTY;
        in_buffers[1].cbBuffer   = 0;
        in_buffers[1].pvBuffer   = NULL;

        SecBufferDesc in_desc = { SECBUFFER_VERSION, 2, in_buffers };

        out_buf.BufferType = SECBUFFER_TOKEN;
        out_buf.cbBuffer   = 0;
        out_buf.pvBuffer   = NULL;
        out_desc.cBuffers  = 1;
        out_desc.pBuffers  = &out_buf;

        status = InitializeSecurityContextA(
            &global_cred,
            &sess->context,       
            NULL,                 
            flags,
            0, 0,
            &in_desc,             
            0,
            NULL,
            &out_desc,            
            &attrs,
            &expiry
        );

        if (out_buf.cbBuffer > 0 && out_buf.pvBuffer != NULL) {
            send(conn->upstream_fd, out_buf.pvBuffer, out_buf.cbBuffer, 0);
            FreeContextBuffer(out_buf.pvBuffer);
        }

        if (status == SEC_E_INCOMPLETE_MESSAGE) {
            continue;
        }

        if (in_buffers[1].BufferType == SECBUFFER_EXTRA) {
            memmove(in_buf, in_buf + (in_len - in_buffers[1].cbBuffer), in_buffers[1].cbBuffer);
            in_len = in_buffers[1].cbBuffer;
        } else {
            in_len = 0;
        }
    }

    if (status != SEC_E_OK) {
        DeleteSecurityContext(&sess->context);
        free(sess);
        return 1;
    }
    if (in_len > 0) {
        memcpy(sess->raw_buf, in_buf, in_len);
        sess->raw_len = in_len;
    }
    QueryContextAttributesA(&sess->context, SECPKG_ATTR_STREAM_SIZES, &sess->sizes);
    conn->upstream_ssl = sess;
    conn->read = schannel_read_wrapper;
    conn->write = schannel_write_wrapper;
    return 0;

}

int conn_write_all(struct proxy_connection *conn, const void *buf, size_t total)
{
    size_t sent = 0;
    while (sent < total) {
        ssize_t n = conn->write(conn, (char *)buf + sent, total - sent);
        if (n <= 0) {
            int err = socket_errno;
            if (err == EAGAIN_COMPAT || err == EWOULDBLOCK_COMPAT)
                write_log(LOG_WARN, "Upstream server write timed out.");
            else
                write_log(LOG_WARN, "Upstream server closed connection unexpectedly.");
            return 1;
        }
        sent += n;
    }
    return 0;
}

void conn_close(struct proxy_connection *conn)
{
    if (conn == NULL) return;
    if (conn->upstream_ssl != NULL) {
        struct win_tls_session *sess = (struct win_tls_session *)conn->upstream_ssl;
        DeleteSecurityContext(&sess->context);
        free(sess);
        conn->upstream_ssl = NULL;
    }
    if (conn->upstream_fd >= 0) {
        close_socket(conn->upstream_fd);
        conn->upstream_fd = -1;
    }
}

#endif


