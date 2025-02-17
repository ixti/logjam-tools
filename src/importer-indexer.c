#include "importer-indexer.h"
#include "logjam-streaminfo.h"
#include "importer-mongoutils.h"


/*
 * connections: n_w = num_writers, n_p = num_parsers, "o" = bind, "[<>v^]" = connect
 *
 *                            controller
 *                                |
 *                               PIPE
 *               PUSH    PULL     |
 *  parser(n_p)  >----------o  indexer
 *
 */

// The parsers send index creation requests as soon as they see a new date in their input stream.
// The indexer keeps track of indexes it has already created to avoid repeated mongodb calls.
// Creating an index on a collection while it is being written to, slows down the writers considerably.
// The indexer therefore creates databases along with all their indexes one day in advance.
// On startup, databases and indexes for the current day are created synchronously. The completion
// of this is signalled to the controller by sending a started message to the controller.
// Databases and indexes for dates in the future are always created via a fresh background
// thread, spawned from the indexer.

typedef struct {
    size_t id;
    mongoc_client_t *mongo_clients[MAX_DATABASES];
    mongoc_collection_t *global_collection;
    zsock_t *controller_socket;
    zsock_t *pull_socket;
    zhash_t *databases;
} indexer_state_t;

typedef struct {
    size_t id;
    char iso_date[ISO_DATE_STR_LEN];
} bg_indexer_args_t;

// sleep 5 seconds in between each step when iterating over all
// databases to create tomorrow's indexes
#define INDEXER_DELAY 5

static
zsock_t *indexer_pull_socket_new()
{
    zsock_t *socket = zsock_new(ZMQ_PULL);
    assert(socket);
    int rc = zsock_bind(socket, "inproc://indexer");
    assert(rc == 0);
    return socket;
}

static
void create_index(indexer_state_t *self, mongoc_database_t *db, const char* collection_name, bson_t *keys)
{
    size_t id = self->id;
    char *index_name = mongoc_collection_keys_to_index_string(keys);
    bson_t *create_index_doc = BCON_NEW ("createIndexes",
                                        BCON_UTF8(collection_name),
                                       "indexes",
                                       "[",
                                       "{",
                                       "key",
                                       BCON_DOCUMENT(keys),
                                       "name",
                                       BCON_UTF8(index_name),
                                       "background",
                                       BCON_BOOL(USE_BACKGROUND_INDEX_BUILDS),
                                       "}",
                                       "]");
    /* char *index_doc_str = bson_as_json(create_index_doc, NULL); */
    /* printf("[D] indexer[%zu]: creating index \"%s\" on \"%s\" (%s)\n", id, index_name, collection_name, index_doc_str); */
    /* bson_free(index_doc_str); */

    bson_t reply;
    bson_error_t error;
    bool ok = mongoc_database_write_command_with_opts(db, create_index_doc, NULL /* opts */, &reply, &error);

    /* char *reply_str = bson_as_json (&reply, NULL); */
    /* printf("[D] indexer[%zu]: create index returned: %s\n", id, reply_str); */
    /* bson_free(reply_str); */

    if (!ok) {
        fprintf(stderr, "[E] indexer[%zu]: index creation on %s failed: %s (%d) %s\n", id, collection_name, index_name, error.code, error.message);
    }

    bson_free(index_name);
    bson_destroy(&reply);
    bson_destroy(create_index_doc);
}

static
void add_request_field_index(indexer_state_t *state, mongoc_database_t *db, const char* field)
{
    bson_t *keys;

    keys = bson_new();
    bson_append_int32(keys, "minute", 6, -1);
    bson_append_int32(keys, field, strlen(field), 1);
    create_index(state, db, "requests", keys);
    bson_destroy(keys);

    keys = bson_new();
    bson_append_int32(keys, "page", 4, 1);
    bson_append_int32(keys, "minute", 6, -1);
    bson_append_int32(keys, field, strlen(field), 1);
    create_index(state, db, "requests", keys);
    bson_destroy(keys);
}

