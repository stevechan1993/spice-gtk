#include "spice-client.h"
#include "spice-common.h"

#include "spice-channel-priv.h"
#include "spice-session-priv.h"
#include "spice-marshal.h"

#include <openssl/rsa.h>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

#include <sys/socket.h>

static void spice_channel_send_msg(SpiceChannel *channel, spice_msg_out *out);
static void spice_channel_send_link(SpiceChannel *channel);

/* ------------------------------------------------------------------ */
/* gobject glue                                                       */

#define SPICE_CHANNEL_GET_PRIVATE(obj)                                  \
    (G_TYPE_INSTANCE_GET_PRIVATE ((obj), SPICE_TYPE_CHANNEL, spice_channel))

G_DEFINE_TYPE (SpiceChannel, spice_channel, G_TYPE_OBJECT);

/* Properties */
enum {
    PROP_0,
    PROP_SESSION,
    PROP_CHANNEL_TYPE,
    PROP_CHANNEL_ID,
};

/* Signals */
enum {
    SPICE_CHANNEL_EVENT,

    SPICE_CHANNEL_LAST_SIGNAL,
};

static guint signals[SPICE_CHANNEL_LAST_SIGNAL];

static void spice_channel_init(SpiceChannel *channel)
{
    spice_channel *c;

    fprintf(stderr, "%s\n", __FUNCTION__);

    c = channel->priv = SPICE_CHANNEL_GET_PRIVATE(channel);

    c->serial = 1;
    c->socket = -1;
    c->protocol = SPICE_VERSION_MAJOR;
    strcpy(c->name, "?");
}

static void spice_channel_constructed(GObject *gobject)
{
    SpiceChannel *channel = SPICE_CHANNEL(gobject);
    spice_channel *c = SPICE_CHANNEL_GET_PRIVATE(channel);

    snprintf(c->name, sizeof(c->name), "%d:%d",
             c->channel_type, c->channel_id);
    fprintf(stderr, "%s %s\n", __FUNCTION__, c->name);

    c->connection_id = spice_session_get_connection_id(c->session);
    spice_session_channel_new(c->session, channel);

    /* Chain up to the parent class */
    if (G_OBJECT_CLASS(spice_channel_parent_class)->constructed)
        G_OBJECT_CLASS(spice_channel_parent_class)->constructed(gobject);
}

static void spice_channel_dispose(GObject *gobject)
{
    SpiceChannel *channel = SPICE_CHANNEL(gobject);
    spice_channel *c = SPICE_CHANNEL_GET_PRIVATE(channel);

    fprintf(stderr, "%s %s\n", __FUNCTION__, c->name);
    spice_channel_disconnect(channel, SPICE_CHANNEL_CLOSED);
    spice_session_channel_destroy(c->session, channel);

    /* Chain up to the parent class */
    if (G_OBJECT_CLASS(spice_channel_parent_class)->dispose)
        G_OBJECT_CLASS(spice_channel_parent_class)->dispose(gobject);
}

static void spice_channel_finalize(GObject *gobject)
{
    SpiceChannel *channel = SPICE_CHANNEL(gobject);
    spice_channel *c = SPICE_CHANNEL_GET_PRIVATE(channel);

    fprintf(stderr, "%s %s\n", __FUNCTION__, c->name);

    /* Chain up to the parent class */
    if (G_OBJECT_CLASS(spice_channel_parent_class)->finalize)
        G_OBJECT_CLASS(spice_channel_parent_class)->finalize(gobject);
}

static void spice_channel_get_property(GObject    *gobject,
                                       guint       prop_id,
                                       GValue     *value,
                                       GParamSpec *pspec)
{
    SpiceChannel *channel = SPICE_CHANNEL(gobject);
    spice_channel *c = SPICE_CHANNEL_GET_PRIVATE(channel);

    switch (prop_id) {
    case PROP_SESSION:
        g_value_set_object(value, c->session);
	break;
    case PROP_CHANNEL_TYPE:
        g_value_set_int(value, c->channel_type);
	break;
    case PROP_CHANNEL_ID:
        g_value_set_int(value, c->channel_id);
	break;
    default:
	G_OBJECT_WARN_INVALID_PROPERTY_ID(gobject, prop_id, pspec);
	break;
    }
}

