#include <zmq.h>
#include <czmq.h>
#include <limits.h>
#include <zlib.h>
#include <snappy-c.h>
#include "logjam-util.h"

zlist_t *split_delimited_string(const char* s)
{
    if (!s) return NULL;

    char delim[] = ", ";
    char* token;
    char* state = NULL;
    zlist_t *strings = zlist_new();

    char *buffer = strdup(s);
    token = strtok_r(buffer, delim, &state);
    while (token != NULL) {
        zlist_push(strings, strdup(token));
        token = strtok_r (NULL, delim, &state);
    }
    free(buffer);

    return strings;
}

bool output_socket_ready(zsock_t *socket, int msecs)
{
    zmq_pollitem_t items[] = { { zsock_resolve(socket), 0, ZMQ_POLLOUT, 0 } };
    int rc = zmq_poll(items, 1, msecs);
    return rc != -1 && (items[0].revents & ZMQ_POLLOUT) != 0;
}

#if !HAVE_DECL_HTONLL
uint64_t htonll(uint64_t net_number)
{
  uint64_t result = 0;
  for (int i = 0; i < (int)sizeof(result); i++) {
    result <<= CHAR_BIT;
    result += (((unsigned char *)&net_number)[i] & UCHAR_MAX);
  }
  return result;
}
#endif

#if !HAVE_DECL_NTOHLL
uint64_t ntohll(uint64_t native_number)
{
  uint64_t result = 0;
  for (int i = (int)sizeof(result) - 1; i >= 0; i--) {
    ((unsigned char *)&result)[i] = native_number & UCHAR_MAX;
    native_number >>= CHAR_BIT;
  }
  return result;
}
#endif

int set_thread_name(const char* name)
{
#if defined(HAVE_PTHREAD_SETNAME_NP) && defined(__linux__)
    pthread_t self = pthread_self();
    return pthread_setname_np(self, name);
#elif defined(HAVE_PTHREAD_SETNAME_NP) && defined(__APPLE__)
    return pthread_setname_np(name);
#else
    return 0;
#endif
}

void dump_meta_info(msg_meta_t *meta)
{
    printf("[D] meta(tag:%hx version:%u compression:%u device:%u sequence:%" PRIu64 " created:%" PRIu64 ")\n",
           meta->tag, meta->version, meta->compression_method, meta->device_number, meta->sequence_number, meta->created_ms);
}

void dump_meta_info_network_format(msg_meta_t *meta)
{
    // copy meta
    msg_meta_t m = *meta;
    meta_info_decode(&m);
    dump_meta_info(&m);
}

int zmq_msg_extract_meta_info(zmq_msg_t *meta_msg, msg_meta_t *meta)
{
    int rc = zmq_msg_size(meta_msg) == sizeof(msg_meta_t);
    if (rc) {
        memcpy(meta, zmq_msg_data(meta_msg), sizeof(msg_meta_t));
        meta_info_decode(meta);
        if (meta->tag != META_INFO_TAG || meta->version != META_INFO_VERSION)
            rc = 0;
    }
    return rc;
}

int frame_extract_meta_info(zframe_t *meta_frame, msg_meta_t *meta)
{
    int rc = zframe_size(meta_frame) == sizeof(msg_meta_t);
    if (rc) {
        memcpy(meta, zframe_data(meta_frame), sizeof(msg_meta_t));
        meta_info_decode(meta);
        if (meta->tag != META_INFO_TAG || meta->version != META_INFO_VERSION)
            rc = 0;
    }
    return rc;
}

int msg_extract_meta_info(zmsg_t *msg, msg_meta_t *meta)
{
    // make sure the caller is clear in his head
    assert(zmsg_size(msg) == 4);
    zframe_t *meta_frame = zmsg_last(msg);

    // check frame size, tag and protocol version
    int rc = frame_extract_meta_info(meta_frame, meta);
    return rc;
}

int string_to_compression_method(const char *s)
{
    if (!strcmp("gzip", s))
        return ZLIB_COMPRESSION;
    else if (!strcmp("snappy", s))
        return SNAPPY_COMPRESSION;
    else if (!strcmp("brotli", s))
        return BROTLI_COMPRESSION;
    else {
        fprintf(stderr, "unsupported compression method: '%s'\n", s);
        return NO_COMPRESSION;
    }
}

const char* compression_method_to_string(int compression_method)
{
    switch (compression_method) {
    case NO_COMPRESSION:     return "no compression";
    case ZLIB_COMPRESSION:   return "zlib";
    case SNAPPY_COMPRESSION: return "snappy";
    case BROTLI_COMPRESSION: return "brotli";
    default:                 return "unknown compression method";
    }
}

#define RESOURCE_TEMPORARILY_UNAVAILABLE 35