static
void add_request_collection_indexes(indexer_state_t *state, mongoc_database_t *db)
{
    add_request_field_index(state, db, "response_code");
    add_request_field_index(state, db, "severity");
    add_request_field_index(state, db, "exceptions");
    add_request_field_index(state, db, "soft_exceptions");
    // add_request_field_index(state, db, "started_ms");
}

static
void add_jse_collection_indexes(indexer_state_t *state, mongoc_database_t *db)
{
    bson_t *keys;

    keys = bson_new();
    bson_append_int32(keys, "logjam_request_id", 17, 1);
    create_index(state, db, "js_exceptions", keys);
    bson_destroy(keys);

    keys = bson_new();
    bson_append_int32(keys, "description", 11, 1);
    create_index(state, db, "js_exceptions", keys);
    bson_destroy(keys);
}

static
void add_metrics_collection_indexes(indexer_state_t *state, mongoc_database_t *db)
{
    bson_t *keys;

    keys = bson_new();
    bson_append_int32(keys, "metric", 6, 1);
    bson_append_int32(keys, "value", 5, -1);
    create_index(state, db, "metrics", keys);
    bson_destroy(keys);

    keys = bson_new();
    bson_append_int32(keys, "page", 4, 1);
    bson_append_int32(keys, "metric", 6, 1);
    bson_append_int32(keys, "value", 5, -1);
    create_index(state, db, "metrics", keys);
    bson_destroy(keys);

    keys = bson_new();
    bson_append_int32(keys, "module", 6, 1);
    bson_append_int32(keys, "metric", 6, 1);
    bson_append_int32(keys, "value", 5, -1);
    create_index(state, db, "metrics", keys);
    bson_destroy(keys);

    keys = bson_new();
    bson_append_int32(keys, "minute", 6, 1);
    bson_append_int32(keys, "metric", 6, 1);
    bson_append_int32(keys, "value", 5, -1);
    create_index(state, db, "metrics", keys);
    bson_destroy(keys);
}

static
int64_t extract_storage_size(bson_t *doc)
{
    bson_iter_t iter;
    if (bson_iter_init_find (&iter, doc, "storageSize")) {
        bson_type_t bit = bson_iter_type (&iter);
        switch (bit) {
        case BSON_TYPE_DOUBLE:
            return bson_iter_double(&iter);
        case BSON_TYPE_INT64:
            return bson_iter_int64(&iter);
        case BSON_TYPE_INT32:
            return bson_iter_int32(&iter);
        default:
            fprintf(stderr, "unexpected bson type when reading databse stats: %d\n", bit);
        }
    }
    return 0;
}

static
void indexer_check_disk_usage(indexer_state_t *state, const char *db_name, stream_info_t *stream_info)
{
    if (dryrun) return;

    bson_t *cmd = bson_new();
    bson_append_int32(cmd, "dbStats", 7, 1);
    bson_append_int32(cmd, "scale", 5, 1);

    mongoc_client_t *client = state->mongo_clients[stream_info->db];
    mongoc_database_t *database = mongoc_client_get_database(client, db_name);
    bson_t reply;
    bson_init(&reply);
    bson_error_t error;

    bool ok = mongoc_database_command_simple(database, cmd, NULL, &reply, &error);
    if (!ok) {
        fprintf(stderr, "[E] could not retrieve database statistics: (%d) %s\n", error.code, error.message);
    } else {
        // size_t n;
        // char* bjs = bson_as_json(reply, &n);
        // printf("[D] database stats for (%s): %s\n", db_name, bjs);
        // bson_free(bjs);
        stream_info->storage_size = extract_storage_size(&reply);
        if (stream_info->storage_size > HARD_LIMIT_STORAGE_SIZE)
            fprintf(stderr, "[E] indexer[%zu]: hard limiting %s at %"PRId64"\n", state->id, db_name, stream_info->storage_size);
        else if (stream_info->storage_size > SOFT_LIMIT_STORAGE_SIZE)
            fprintf(stderr, "[W] indexer[%zu]: soft limiting %s at %"PRId64"\n", state->id, db_name, stream_info->storage_size);
        else if (verbose)
            fprintf(stdout, "[I] indexer[%zu]: not limiting %s at %"PRId64"\n", state->id, db_name, stream_info->storage_size);
    }

    bson_destroy(cmd);
    bson_destroy(&reply);
    mongoc_database_destroy(database);
}