static void spice_channel_set_property(GObject      *gobject,
                                       guint         prop_id,
                                       const GValue *value,
                                       GParamSpec   *pspec)
{
    SpiceChannel *channel = SPICE_CHANNEL(gobject);
    spice_channel *c = SPICE_CHANNEL_GET_PRIVATE(channel);

    switch (prop_id) {
    case PROP_SESSION:
        c->session = g_value_get_object(value);
        break;
    case PROP_CHANNEL_TYPE:
        c->channel_type = g_value_get_int(value);
        break;
    case PROP_CHANNEL_ID:
        c->channel_id = g_value_get_int(value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(gobject, prop_id, pspec);
        break;
    }
}

static void spice_channel_class_init(SpiceChannelClass *klass)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

    fprintf(stderr, "%s\n", __FUNCTION__);

    gobject_class->constructed  = spice_channel_constructed;
    gobject_class->dispose      = spice_channel_dispose;
    gobject_class->finalize     = spice_channel_finalize;
    gobject_class->get_property = spice_channel_get_property;
    gobject_class->set_property = spice_channel_set_property;

    g_object_class_install_property
        (gobject_class, PROP_SESSION,
         g_param_spec_object("spice-session",
                             "Spice session",
                             "",
                             SPICE_TYPE_SESSION,
                             G_PARAM_READWRITE |
                             G_PARAM_CONSTRUCT_ONLY |
                             G_PARAM_STATIC_NAME |
                             G_PARAM_STATIC_NICK |
                             G_PARAM_STATIC_BLURB));

    g_object_class_install_property
        (gobject_class, PROP_CHANNEL_TYPE,
         g_param_spec_int("channel-type",
                          "Channel type",
                          "",
                          -1, INT_MAX, -1,
                          G_PARAM_READWRITE |
                          G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_NAME |
                          G_PARAM_STATIC_NICK |
                          G_PARAM_STATIC_BLURB));

    g_object_class_install_property
        (gobject_class, PROP_CHANNEL_ID,
         g_param_spec_int("channel-id",
                          "Channel ID",
                          "",
                          -1, INT_MAX, -1,
                          G_PARAM_READWRITE |
                          G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_NAME |
                          G_PARAM_STATIC_NICK |
                          G_PARAM_STATIC_BLURB));

    signals[SPICE_CHANNEL_EVENT] =
        g_signal_new("spice-channel-event",
                     G_OBJECT_CLASS_TYPE(gobject_class),
                     G_SIGNAL_RUN_FIRST,
                     G_STRUCT_OFFSET(SpiceChannelClass, spice_channel_event),
                     NULL, NULL,
                     g_cclosure_marshal_VOID__INT,
                     G_TYPE_NONE,
                     1,
                     G_TYPE_INT);

    g_type_class_add_private(klass, sizeof(spice_channel));

    SSL_library_init();
    SSL_load_error_strings();
}

static void spice_channel_emit_event(SpiceChannel *channel,
                                     enum SpiceChannelEvent event)
{
    g_signal_emit(channel, signals[SPICE_CHANNEL_EVENT], 0, event);
}

/* ---------------------------------------------------------------- */

spice_msg_in *spice_msg_in_new(SpiceChannel *channel)
{
    spice_msg_in *in;

    in = spice_new0(spice_msg_in, 1);
    in->refcount = 1;
    in->channel  = channel;
    return in;
}

spice_msg_in *spice_msg_in_sub_new(SpiceChannel *channel, spice_msg_in *parent,
                                   SpiceSubMessage *sub)
{
    spice_msg_in *in;

    in = spice_msg_in_new(channel);
    in->header.type = sub->type;
    in->header.size = sub->size;
    in->data = (uint8_t*)(sub+1);
    in->dpos = sub->size;
    in->parent = parent;
    spice_msg_in_ref(parent);
    return in;
}

