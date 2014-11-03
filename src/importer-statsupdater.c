#include "importer-statsupdater.h"
#include "importer-increments.h"
#include "importer-streaminfo.h"
#include "importer-indexer.h"
#include "importer-resources.h"
#include "importer-mongoutils.h"


/*
 * connections: n_u = NUM_UPDATERS, "o" = bind, "[<>v^]" = connect
 *
 *                                controller
 *                                    |
 *                                   PIPE
 *                 PUSH    PULL       |
 *  [controller]   o----------<  updater(n_u)
 *
 */

// Receives controller commands via PIPE socket and database update tasks vie PULL socket.
// Currently both messages types are sent by the controller (but this might change).
// It might be better to insert a load balancer device between controller and updaters.

typedef struct {
    size_t id;
    mongoc_client_t *mongo_clients[MAX_DATABASES];
    mongoc_collection_t *global_collection;
    zhash_t *stats_collections;
    zsock_t *controller_socket;
    zsock_t *pull_socket;
    size_t updates_count;
} stats_updater_state_t;

typedef struct {
    mongoc_collection_t *totals;
    mongoc_collection_t *minutes;
    mongoc_collection_t *quants;
} stats_collections_t;

typedef struct {
    const char *db_name;
    mongoc_collection_t *collection;
} collection_update_callback_t;

static
bson_t* increments_to_bson(const char* namespace, increments_t* increments)
{
    // dump_increments(namespace, increments, NULL);

    bson_t *incs = bson_new();
    if (increments->backend_request_count)
        bson_append_int32(incs, "count", 5, increments->backend_request_count);
    size_t frontend_request_count = 0;
    if (increments->page_request_count) {
        frontend_request_count += increments->page_request_count;
        bson_append_int32(incs, "page_count", 10, increments->page_request_count);
    }
    if (increments->ajax_request_count) {
        frontend_request_count += increments->ajax_request_count;
        bson_append_int32(incs, "ajax_count", 10, increments->ajax_request_count);
    }
    // store frontend_count for easier retrieval of frontend totals/minutes
    if (frontend_request_count) {
        bson_append_int32(incs, "frontend_count", 14, frontend_request_count);
    }

    for (size_t i=0; i<=last_resource_offset; i++) {
        double val = increments->metrics[i].val;
        if (val > 0) {
            const char *name = int_to_resource[i];
            bson_append_double(incs, name, strlen(name), val);
            const char *name_sq = int_to_resource_sq[i];
            bson_append_double(incs, name_sq, strlen(name_sq), increments->metrics[i].val_squared);
        }
    }

    json_object_object_foreach(increments->others, key, value_obj) {
        size_t n = strlen(key);
        enum json_type type = json_object_get_type(value_obj);
        switch (type) {
        case json_type_int:
            bson_append_int32(incs, key, n, json_object_get_int(value_obj));
            break;
        case json_type_double:
            bson_append_double(incs, key, n, json_object_get_double(value_obj));
            break;
        default:
            fprintf(stderr, "[E] unsupported json type in json to bson conversion: %s, key: %s\n", json_type_to_name(type), key);
        }
    }

    bson_t *document = bson_new();
    bson_append_document(document, "$inc", 4, incs);

    // size_t n;
    // char* bs = bson_as_json(document, &n);
    // printf("[D] document. size: %zu; value:%s\n", n, bs);
    // bson_free(bs);

    bson_destroy(incs);

    return document;
}