static
void indexer_refresh_storage_sizes(indexer_state_t *self)
{
    if (dryrun) return;

    zlist_t *streams = get_active_stream_names();
    char *stream = zlist_first(streams);
    while (stream && !zsys_interrupted) {
        stream_info_t *info = get_stream_info(stream, NULL);
        if (info) {
            char db_name[1000];
            sprintf(db_name, "logjam-%s-%s-%s", info->app, info->env, iso_date_today);
            indexer_check_disk_usage(self, db_name, info);
            release_stream_info(info);
        }
        stream = zlist_next(streams);
    }
    zlist_destroy(&streams);
}

static
void indexer_create_indexes(indexer_state_t *state, const char *db_name, stream_info_t *stream_info)
{
    if (dryrun) return;

    mongoc_client_t *client = state->mongo_clients[stream_info->db];
    mongoc_database_t *db = mongoc_client_get_database(client, db_name);
    bson_t *keys;
    size_t id = state->id;

    // if it is a db of today, then make it known
    if (strstr(db_name, iso_date_today)) {
        printf("[I] indexer[%zu]: ensuring known database: %s\n", id, db_name);
        ensure_known_database(client, db_name);
    }
    printf("[I] indexer[%zu]: creating indexes for %s\n", id, db_name);

    keys = bson_new();
    assert(bson_append_int32(keys, "page", 4, 1));
    create_index(state, db, "totals", keys);
    bson_destroy(keys);

    keys = bson_new();
    assert(bson_append_int32(keys, "page", 4, 1));
    assert(bson_append_int32(keys, "minute", 6, 1));
    create_index(state, db, "minutes", keys);
    bson_destroy(keys);

    keys = bson_new();
    assert(bson_append_int32(keys, "page", 4, 1));
    assert(bson_append_int32(keys, "kind", 4, 1));
    assert(bson_append_int32(keys, "quant", 5, 1));
    create_index(state, db, "quants", keys);
    bson_destroy(keys);

    keys = bson_new();
    assert(bson_append_int32(keys, "page", 4, 1));
    assert(bson_append_int32(keys, "minute", 6, 1));
    create_index(state, db, "heatmaps", keys);
    bson_destroy(keys);

    keys = bson_new();
    assert(bson_append_int32(keys, "agent", 5, 1));
    create_index(state, db, "agents", keys);
    bson_destroy(keys);

    add_metrics_collection_indexes(state, db);
    add_request_collection_indexes(state, db);
    add_jse_collection_indexes(state, db);

    mongoc_database_destroy(db);
}

static
void indexer_create_all_indexes(indexer_state_t *self, const char *iso_date, int delay)
{
    if (dryrun) return;

    zlist_t *streams = get_active_stream_names();
    char *stream = zlist_first(streams);
    while (stream && !zsys_interrupted) {
        stream_info_t *info = get_stream_info(stream, NULL);
        if (info) {
            char db_name[1000];
            sprintf(db_name, "logjam-%s-%s-%s", info->app, info->env, iso_date);
            indexer_create_indexes(self, db_name, info);
            indexer_check_disk_usage(self, db_name, info);
            release_stream_info(info);
            if (delay) {
                zclock_sleep(1000 * delay);
            }
        }
        stream = zlist_next(streams);
    }
    zlist_destroy(&streams);
}

