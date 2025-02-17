#ifndef __LOGJAM_MESSAGE_H_INCLUDED__
#define __LOGJAM_MESSAGE_H_INCLUDED__

#include <czmq.h>
#include <json_tokener.h>
#include "gelf-message.h"

typedef struct _logjam_message logjam_message;

logjam_message* logjam_message_read(zsock_t *receiver);

gelf_message* logjam_message_to_gelf(logjam_message *logjam_msg, json_tokener *tokener, zhash_t* stream_info_cache, zchunk_t *decompression_buffer, zchunk_t *scratch_buffer);

size_t logjam_message_size(logjam_message *msg);

void logjam_message_destroy(logjam_message **msg);

#endif