int publish_on_zmq_transport(zmq_msg_t *message_parts, void *publisher, msg_meta_t *msg_meta, int flags)
{
    int rc=0;
    zmq_msg_t *app_env = &message_parts[0];
    zmq_msg_t *key     = &message_parts[1];
    zmq_msg_t *body    = &message_parts[2];

    rc = zmq_msg_send(app_env, publisher, flags|ZMQ_SNDMORE);
    if (rc == -1) {
        if (errno != RESOURCE_TEMPORARILY_UNAVAILABLE)
            log_zmq_error(rc, __FILE__, __LINE__);
        return rc;
    }
    rc = zmq_msg_send(key, publisher, flags|ZMQ_SNDMORE);
    if (rc == -1) {
        if (errno != RESOURCE_TEMPORARILY_UNAVAILABLE)
            log_zmq_error(rc, __FILE__, __LINE__);
        return rc;
    }
    rc = zmq_msg_send(body, publisher, flags|ZMQ_SNDMORE);
    if (rc == -1) {
        if (errno != RESOURCE_TEMPORARILY_UNAVAILABLE)
            log_zmq_error(rc, __FILE__, __LINE__);
        return rc;
    }

    zmq_msg_t meta;
    // dump_meta_info(msg_meta);
    msg_add_meta_info(&meta, msg_meta);
    // dump_meta_info_network_format(zmq_msg_data(&meta));

    rc = zmq_msg_send(&meta, publisher, flags);
    if (rc == -1) {
        if (errno != RESOURCE_TEMPORARILY_UNAVAILABLE)
            log_zmq_error(rc, __FILE__, __LINE__);
    }
    zmq_msg_close(&meta);
    return rc;
}

void compress_message_data_gzip(zchunk_t* buffer, zmq_msg_t *body, char *data, size_t data_len)
{
    const Bytef *raw_data = (Bytef *)data;
    uLong raw_len = data_len;
    uLongf compressed_len = compressBound(raw_len);
    // printf("[D] util: compression bound %zu\n", compressed_len);
    size_t buffer_size = zchunk_max_size(buffer);
    if (buffer_size < compressed_len) {
        size_t next_size = 2 * buffer_size;
        while (next_size < compressed_len)
            next_size *= 2;
        // printf("[D] util: resizing compression buffer to %zu\n", next_size);
        zchunk_resize(buffer, next_size);
    }
    Bytef *compressed_data = zchunk_data(buffer);

    // compress will update compressed_len to the actual size of the compressed data
    int rc = compress(compressed_data, &compressed_len, raw_data, raw_len);
    assert(rc == Z_OK);
    assert(compressed_len <= zchunk_max_size(buffer));

    zmq_msg_t compressed_msg;
    zmq_msg_init_size(&compressed_msg, compressed_len);
    memcpy(zmq_msg_data(&compressed_msg), compressed_data, compressed_len);
    zmq_msg_move(body, &compressed_msg);

    // printf("[D] uncompressed/compressed: %ld/%ld\n", raw_len, compressed_len);
}

void compress_message_data_snappy(zchunk_t* buffer, zmq_msg_t *body, char *data, size_t data_len)
{
    size_t max_compressed_len = snappy_max_compressed_length(data_len);
    // printf("[D] util: compression bound %zu\n", max_compressed_len);
    size_t buffer_size = zchunk_max_size(buffer);
    if (buffer_size < max_compressed_len) {
        size_t next_size = 2 * buffer_size;
        while (next_size < max_compressed_len)
            next_size *= 2;
        // printf("[D] util: resizing compression buffer to %zu\n", next_size);
        zchunk_resize(buffer, next_size);
    }
    char *compressed_data = (char*) zchunk_data(buffer);

    // compress will update compressed_len to the actual size of the compressed data
    size_t compressed_len = max_compressed_len;
    int rc = snappy_compress(data, data_len, compressed_data, &compressed_len);
    assert(rc == SNAPPY_OK);
    assert(compressed_len <= zchunk_max_size(buffer));

    zmq_msg_t compressed_msg;
    zmq_msg_init_size(&compressed_msg, compressed_len);
    memcpy(zmq_msg_data(&compressed_msg), compressed_data, compressed_len);
    zmq_msg_move(body, &compressed_msg);

    // printf("[D] uncompressed/compressed: %ld/%ld\n", raw_len, compressed_len);
}


void compress_message_data(int compression_method, zchunk_t* buffer, zmq_msg_t *body, char *data, size_t data_len)
{
    switch (compression_method) {
    case ZLIB_COMPRESSION:
        compress_message_data_gzip(buffer, body, data, data_len);
        break;
    case SNAPPY_COMPRESSION:
        compress_message_data_snappy(buffer, body, data, data_len);
        break;
    default:
        fprintf(stderr, "[D] unknown compression method\n");
    }
}