static
void* create_indexes_for_date(void* args)
{
    bg_indexer_args_t *indexer_args = args;
    if (dryrun) goto exit;

    indexer_state_t state;
    memset(&state, 0, sizeof(state));
    state.id = indexer_args->id;;

    char thread_name[16];
    memset(thread_name, 0, 16);
    snprintf(thread_name, 16, "indexer[%zu]", state.id);
    set_thread_name(thread_name);

    for (int i=0; i<num_databases; i++) {
        state.mongo_clients[i] = mongoc_client_new(databases[i]);
        assert(state.mongo_clients[i]);
    }
    state.databases = zhash_new();

    indexer_create_all_indexes(&state, indexer_args->iso_date, INDEXER_DELAY);

    zhash_destroy(&state.databases);
    for (int i=0; i<num_databases; i++) {
        mongoc_client_destroy(state.mongo_clients[i]);
    }

 exit:
    free(indexer_args);
    return NULL;
}

static
void spawn_bg_indexer_for_date(size_t id, const char* iso_date)
{
    bg_indexer_args_t *indexer_args = zmalloc(sizeof(bg_indexer_args_t));
    assert(indexer_args != NULL);
    indexer_args->id = id;
    strcpy(indexer_args->iso_date, iso_date);
    pthread_t thread;
    int rc = pthread_create (&thread, NULL, create_indexes_for_date, indexer_args);
    assert(rc == 0);
    rc = pthread_detach (thread);
    assert(rc == 0);
}

static
void ensure_databases_are_known(indexer_state_t *state, const char* iso_date)
{
    if (dryrun) return;

    zlist_t *streams = get_active_stream_names();
    char *stream = zlist_first(streams);
    while (stream && !zsys_interrupted) {
        stream_info_t *info = get_stream_info(stream, NULL);
        if (info) {
            mongoc_client_t *client = state->mongo_clients[info->db];
            char db_name[1000];
            sprintf(db_name, "logjam-%s-%s-%s", info->app, info->env, iso_date);
            ensure_known_database(client, db_name);
            release_stream_info(info);
        }
        stream = zlist_next(streams);
    }
    zlist_destroy(&streams);
}

static
void handle_indexer_request(zmsg_t *msg, indexer_state_t *state)
{
    zframe_t *db_frame = zmsg_first(msg);
    zframe_t *stream_frame = zmsg_next(msg);

    size_t n = zframe_size(db_frame);
    char db_name[n+1];
    memcpy(db_name, zframe_data(db_frame), n);
    db_name[n] = '\0';

    size_t m = zframe_size(stream_frame);
    char stream_name[m+1];
    memcpy(stream_name, zframe_data(stream_frame), m);
    stream_name[m] = '\0';

    stream_info_t *stream_info = zframe_getptr(stream_frame);

    const char *known_db = zhash_lookup(state->databases, db_name);
    if (known_db == NULL) {
        zhash_insert(state->databases, db_name, strdup(db_name));
        zhash_freefn(state->databases, db_name, free);
        indexer_create_indexes(state, db_name, stream_info);
    } else {
        // printf("[D] indexer[%zu]: indexes already created: %s\n", state->id, db_name);
    }
    release_stream_info(stream_info);
}

static
indexer_state_t* indexer_state_new(zsock_t *pipe, size_t id)
{
    indexer_state_t *state = zmalloc(sizeof(*state));
    state->id = id;
    state->controller_socket = pipe;
    state->pull_socket = indexer_pull_socket_new();
    if (!dryrun) {
        for (int i=0; i<num_databases; i++) {
            state->mongo_clients[i] = mongoc_client_new(databases[i]);
            assert(state->mongo_clients[i]);
        }
    }
    state->databases = zhash_new();
    return state;
}

static
void indexer_state_destroy(indexer_state_t **state_p)
{
    indexer_state_t *state = *state_p;
    zsock_destroy(&state->pull_socket);
    zhash_destroy(&state->databases);
    if (!dryrun) {
        for (int i=0; i<num_databases; i++) {
            mongoc_client_destroy(state->mongo_clients[i]);
        }
    }
    free(state);
    *state_p = NULL;
}