void spice_msg_in_ref(spice_msg_in *in)
{
    in->refcount++;
}

void spice_msg_in_unref(spice_msg_in *in)
{
    in->refcount--;
    if (in->refcount > 0)
        return;
    if (in->parsed)
        in->pfree(in->parsed);
    if (in->parent) {
        spice_msg_in_unref(in->parent);
    } else {
        free(in->data);
    }
    free(in);
}

int spice_msg_in_type(spice_msg_in *in)
{
    return in->header.type;
}

void *spice_msg_in_parsed(spice_msg_in *in)
{
    return in->parsed;
}

void *spice_msg_in_raw(spice_msg_in *in, int *len)
{
    *len = in->dpos;
    return in->data;
}

static void hexdump(char *prefix, unsigned char *data, int len)
{
    int i;

    for (i = 0; i < len; i++) {
        if (i % 16 == 0)
            fprintf(stderr, "%s:", prefix);
        if (i % 4 == 0)
            fprintf(stderr, " ");
        fprintf(stderr, " %02x", data[i]);
        if (i % 16 == 15)
            fprintf(stderr, "\n");
    }
    if (i % 16 != 0)
        fprintf(stderr, "\n");
}

void spice_msg_in_hexdump(spice_msg_in *in)
{
    spice_channel *c = SPICE_CHANNEL_GET_PRIVATE(in->channel);

    fprintf(stderr, "--\n<< hdr: %s serial %ld type %d size %d sub-list %d\n",
            c->name, in->header.serial, in->header.type,
            in->header.size, in->header.sub_list);
    hexdump("<< msg", in->data, in->dpos);
}

void spice_msg_out_hexdump(spice_msg_out *out, unsigned char *data, int len)
{
    spice_channel *c = SPICE_CHANNEL_GET_PRIVATE(out->channel);

    fprintf(stderr, "--\n>> hdr: %s serial %ld type %d size %d sub-list %d\n",
            c->name, out->header->serial, out->header->type,
            out->header->size, out->header->sub_list);
    hexdump(">> msg", data, len);
}

/* ---------------------------------------------------------------- */

spice_msg_out *spice_msg_out_new(SpiceChannel *channel, int type)
{
    spice_channel *c = SPICE_CHANNEL_GET_PRIVATE(channel);
    spice_msg_out *out;

    out = spice_new0(spice_msg_out, 1);
    out->refcount = 1;
    out->channel  = channel;

    out->marshallers = c->marshallers;
    out->marshaller = spice_marshaller_new();
    out->header = (SpiceDataHeader *)
        spice_marshaller_reserve_space(out->marshaller, sizeof(SpiceDataHeader));
    spice_marshaller_set_base(out->marshaller, sizeof(SpiceDataHeader));
    out->header->serial = c->serial++;
    out->header->type = type;
    out->header->sub_list = 0;
    return out;
}

void spice_msg_out_ref(spice_msg_out *out)
{
    out->refcount++;
}

void spice_msg_out_unref(spice_msg_out *out)
{
    out->refcount--;
    if (out->refcount > 0)
        return;
    spice_marshaller_destroy(out->marshaller);
    free(out);
}

void spice_msg_out_send(spice_msg_out *out)
{
    out->header->size =
        spice_marshaller_get_total_size(out->marshaller) - sizeof(SpiceDataHeader);
    spice_channel_send_msg(out->channel, out);
}

/* ---------------------------------------------------------------- */

static int spice_channel_send(SpiceChannel *channel, void *buf, int len)
{
    spice_channel *c = SPICE_CHANNEL_GET_PRIVATE(channel);

    if (c->tls) {
        return SSL_write(c->ssl, buf, len);
    } else {
        return send(c->socket, buf, len, 0);
    }
}