static
int minutes_add_increments(const char* namespace, void* data, void* arg)
{
    collection_update_callback_t *cb = arg;
    mongoc_collection_t *collection = cb->collection;
    const char *db_name = cb->db_name;
    increments_t* increments = data;

    int minute = 0;
    char* p = (char*) namespace;
    while (isdigit(*p)) {
        minute *= 10;
        minute += *(p++) - '0';
    }
    p++;

    bson_t *selector = bson_new();
    assert( bson_append_utf8(selector, "page", 4, p, strlen(p)) );
    assert( bson_append_int32(selector, "minute", 6, minute ) );

    // size_t n;
    // char* bs = bson_as_json(selector, &n);
    // printf("[D] selector. size: %zu; value:%s\n", n, bs);
    // bson_free(bs);

    bson_t *document = increments_to_bson(namespace, increments);
    if (!dryrun) {
        bson_error_t error;
        int tries = TOKU_TX_RETRIES;
    retry:
        if (!mongoc_collection_update(collection, MONGOC_UPDATE_UPSERT, selector, document, wc_no_wait, &error)) {
            if ((error.code == TOKU_TX_LOCK_FAILED) && (--tries > 0)) {
                fprintf(stderr, "[W] retrying minutes update operation on %s\n", db_name);
                goto retry;
            } else {
                size_t n;
                char* bjs = bson_as_json(document, &n);
                fprintf(stderr,
                        "[E] update failed for %s on minutes: (%d) %s\n"
                        "[E] document size: %zu; value: %s\n",
                        db_name, error.code, error.message, n, bjs);
                bson_free(bjs);
            }
        }
    }
    bson_destroy(selector);
    bson_destroy(document);
    return 0;
}

static
int totals_add_increments(const char* namespace, void* data, void* arg)
{
    collection_update_callback_t *cb = arg;
    mongoc_collection_t *collection = cb->collection;
    const char *db_name = cb->db_name;
    increments_t* increments = data;
    assert(increments);

    bson_t *selector = bson_new();
    assert( bson_append_utf8(selector, "page", 4, namespace, strlen(namespace)) );

    // size_t n;
    // char* bs = bson_as_json(selector, &n);
    // printf("[D] selector. size: %zu; value:%s\n", n, bs);
    // bson_free(bs);

    bson_t *document = increments_to_bson(namespace, increments);
    if (!dryrun) {
        bson_error_t error;
        int tries = TOKU_TX_RETRIES;
    retry:
        if (!mongoc_collection_update(collection, MONGOC_UPDATE_UPSERT, selector, document, wc_no_wait, &error)) {
            if ((error.code == TOKU_TX_LOCK_FAILED) && (--tries > 0)) {
                fprintf(stderr, "[W] retrying totals update operation on %s\n", db_name);
                goto retry;
            } else {
                size_t n;
                char* bjs = bson_as_json(document, &n);
                fprintf(stderr,
                        "[E] update failed for %s on totals: (%d) %s\n"
                        "[E] document size: %zu; value: %s\n",
                        db_name, error.code, error.message, n, bjs);
                bson_free(bjs);
            }
        }
    }

    bson_destroy(selector);
    bson_destroy(document);
    return 0;
}

static
int quants_add_quants(const char* namespace, void* data, void* arg)
{
    collection_update_callback_t *cb = arg;
    mongoc_collection_t *collection = cb->collection;
    const char *db_name = cb->db_name;

    // extract keys from namespace: kind-quant-page
    char* p = (char*) namespace;
    char kind[2];
    kind[0] = *(p++);
    kind[1] = '\0';

    p++; // skip '-'
    size_t quant = 0;
    while (isdigit(*p)) {
        quant *= 10;
        quant += *(p++) - '0';
    }
    p++; // skip '-'

    bson_t *selector = bson_new();
    bson_append_utf8(selector, "page", 4, p, strlen(p));
    bson_append_utf8(selector, "kind", 4, kind, 1);
    bson_append_int32(selector, "quant", 5, quant);

    // size_t n;
    // char* bs = bson_as_json(selector, &n);
    // printf("[D] selector. size: %zu; value:%s\n", n, bs);
    // bson_free(bs);

    bson_t *incs = bson_new();
    size_t *quants = data;
    for (int i=0; i <= last_resource_offset; i++) {
        if (quants[i] > 0) {
            const char *resource = i2r(i);
            bson_append_int32(incs, resource, -1, quants[i]);
        }
    }
    bson_t *document = bson_new();
    bson_append_document(document, "$inc", 4, incs);

    // size_t n; char*
    // bs = bson_as_json(document, &n);
    // printf("[D] document. size: %zu; value:%s\n", n, bs);
    // bson_free(bs);

    if (!dryrun) {
        bson_error_t error;
        int tries = TOKU_TX_RETRIES;
    retry:
        if (!mongoc_collection_update(collection, MONGOC_UPDATE_UPSERT, selector, document, wc_no_wait, &error)) {
            if ((error.code == TOKU_TX_LOCK_FAILED) && (--tries > 0)) {
                fprintf(stderr, "[W] retrying quants update operation on %s\n", db_name);
                goto retry;
            } else {
                size_t n;
                char* bjs = bson_as_json(document, &n);
                fprintf(stderr,
                        "[E] update failed for %s on quants: (%d) %s\n"
                        "[E] document size: %zu; value: %s\n",
                        db_name, error.code, error.message, n, bjs);
                bson_free(bjs);
            }
        }
    }
    bson_destroy(selector);
    bson_destroy(incs);
    bson_destroy(document);
    return 0;
}