// we give up if the buffer needs to be larger than 10MB
const size_t max_buffer_size = 32 * 1024 * 1024;

int decompress_frame_gzip(zframe_t *body_frame, zchunk_t *buffer, char **body, size_t* body_len)
{
    uLongf dest_size = zchunk_max_size(buffer);
    Bytef *dest = zchunk_data(buffer);
    const Bytef *source = zframe_data(body_frame);
    uLong source_len = zframe_size(body_frame);

    while ( zchunk_max_size(buffer) <= max_buffer_size ) {
        if ( Z_OK == uncompress(dest, &dest_size, source, source_len) ) {
            *body = (char*) zchunk_data(buffer);
            *body_len = dest_size;
            return 1;
        } else {
            size_t next_size = 2 * zchunk_max_size(buffer);
            if (next_size > max_buffer_size)
                next_size = max_buffer_size;
            zchunk_resize(buffer, next_size);
            dest_size = next_size;
            dest = zchunk_data(buffer);
        }
    }
    return 0;
}

int decompress_frame_snappy(zframe_t *body_frame, zchunk_t *buffer, char **body, size_t* body_len)
{
    const char *source = (char*) zframe_data(body_frame);
    size_t source_len = zframe_size(body_frame);

    size_t dest_size = zchunk_max_size(buffer);
    char *dest = (char*) zchunk_data(buffer);

    *body = "";
    *body_len = 0;

    size_t uncompressed_length;
    if (SNAPPY_OK != snappy_uncompressed_length(source, source_len, &uncompressed_length))
        return 0;

    if (uncompressed_length > dest_size) {
        size_t next_size = 2 * dest_size;
        while (next_size < max_buffer_size)
            next_size *= 2;
        if (next_size > max_buffer_size)
            next_size = max_buffer_size;
        zchunk_resize(buffer, next_size);
        dest_size = next_size;
        dest = (char*) zchunk_data(buffer);
    }

    if (SNAPPY_OK != snappy_uncompress(source, source_len, dest, &dest_size))
        return 0;

    *body = dest;
    *body_len = dest_size;

    return 1;
}

int decompress_frame(zframe_t *body_frame, int compression_method, zchunk_t *buffer, char **body, size_t* body_len)
{
    switch (compression_method) {
    case ZLIB_COMPRESSION:
        return decompress_frame_gzip(body_frame, buffer, body, body_len);
    case SNAPPY_COMPRESSION:
        return decompress_frame_snappy(body_frame, buffer, body, body_len);
    default:
        fprintf(stderr, "[D] unknown compression method: %d\n", compression_method);
        return 0;
    }
}

json_object* parse_json_data(const char *json_data, size_t json_data_len, json_tokener* tokener)
{
    json_tokener_reset(tokener);
    json_object *jobj = json_tokener_parse_ex(tokener, json_data, json_data_len);
    enum json_tokener_error jerr = json_tokener_get_error(tokener);
    if (jerr != json_tokener_success) {
        fprintf(stderr, "[E] parse_json_body: %s\n", json_tokener_error_desc(jerr));
    } else {
        // const char *json_str_orig = zframe_strdup(body);
        // printf("[D] %s\n", json_str_orig);
        // free(json_str_orig);
        // dump_json_object(stdout, "[D]", jobj);
    }
    if (tokener->char_offset < json_data_len) // XXX shouldn't access internal fields
    {
        // Handle extra characters after parsed object as desired.
        fprintf(stderr, "[W] parse_json_body: %s\n", "extranoeus data in message payload");
        fprintf(stderr, "[W] MSGBODY=%.*s", (int)json_data_len, json_data);
    }
    // if (strnlen(json_data, json_data_len) < json_data_len) {
    //     fprintf(stderr, "[W] parse_json_body: json payload has null bytes\ndata: %*s\n", json_data_len, json_data);
    //     dump_json_object(stderr, "[W]", jobj);
    //     return NULL;
    // }
    return jobj;
}

void dump_json_object(FILE *f, const char* prefix, json_object *jobj)
{
    const char *json_str = json_object_to_json_string_ext(jobj, JSON_C_TO_STRING_PLAIN);
    fprintf(f, "%s %s\n", prefix, json_str);
    // don't try to free the json string. it will crash.
}