static int spice_channel_recv(SpiceChannel *channel, void *buf, int len)
{
    spice_channel *c = SPICE_CHANNEL_GET_PRIVATE(channel);
    int rc, err;

    if (c->tls) {
        rc = SSL_read(c->ssl, buf, len);
        if (rc > 0) {
            return rc;
        }
        if (rc == 0) {
            fprintf(stderr, "channel/tls eof: %s\n", c->name);
            spice_channel_disconnect(channel, SPICE_CHANNEL_CLOSED);
            return 0;
        }
        err = SSL_get_error(c->ssl, rc);
        if (err == SSL_ERROR_WANT_READ) {
            return 0;
        }
        fprintf(stderr, "channel/tls error: %s: %s\n",
                c->name, ERR_error_string(err, NULL));
        spice_channel_disconnect(channel, SPICE_CHANNEL_ERROR_IO);
        return 0;
    } else {
        rc = recv(c->socket, buf, len, 0);
        switch (rc) {
        case -1:
            if (errno == EAGAIN)
                return 0;
            fprintf(stderr, "channel error: %s: %s\n",
                    c->name, strerror(errno));
            spice_channel_disconnect(channel, SPICE_CHANNEL_ERROR_IO);
            return 0;
        case 0:
            fprintf(stderr, "channel eof: %s\n", c->name);
            spice_channel_disconnect(channel, SPICE_CHANNEL_CLOSED);
            return 0;
        default:
            return rc;
        }
    }
}

static void spice_channel_tls_connect(SpiceChannel *channel)
{
    spice_channel *c = SPICE_CHANNEL_GET_PRIVATE(channel);
    int rc, err;

    rc = SSL_connect(c->ssl);
    if (rc <= 0) {
        err = SSL_get_error(c->ssl, rc);
        if (err == SSL_ERROR_WANT_READ) {
            return;
        }
        fprintf(stderr, "SSL_connect: %s", ERR_error_string(err, NULL));
        spice_channel_emit_event(channel, SPICE_CHANNEL_ERROR_TLS);
    }
    c->state = SPICE_CHANNEL_STATE_LINK_HDR;
    spice_channel_send_link(channel);
}

static void spice_channel_send_auth(SpiceChannel *channel)
{
    spice_channel *c = SPICE_CHANNEL_GET_PRIVATE(channel);
    EVP_PKEY *pubkey;
    int nRSASize;
    BIO *bioKey;
    RSA *rsa;
    const char *password;
    uint8_t *encrypted;
    int rc;

    bioKey = BIO_new(BIO_s_mem());
    if (bioKey == NULL)
        PANIC("Could not initiate BIO");

    BIO_write(bioKey, c->peer_msg->pub_key, SPICE_TICKET_PUBKEY_BYTES);
    pubkey = d2i_PUBKEY_bio(bioKey, NULL);
    rsa = pubkey->pkey.rsa;
    nRSASize = RSA_size(rsa);

    encrypted = spice_malloc(nRSASize);
    /*
      The use of RSA encryption limit the potential maximum password length.
      for RSA_PKCS1_OAEP_PADDING it is RSA_size(rsa) - 41.
    */
    g_object_get(c->session, "password", &password, NULL);
    if (password == NULL)
        password = "";
    rc = RSA_public_encrypt(strlen(password) + 1, (uint8_t*)password,
                            encrypted, rsa, RSA_PKCS1_OAEP_PADDING);
    if (rc <= 0)
        PANIC("could not encrypt password");
    spice_channel_send(channel, encrypted, nRSASize);
    memset(encrypted, 0, nRSASize);
    free(encrypted);
    BIO_free(bioKey);
}

static void spice_channel_recv_auth(SpiceChannel *channel)
{
    spice_channel *c = SPICE_CHANNEL_GET_PRIVATE(channel);
    uint32_t link_res;
    int rc;

    rc = spice_channel_recv(channel, &link_res, sizeof(link_res));
    if (rc != sizeof(link_res))
        PANIC("incomplete auth reply (%d/%zd)", rc, sizeof(link_res));
    if (link_res != SPICE_LINK_ERR_OK) {
        spice_channel_disconnect(channel, SPICE_CHANNEL_ERROR_AUTH);
        return;
    }

    fprintf(stderr, "channel up: %s\n", c->name);
    c->state = SPICE_CHANNEL_STATE_READY;
    spice_channel_emit_event(channel, SPICE_CHANNEL_OPENED);

    if (SPICE_CHANNEL_GET_CLASS(channel)->channel_up)
        SPICE_CHANNEL_GET_CLASS(channel)->channel_up(channel);
}