static
stats_collections_t *stats_collections_new(mongoc_client_t* client, const char* db_name)
{
    stats_collections_t *collections = zmalloc(sizeof(stats_collections_t));

    if (dryrun) return collections;

    collections->totals = mongoc_client_get_collection(client, db_name, "totals");
    collections->minutes = mongoc_client_get_collection(client, db_name, "minutes");
    collections->quants = mongoc_client_get_collection(client, db_name, "quants");

    return collections;
}

static
void destroy_stats_collections(stats_collections_t* collections)
{
    if (collections->totals != NULL)
        mongoc_collection_destroy(collections->totals);
    if (collections->minutes != NULL)
        mongoc_collection_destroy(collections->minutes);
    if (collections->quants != NULL)
        mongoc_collection_destroy(collections->quants);
    free(collections);
}

static
stats_collections_t *stats_updater_get_collections(stats_updater_state_t *self, const char* db_name, stream_info_t *stream_info)
{
    stats_collections_t *collections = zhash_lookup(self->stats_collections, db_name);
    if (collections == NULL) {
        mongoc_client_t *mongo_client = self->mongo_clients[stream_info->db];
        // ensure_known_database(mongo_client, db_name);
        collections = stats_collections_new(mongo_client, db_name);
        assert(collections);
        zhash_insert(self->stats_collections, db_name, collections);
        zhash_freefn(self->stats_collections, db_name, (zhash_free_fn*)destroy_stats_collections);
    }
    return collections;
}

static
stats_updater_state_t* stats_updater_state_new(zsock_t *pipe, size_t id)
{
    stats_updater_state_t *state = zmalloc(sizeof(*state));
    state->id = id;
    state->updates_count = 0;
    state->controller_socket = pipe;
    state->pull_socket = zsock_new(ZMQ_PULL);
    assert(state->pull_socket);

    int rc = zsock_connect(state->pull_socket, "inproc://stats-updates");
    assert(rc==0);

    for (int i = 0; i<num_databases; i++) {
        state->mongo_clients[i] = mongoc_client_new(databases[i]);
        assert(state->mongo_clients[i]);
    }
    state->stats_collections = zhash_new();
    return state;
}

static
void stats_updater_state_destroy(stats_updater_state_t **state_p)
{
    stats_updater_state_t *state = *state_p;
    zsock_destroy(&state->pull_socket);
    zhash_destroy(&state->stats_collections);
    for (int i = 0; i<num_databases; i++) {
        mongoc_client_destroy(state->mongo_clients[i]);
    }
    free(state);
    *state_p = NULL;
}

static
void update_collection(zhash_t *updates, zhash_foreach_fn *fn, collection_update_callback_t *cb)
{
    void *update = zhash_first(updates);
    while (update) {
        const char *key = zhash_cursor(updates);
        fn(key, update, cb);
        update = zhash_next(updates);
    }
}