static void print_msg(byte* data, size_t size, const char *prefix, FILE *file)
{
    if (prefix)
        fprintf (file, "%s", prefix);

    int is_bin = 0;
    uint char_nbr;
    for (char_nbr = 0; char_nbr < size; char_nbr++)
        if (data [char_nbr] < 9 || data [char_nbr] > 127)
            is_bin = 1;

    fprintf (file, "[%03d] ", (int) size);
    size_t max_size = is_bin? 2048: 4096;
    const char *ellipsis = "";
    if (size > max_size) {
        size = max_size;
        ellipsis = "...";
    }
    for (char_nbr = 0; char_nbr < size; char_nbr++) {
        if (is_bin)
            fprintf (file, "%02X", (unsigned char) data [char_nbr]);
        else
            fprintf (file, "%c", data [char_nbr]);
    }
    fprintf (file, "%s\n", ellipsis);
}

void my_zframe_fprint(zframe_t *self, const char *prefix, FILE *file)
{
    assert (self);
    byte *data = zframe_data (self);
    size_t size = zframe_size (self);
    print_msg(data, size, prefix, file);
}

void my_zmsg_fprint(zmsg_t* self, const char* prefix, FILE* file)
{
    zframe_t *frame = zmsg_first(self);
    int frame_nbr = 0;
    while (frame && frame_nbr++ < 10) {
        my_zframe_fprint(frame, prefix, file);
        frame = zmsg_next(self);
    }
}

void my_zmq_msg_fprint(zmq_msg_t* msg, size_t n, const char* prefix, FILE* file )
{
    for (size_t i = 0; i < n; i++) {
        byte* data = zmq_msg_data(&msg[i]);
        size_t size = zmq_msg_size(&msg[i]);
        print_msg(data, size, prefix, file);
    }
}

//  --------------------------------------------------------------------------
//  Save message to an open file, return 0 if OK, else -1. The message is
//  saved as a series of frames, each with length and data. Note that the
//  file is NOT guaranteed to be portable between operating systems, not
//  versions of CZMQ. The file format is at present undocumented and liable
//  to arbitrary change.

int
zmsg_savex (zmsg_t *self, FILE *file)
{
    assert (self);
    assert (zmsg_is (self));
    assert (file);

    size_t frame_count = zmsg_size (self);
    if (fwrite (&frame_count, sizeof (frame_count), 1, file) != 1)
        return -1;

    zframe_t *frame = zmsg_first (self);
    while (frame) {
        size_t frame_size = zframe_size (frame);
        if (fwrite (&frame_size, sizeof (frame_size), 1, file) != 1)
            return -1;
        if (fwrite (zframe_data (frame), frame_size, 1, file) != 1)
            return -1;
        frame = zmsg_next (self);
    }
    return 0;
}

//  --------------------------------------------------------------------------
//  Load/append an open file into message, create new message if
//  null message provided. Returns NULL if the message could not be
//  loaded.

zmsg_t *
zmsg_loadx (zmsg_t *self, FILE *file)
{
    assert (file);
    if (!self)
        self = zmsg_new ();
    if (!self)
        return NULL;

    size_t frame_count;
    size_t rc = fread (&frame_count, sizeof (frame_count), 1, file);

    if (rc == 1) {
        for (size_t i = 0; i < frame_count; i++) {
            size_t frame_size;
            rc = fread (&frame_size, sizeof (frame_size), 1, file);
            if (rc == 1) {
                zframe_t *frame = zframe_new (NULL, frame_size);
                if (!frame) {
                    zmsg_destroy (&self);
                    return NULL;    //  Unable to allocate frame, fail
                }
                rc = fread (zframe_data (frame), frame_size, 1, file);
                if (frame_size > 0 && rc != 1) {
                    zframe_destroy (&frame);
                    zmsg_destroy (&self);
                    return NULL;    //  Corrupt file, fail
                }
                if (zmsg_append (self, &frame) == -1) {
                    zmsg_destroy (&self);
                    return NULL;    //  Unable to add frame, fail
                }
            }
            else
                break;              //  Unable to read properly, quit
        }
    }
    if (!zmsg_size (self)) {
        zmsg_destroy (&self);
        self = NULL;
    }
    return self;
}

void setup_subscriptions_for_sub_socket(zlist_t *subscriptions, zsock_t *socket)
{
    if (subscriptions==NULL || zlist_size(subscriptions)==0) {
        zsock_set_subscribe(socket, "");
        return;
    }
    // setup subscriptions for only a subset
    char *stream = zlist_first(subscriptions);
    while (stream != NULL)  {
        printf("[I] subscriber: subscribing to stream: %s\n", stream);
        zsock_set_subscribe(socket, stream);
        size_t n = strlen(stream);
        if (n > 15 && !strncmp(stream, "request-stream-", 15)) {
            zsock_set_subscribe(socket, stream+15);
        } else {
            char old_stream[n+15+1];
            sprintf(old_stream, "request-stream-%s", stream);
            zsock_set_subscribe(socket, old_stream);
        }
        stream = zlist_next(subscriptions);
    }
}