static void spice_channel_send_link(SpiceChannel *channel)
{
    spice_channel *c = SPICE_CHANNEL_GET_PRIVATE(channel);
    uint8_t *buffer, *p;

    c->link_hdr.magic = SPICE_MAGIC;
    c->link_hdr.size = sizeof(c->link_msg);

    switch (c->protocol) {
    case 1: /* protocol 1 == major 1, old 0.4 protocol, last active minor */
        c->link_hdr.major_version = 1;
        c->link_hdr.minor_version = 3;
        c->parser = spice_get_server_channel_parser1(c->channel_type, NULL);
        c->marshallers = spice_message_marshallers_get1();
        break;
    case SPICE_VERSION_MAJOR: /* protocol 2 == current */
        c->link_hdr.major_version = SPICE_VERSION_MAJOR;
        c->link_hdr.minor_version = SPICE_VERSION_MINOR;
        c->parser = spice_get_server_channel_parser(c->channel_type, NULL);
        c->marshallers = spice_message_marshallers_get();
        break;
    default:
        PANIC("unknown major %d", c->protocol);
    }

    c->link_msg.connection_id = c->connection_id;
    c->link_msg.channel_type  = c->channel_type;
    c->link_msg.channel_id    = c->channel_id;
    c->link_msg.caps_offset   = sizeof(c->link_msg);

#if 0 /* TODO */
    c->link_msg.num_common_caps = get_common_caps().size();
    c->link_msg.num_channel_caps = get_caps().size();
    c->link_hdr.size += (c->link_msg.num_common_caps + c->link_msg.num_channel_caps) * sizeof(uint32_t);
#endif

    buffer = spice_malloc(sizeof(c->link_hdr) + c->link_hdr.size);
    p = buffer;

    memcpy(p, &c->link_hdr, sizeof(c->link_hdr)); p += sizeof(c->link_hdr);
    memcpy(p, &c->link_msg, sizeof(c->link_msg)); p += sizeof(c->link_msg);

#if 0
    for (i = 0; i < _common_caps.size(); i++) {
        *(uint32_t *)p = _common_caps[i];
        p += sizeof(uint32_t);
    }
    for (i = 0; i < _caps.size(); i++) {
        *(uint32_t *)p = _caps[i];
        p += sizeof(uint32_t);
    }
#endif

    spice_channel_send(channel, buffer, p - buffer);
}

static void spice_channel_recv_link_hdr(SpiceChannel *channel)
{
    spice_channel *c = SPICE_CHANNEL_GET_PRIVATE(channel);
    int rc;

    rc = spice_channel_recv(channel, &c->peer_hdr, sizeof(c->peer_hdr));
    if (rc != sizeof(c->peer_hdr))
        PANIC("incomplete link header (%d/%zd)", rc, sizeof(c->peer_hdr));
    if (c->peer_hdr.magic != SPICE_MAGIC)
        PANIC("bad magic");
    if (c->peer_hdr.major_version != c->link_hdr.major_version) {
        if (c->peer_hdr.major_version == 1) {
            /* enter spice 0.4 mode */
            c->protocol = 1;
            fprintf(stderr, "switching to protocol 1 (spice 0.4)\n");
            spice_channel_disconnect(channel, SPICE_CHANNEL_NONE);
            spice_channel_connect(channel);
            return;
        }
        PANIC("major mismatch (got %d, expected %d)",
              c->peer_hdr.major_version, c->link_hdr.major_version);
    }

    c->peer_msg = spice_malloc(c->peer_hdr.size);
    c->state = SPICE_CHANNEL_STATE_LINK_MSG;
}