void stats_updater(zsock_t *pipe, void *args)
{
    size_t id = (size_t)args;
    size_t ticks = 0;
    stats_updater_state_t *state = stats_updater_state_new(pipe, id);
    // signal readyiness after sockets have been created
    zsock_signal(pipe, 0);

    zpoller_t *poller = zpoller_new(state->controller_socket, state->pull_socket, NULL);
    assert(poller);

    while (!zctx_interrupted) {
        // printf("[D] updater[%zu]: polling\n", id);
        // -1 == block until something is readable
        void *socket = zpoller_wait(poller, -1);
        zmsg_t *msg = NULL;
        if (socket == state->controller_socket) {
            msg = zmsg_recv(state->controller_socket);
            char *cmd = zmsg_popstr(msg);
            zmsg_destroy(&msg);
            if (streq(cmd, "tick")) {
                if (state->updates_count)
                    printf("[I] updater[%zu]: tick (%zu updates)\n", id, state->updates_count);
                // ping the server
                if (ticks++ % PING_INTERVAL == 0) {
                    for (int i=0; i<num_databases; i++) {
                        mongo_client_ping(state->mongo_clients[i]);
                    }
                }
                // refresh database information
                if (ticks % COLLECTION_REFRESH_INTERVAL == COLLECTION_REFRESH_INTERVAL - id - 1) {
                    zhash_destroy(&state->stats_collections);
                    state->stats_collections = zhash_new();
                }
                state->updates_count = 0;
                free(cmd);
            } else if (streq(cmd, "$TERM")) {
                // printf("[D] updater[%zu]: received $TERM command\n", id);
                free(cmd);
                break;
            } else {
                printf("[E] updater[%zu]: received unknnown command: %s\n", id, cmd);
                assert(false);
            }
        } else if (socket == state->pull_socket) {
            msg = zmsg_recv(state->pull_socket);
            state->updates_count++;
            int64_t start_time_ms = zclock_time();

            zframe_t *task_frame = zmsg_first(msg);
            zframe_t *db_frame = zmsg_next(msg);
            zframe_t *stream_frame = zmsg_next(msg);
            zframe_t *hash_frame = zmsg_next(msg);

            assert(zframe_size(task_frame) == 1);
            char task_type = *(char*)zframe_data(task_frame);

            size_t n = zframe_size(db_frame);
            char db_name[n+1];
            memcpy(db_name, zframe_data(db_frame), n);
            db_name[n] = '\0';

            stream_info_t *stream_info;
            assert(zframe_size(stream_frame) == sizeof(stream_info));
            memcpy(&stream_info, zframe_data(stream_frame), sizeof(stream_info));

            zhash_t *updates;
            assert(zframe_size(hash_frame) == sizeof(updates));
            memcpy(&updates, zframe_data(hash_frame), sizeof(updates));

            stats_collections_t *collections = stats_updater_get_collections(state, db_name, stream_info);
            collection_update_callback_t cb;
            cb.db_name = db_name;

            switch (task_type) {
            case 't':
                cb.collection = collections->totals;
                update_collection(updates, totals_add_increments, &cb);
                break;
            case 'm':
                cb.collection = collections->minutes;
                update_collection(updates, minutes_add_increments, &cb);
                break;
            case 'q':
                cb.collection = collections->quants;
                update_collection(updates, quants_add_quants, &cb);
                break;
            default:
                fprintf(stderr, "[E] updater[%zu]: unknown task type: %c\n", id, task_type);
                assert(false);
            }
            zhash_destroy(&updates);

            int64_t end_time_ms = zclock_time();
            printf("[I] updater[%zu]: task[%c]: (%3d ms) %s\n", id, task_type, (int)(end_time_ms - start_time_ms), db_name);
            zmsg_destroy(&msg);
        } else {
            printf("[I] updater[%zu]: no socket input. interrupted = %d\n", id, zctx_interrupted);
            break;
        }
    }

    printf("[I] updater[%zu]: shutting down\n", id);
    stats_updater_state_destroy(&state);
    printf("[I] updater[%zu]: terminated\n", id);
}