void indexer(zsock_t *pipe, void *args)
{
    size_t id = 0;
    char thread_name[16];
    memset(thread_name, 0, 16);
    snprintf(thread_name, 16, "indexer[%zu]", id);
    set_thread_name(thread_name);

    if (!quiet)
        printf("[I] indexer[%zu]: starting\n", id);

    size_t ticks = 0;
    size_t bg_indexer_runs = 0;
    indexer_state_t *state = indexer_state_new(pipe, id);

    // setup indexes for today (synchronously)
    config_update_date_info();
    indexer_create_all_indexes(state, iso_date_today, 0);

    // signal readyiness after index creation
    zsock_signal(pipe, 0);

    // setup indexes for tomorrow (asynchronously)
    spawn_bg_indexer_for_date(++bg_indexer_runs, iso_date_tomorrow);

    zpoller_t *poller = zpoller_new(state->controller_socket, state->pull_socket, NULL);
    assert(poller);

    while (!zsys_interrupted) {
        // printf("indexer[%zu]: polling\n", id);
        // wait at most one second
        void *socket = zpoller_wait(poller, 1000);
        zmsg_t *msg = NULL;
        if (socket == state->controller_socket) {
            msg = zmsg_recv(state->controller_socket);
            char *cmd = zmsg_popstr(msg);
            zmsg_destroy(&msg);
            if (streq(cmd, "tick")) {
                if (verbose)
                    printf("[D] indexer[%zu]: tick\n", id);

                // if date has changed, make sure databases of today are added to the known datbases
                // table and spawn a background thread to create databases for the next day
                if (config_update_date_info()) {
                    printf("[I] indexer[%zu]: date change detected\n", id);
                    printf("[I] indexer[%zu]: making sure today's databases are known\n", id);
                    ensure_databases_are_known(state, iso_date_today);
                    printf("[I] indexer[%zu]: creating indexes for tomorrow\n", id);
                    spawn_bg_indexer_for_date(++bg_indexer_runs, iso_date_tomorrow);
                }
                if (ticks++ % PING_INTERVAL == 0) {
                    // ping mongodb to reestablish connection if it got lost
                    for (int i=0; i<num_databases; i++) {
                        mongo_client_ping(state->mongo_clients[i]);
                    }
                }
                if (ticks % DATABASE_INFO_REFRESH_INTERVAL == 0) {
                    // retrieve current database storage sizew
                    indexer_refresh_storage_sizes(state);
                }
                // free collection pointers every hour
                if (ticks % COLLECTION_REFRESH_INTERVAL == COLLECTION_REFRESH_INTERVAL - id - 1) {
                    printf("[I] indexer[%zu]: freeing database info\n", id);
                    zhash_destroy(&state->databases);
                    state->databases = zhash_new();
                }
                free(cmd);
            } else if (streq(cmd, "$TERM")) {
                // printf("[D] indexer[%zu]: received $TERM command\n", id);
                free(cmd);
                break;
            } else {
                printf("[E] indexer[%zu]: received unknown command: %s\n", id, cmd);
                assert(false);
            }
        } else if (socket == state->pull_socket) {
            msg = zmsg_recv(state->pull_socket);
            if (msg != NULL) {
                handle_indexer_request(msg, state);
                zmsg_destroy(&msg);
            }
        } else if (socket) {
            // if socket is not null, something is horribly broken
            printf("[E] indexer[%zu]: broken poller. committing suicide.\n", id);
            assert(false);
        }
        else {
            // probably interrupted by signal handler
            // if so, loop will terminate on condition !zsys_interrupted
        }
    }

    if (!quiet)
        printf("[I] indexer[%zu]: shutting down\n", id);

    indexer_state_destroy(&state);

    if (!quiet)
        printf("[I] indexer[%zu]: terminated\n", id);
}