static void spice_channel_recv_link_msg(SpiceChannel *channel)
{
    spice_channel *c = SPICE_CHANNEL_GET_PRIVATE(channel);
    int rc, num_caps;

    rc = spice_channel_recv(channel, c->peer_msg, c->peer_hdr.size);
    if (rc != c->peer_hdr.size)
        PANIC("incomplete link reply (%d/%d)", rc, c->peer_hdr.size);
    switch (c->peer_msg->error) {
    case SPICE_LINK_ERR_OK:
        /* nothing */
        break;
    case SPICE_LINK_ERR_NEED_SECURED:
        c->tls = true;
        fprintf(stderr, "switching to tls\n");
        spice_channel_disconnect(channel, SPICE_CHANNEL_NONE);
        spice_channel_connect(channel);
        return;
    default:
        fprintf(stderr, "%s: unhandled error %d\n", __FUNCTION__,
                c->peer_msg->error);
        spice_channel_disconnect(channel, SPICE_CHANNEL_ERROR_LINK);
        return;
    }

    num_caps = c->peer_msg->num_channel_caps + c->peer_msg->num_common_caps;
    if (num_caps) {
        fprintf(stderr, "%s: %d caps\n", __FUNCTION__, num_caps);
    }

#if 0
    if ((uint8_t *)(reply + 1) > reply_buf.get() + header.size ||
        (uint8_t *)reply + reply->caps_offset + num_caps * sizeof(uint32_t) >
                                                                    reply_buf.get() + header.size) {
        THROW_ERR(SPICEC_ERROR_CODE_CONNECT_FAILED, "access violation");
    }

    uint32_t *caps = (uint32_t *)((uint8_t *)reply + reply->caps_offset);

    _remote_common_caps.clear();
    for (i = 0; i < reply->num_common_caps; i++, caps++) {
        _remote_common_caps.resize(i + 1);
        _remote_common_caps[i] = *caps;
    }

    _remote_caps.clear();
    for (i = 0; i < reply->num_channel_caps; i++, caps++) {
        _remote_caps.resize(i + 1);
        _remote_caps[i] = *caps;
    }
#endif

    c->state = SPICE_CHANNEL_STATE_AUTH;
    spice_channel_send_auth(channel);
}

void spice_channel_send_msg(SpiceChannel *channel, spice_msg_out *out)
{
    uint8_t *data;
    int free_data;
    size_t len;
    uint32_t res;

    data = spice_marshaller_linearize(out->marshaller, 0,
                                      &len, &free_data);
#if 0
    spice_msg_out_hexdump(out, data, len);
#endif
    res = spice_channel_send(channel, data, len);
    if (free_data) {
        free(data);
    }
    if (res != len) {
        /* TODO: queue up */
        PANIC("sending message data failed");
    }
}

static void spice_channel_recv_msg(SpiceChannel *channel)
{
    spice_channel *c = SPICE_CHANNEL_GET_PRIVATE(channel);
    spice_msg_in *in;
    int rc;

    if (!c->msg_in) {
        c->msg_in = spice_msg_in_new(channel);
    }
    in = c->msg_in;

    /* receive message */
    if (in->hpos < sizeof(in->header)) {
        rc = spice_channel_recv(channel, (uint8_t*)&in->header + in->hpos,
                                sizeof(in->header) - in->hpos);
        if (rc < 0)
            PANIC("recv hdr: %s", strerror(errno));
        in->hpos += rc;
        if (in->hpos < sizeof(in->header))
            return;
        in->data = spice_malloc(in->header.size);
    }
    if (in->dpos < in->header.size) {
        rc = spice_channel_recv(channel, in->data + in->dpos,
                                in->header.size - in->dpos);
        if (rc < 0)
            PANIC("recv msg: %s", strerror(errno));
        in->dpos += rc;
        if (in->dpos < in->header.size)
            return;
    }

    if (in->header.sub_list) {
        SpiceSubMessageList *sub_list;
        SpiceSubMessage *sub;
        spice_msg_in *sub_in;
        int i;

        sub_list = (SpiceSubMessageList *)(in->data + in->header.sub_list);
        for (i = 0; i < sub_list->size; i++) {
            sub = (SpiceSubMessage *)(in->data + sub_list->sub_messages[i]);
            sub_in = spice_msg_in_sub_new(channel, in, sub);
            sub_in->parsed = c->parser(sub_in->data, sub_in->data + sub_in->dpos,
                                       sub_in->header.type, c->peer_hdr.minor_version,
                                       &sub_in->psize, &sub_in->pfree);
            if (sub_in->parsed == NULL)
                PANIC("failed to parse sub-message: %s type %d",
                      c->name, sub_in->header.type);
            SPICE_CHANNEL_GET_CLASS(channel)->handle_msg(channel, sub_in);
            spice_msg_in_unref(sub_in);
        }
    }

    /* ack message */
    if (c->message_ack_count) {
        c->message_ack_count--;
        if (!c->message_ack_count) {
            spice_msg_out *out = spice_msg_out_new(channel, SPICE_MSGC_ACK);
            spice_msg_out_send(out);
            spice_msg_out_unref(out);
            c->message_ack_count = c->message_ack_window;
        }
    }

    /* parse message */
    in->parsed = c->parser(in->data, in->data + in->dpos, in->header.type,
                           c->peer_hdr.minor_version, &in->psize, &in->pfree);
    if (in->parsed == NULL)
        PANIC("failed to parse message: %s type %d",
              c->name, in->header.type);

    /* process message */
    SPICE_CHANNEL_GET_CLASS(channel)->handle_msg(channel, in);

    /* release message */
    spice_msg_in_unref(c->msg_in);
    c->msg_in = NULL;
}

static void spice_channel_data(int event, void *opaque)
{
    SpiceChannel *channel = opaque;
    spice_channel *c = SPICE_CHANNEL_GET_PRIVATE(channel);

    switch (c->state) {
    case SPICE_CHANNEL_STATE_TLS:
        spice_channel_tls_connect(channel);
        break;
    case SPICE_CHANNEL_STATE_LINK_HDR:
        spice_channel_recv_link_hdr(channel);
        break;
    case SPICE_CHANNEL_STATE_LINK_MSG:
        spice_channel_recv_link_msg(channel);
        break;
    case SPICE_CHANNEL_STATE_AUTH:
        spice_channel_recv_auth(channel);
        break;
    case SPICE_CHANNEL_STATE_READY:
        spice_channel_recv_msg(channel);
        break;
    default:
        PANIC("unknown state %d", c->state);
    }
}

SpiceChannel *spice_channel_new(SpiceSession *s, int type, int id)
{
    SpiceChannel *channel;
    GType gtype = 0;

    fprintf(stderr, "%s: %d:%d\n", __FUNCTION__, type, id);

    switch (type) {
    case SPICE_CHANNEL_MAIN:
        gtype = SPICE_TYPE_MAIN_CHANNEL;
        break;
    case SPICE_CHANNEL_DISPLAY:
        gtype = SPICE_TYPE_DISPLAY_CHANNEL;
        break;
    case SPICE_CHANNEL_CURSOR:
        gtype = SPICE_TYPE_CURSOR_CHANNEL;
        break;
    case SPICE_CHANNEL_INPUTS:
        gtype = SPICE_TYPE_INPUTS_CHANNEL;
        break;
    case SPICE_CHANNEL_PLAYBACK:
        gtype = SPICE_TYPE_PLAYBACK_CHANNEL;
        break;
    default:
        return NULL;
    }
    channel = SPICE_CHANNEL(g_object_new(gtype,
                                         "spice-session", s,
                                         "channel-type", type,
                                         "channel-id", id,
                                         NULL));
    return channel;
}

void spice_channel_destroy(SpiceChannel *channel)
{
    spice_channel *c = SPICE_CHANNEL_GET_PRIVATE(channel);

    fprintf(stderr, "%s %s\n", __FUNCTION__, c->name);
    g_object_unref(channel);
}

int spice_channel_id(SpiceChannel *channel)
{
    gint id;

    g_object_get(G_OBJECT(channel), "channel-id", &id, NULL);
    return id;
}

static int tls_verify(int preverify_ok, X509_STORE_CTX *ctx)
{
    spice_channel *c;
    char *hostname;
    SSL *ssl;

    ssl = X509_STORE_CTX_get_ex_data(ctx, SSL_get_ex_data_X509_STORE_CTX_idx());
    c = SSL_get_app_data(ssl);

    g_object_get(c->session, "host", &hostname, NULL);
    /* TODO: check hostname */

    return preverify_ok;
}

gboolean spice_channel_connect(SpiceChannel *channel)
{
    spice_channel *c = SPICE_CHANNEL_GET_PRIVATE(channel);
    int rc, err;

    if (c->session == NULL || c->channel_type == -1 || c->channel_id == -1) {
        /* unset properties or unknown channel type */
        fprintf(stderr, "%s: channel setup incomplete\n", __FUNCTION__);
        return false;
    }
    if (c->state != SPICE_CHANNEL_STATE_UNCONNECTED) {
        return true;
    }

reconnect:
    c->socket = spice_session_channel_connect(c->session, c->tls);
    if (c->socket == -1) {
        if (!c->tls) {
            c->tls = true;
            goto reconnect;
        }
        spice_channel_emit_event(channel, SPICE_CHANNEL_ERROR_CONNECT);
        return false;
    }
    c->watch = spice_watch_new(c->socket, SPICE_WATCH_EVENT_READ,
                               spice_channel_data, channel);

    if (c->tls) {
        char *ca_file;

        c->ctx = SSL_CTX_new(TLSv1_method());
        if (c->ctx == NULL) {
            PANIC("SSL_CTX_new failed");
        }

        g_object_get(c->session, "ca-file", &ca_file, NULL);
        if (ca_file) {
            rc = SSL_CTX_load_verify_locations(c->ctx, ca_file, NULL);
            if (rc <= 0) {
                fprintf(stderr, "loading ca certs from %s failed\n", ca_file);
            }
        }
        SSL_CTX_set_verify(c->ctx, SSL_VERIFY_PEER, tls_verify);

        c->ssl = SSL_new(c->ctx);
        if (c->ssl == NULL) {
            PANIC("SSL_new failed");
        }
        rc = SSL_set_fd(c->ssl, c->socket);
        if (rc <= 0) {
            PANIC("SSL_set_fd failed");
        }
        SSL_set_app_data(c->ssl, c);
        rc = SSL_connect(c->ssl);
        if (rc <= 0) {
            err = SSL_get_error(c->ssl, rc);
            if (err == SSL_ERROR_WANT_READ) {
                c->state = SPICE_CHANNEL_STATE_TLS;
                return 0;
            }
            fprintf(stderr, "SSL_connect: %s", ERR_error_string(err, NULL));
            spice_channel_emit_event(channel, SPICE_CHANNEL_ERROR_TLS);
        }
    }

    c->state = SPICE_CHANNEL_STATE_LINK_HDR;
    spice_channel_send_link(channel);
    return true;
}

void spice_channel_disconnect(SpiceChannel *channel, enum SpiceChannelEvent reason)
{
    spice_channel *c = SPICE_CHANNEL_GET_PRIVATE(channel);

    if (c->state == SPICE_CHANNEL_STATE_UNCONNECTED) {
        return;
    }

    if (c->tls) {
        if (c->ssl) {
            SSL_free(c->ssl);
            c->ssl = NULL;
        }
        if (c->ctx) {
            SSL_CTX_free(c->ctx);
            c->ctx = NULL;
        }
    }
    if (c->watch) {
        spice_watch_put(c->watch);
        c->watch = NULL;
    }
    if (c->socket != -1) {
        close(c->socket);
        c->socket = -1;
    }
    c->state = SPICE_CHANNEL_STATE_UNCONNECTED;
    if (reason != SPICE_CHANNEL_NONE) {
        spice_channel_emit_event(channel, reason);
    }
}