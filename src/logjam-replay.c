#include "logjam-util.h"
#include <getopt.h>

bool dryrun = false;
bool verbose = false;
bool debug = false;
bool quiet = false;

FILE* dump_file = NULL;
static char *dump_file_name = "logjam-stream.dump";
static size_t dump_file_size = 0;
static size_t bytes_read_from_file = 0;

static size_t io_threads = 1;
static char *connection_spec = NULL;
static int socket_type = ZMQ_PUB;

#define DEFAULT_CONNECTION_PORT_PUB 9606
#define DEFAULT_CONNECTION_SPEC_PUB "tcp://*:9606"
#define DEFAULT_CONNECTION_PORT_DEALER 9604
#define DEFAULT_CONNECTION_SPEC_DEALER "tcp://localhost:9604"

static bool endless_loop = false;
static int messages_per_second = 100000;
static int message_credit = 1000000;
static int device_number = 4711;

static size_t replayed_messages_count = 0;
static size_t replayed_messages_bytes = 0;
static size_t replayed_messages_max_bytes = 0;

static int timer_event( zloop_t *loop, int timer_id, void *arg)
{
    static size_t last_replayed_count = 0;
    static size_t last_replayed_bytes = 0;
    size_t message_count = replayed_messages_count - last_replayed_count;
    size_t message_bytes = replayed_messages_bytes - last_replayed_bytes;
    double avg_msg_size = message_count ? (message_bytes / 1024.0) / message_count : 0;
    double max_msg_size = replayed_messages_max_bytes / 1024.0;
    printf("[I] processed %zu messages (%.2f KB), avg: %.2f KB, max: %.2f KB\n",
           message_count, message_bytes/1024.0, avg_msg_size, max_msg_size);
    last_replayed_count = replayed_messages_count;
    last_replayed_bytes = replayed_messages_bytes;
    replayed_messages_max_bytes = 0;
    message_credit = messages_per_second;

    return 0;
}

static int file_consume_message_and_forward(zloop_t *loop, zmq_pollitem_t *item, void* arg)
{
    zsock_t *socket = arg;

    if (message_credit-- <= 0) {
        zclock_sleep(1);
        return 0;
    }

    zmsg_t *msg = zmsg_loadx(NULL, dump_file);
    if (!msg) return 1;

    // update device and seqeunce number
    static uint64_t sequence_number = 0;
    zmsg_set_device_and_sequence_number(msg, device_number, ++sequence_number);

    // calculate stats
    size_t msg_bytes = zmsg_content_size(msg);
    bytes_read_from_file  += sizeof(size_t) * 5 + msg_bytes;
    replayed_messages_count++;
    replayed_messages_bytes += msg_bytes;
    if (msg_bytes > replayed_messages_max_bytes)
        replayed_messages_max_bytes = msg_bytes;

    if (debug) {
        my_zmsg_fprint(msg, "[D]", stdout);
        msg_meta_t meta;
        msg_extract_meta_info(msg, &meta);
        dump_meta_info("[D]", &meta);
    }

    // send message and destroy it
    zmsg_send(&msg, socket);

    if (bytes_read_from_file == dump_file_size) {
        if (endless_loop) {
            if (verbose) printf("[I] end of dump file reached. rewinding.\n");
            bytes_read_from_file = 0;
            rewind(dump_file);
        } else
            zsys_interrupted = 1;
    }
    return 0;
}

void print_usage(char * const *argv)
{
    fprintf(stderr,
            "usage: %s [options] [dump-file-name]\n"
            "\nOptions:\n"
            "  -i, --io-threads N         zeromq io threads\n"
            "  -l, --loop                 loop the dump file\n"
            "  -r, --msg-rate N           output message rate (per second)\n"
            "  -v, --verbose              log more (use -vv for debug output)\n"
            "  -d, --dealer               use zqm DEALER socket for publishing\n"
            "  -p, --pub S                zmq specification for publishing socket\n"
            "  -s, --device N             simulate given device number\n"
            "      --help                 display this message\n"
            , argv[0]);
}

void process_arguments(int argc, char * const *argv)
{
    char c;
    int longindex = 0;
    opterr = 0;

    static struct option long_options[] = {
        { "help",          no_argument,       0,  0  },
        { "loop",          no_argument,       0, 'l' },
        { "msg-rate",      required_argument, 0, 'r' },
        { "io-threads",    required_argument, 0, 'i' },
        { "pub",           required_argument, 0, 'p' },
        { "device",        required_argument, 0, 's' },
        { "verbose",       no_argument,       0, 'v' },
        { "dealer",        no_argument,       0, 'd' },
        { 0,               0,                 0,  0  }
    };

    while ((c = getopt_long(argc, argv, "vdlr:i:p:s:", long_options, &longindex)) != -1) {
        switch (c) {
        case 'v':
            if (verbose)
                debug = true;
            else
                verbose = true;
            break;
        case 'l':
            endless_loop = true;
            break;
        case 'r':
            messages_per_second = atoi(optarg);
            break;
        case 'd':
            socket_type = ZMQ_DEALER;
            break;
        case 'i':
            io_threads = atoi(optarg);
            break;
        case 'p':
            connection_spec = optarg;
            break;
        case 's':
            device_number = atoi(optarg);
            break;
        case 0:
            print_usage(argv);
            exit(0);
            break;
        case '?':
            if (strchr("rips", optopt))
                fprintf(stderr, "[E] option -%c requires an argument.\n", optopt);
            else if (isprint (optopt))
                fprintf(stderr, "[E] unknown option `-%c'.\n", optopt);
            else
                fprintf(stderr, "[E] unknown option character `\\x%x'.\n", optopt);
            print_usage(argv);
            exit(1);
        default:
            fprintf(stderr, "BUG: can't process option -%c\n", optopt);
            exit(1);
        }
    }

    if (optind + 1 < argc) {
        fprintf(stderr, "[E] too many arguments\n");
        print_usage(argv);
        exit(1);
    } else if (optind +1 == argc) {
        dump_file_name = argv[argc-1];
    }

    if (socket_type == ZMQ_PUB) {
        if (connection_spec == NULL)
            connection_spec = DEFAULT_CONNECTION_SPEC_PUB;
        else
            connection_spec = augment_zmq_connection_spec(connection_spec, DEFAULT_CONNECTION_PORT_PUB);
    } else {
        if (connection_spec == NULL)
            connection_spec = DEFAULT_CONNECTION_SPEC_DEALER;
        else
            connection_spec = augment_zmq_connection_spec(connection_spec, DEFAULT_CONNECTION_PORT_DEALER);
    }
}

int main(int argc, char * const *argv)
{
    // don't buffer stdout and stderr
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);

    process_arguments(argc, argv);

    // open dump file
    dump_file = fopen(dump_file_name, "r");
    if (!dump_file) {
        fprintf(stderr, "[E] could not open dump file: %s\n", strerror(errno));
        exit(1);
    }
    if (verbose) printf("[I] replaying stream from %s\n", dump_file_name);
    dump_file_size = zsys_file_size (dump_file_name);

    // set global config
    zsys_init();
    zsys_set_rcvhwm(10000);
    zsys_set_sndhwm(10000);
    zsys_set_pipehwm(1000);
    zsys_set_linger(100);
    zsys_set_io_threads(io_threads);

    // create socket to push messages to
    zsock_t* publisher = zsock_new(socket_type);
    assert_x(publisher != NULL, "[E] zmq socket creation failed", __FILE__, __LINE__);

    // configure the push socket
    zsock_set_sndhwm(publisher, 1000000);

    if (socket_type == ZMQ_PUB) {
        // bind pub socket
        printf("[I] binding PUB socket to %s\n", connection_spec);
        int rc = zsock_bind(publisher, "%s", connection_spec);
        log_zmq_error(rc, __FILE__, __LINE__);
        assert(rc != -1);
    } else {
        // connect dealer socket
        printf("[I] connecting DEALER socket to %s\n", connection_spec);
        int rc = zsock_connect(publisher, "%s", connection_spec);
        log_zmq_error(rc, __FILE__, __LINE__);
        assert(rc == 0);
    }

    // set up event loop
    zloop_t *loop = zloop_new();
    assert(loop);
    zloop_set_verbose(loop, 0);

    // register FILE descriptor for pollin events
    zmq_pollitem_t dump_file_item = {
        .fd = fileno(dump_file),
        .events = ZMQ_POLLIN
    };
    int rc = zloop_poller(loop, &dump_file_item, file_consume_message_and_forward, publisher);
    assert(rc==0);

    // calculate statistics every 1000 ms
    int timer_id = 1;
    rc = zloop_timer(loop, 1000, 0, timer_event, &timer_id);
    assert(rc != -1);

    // set publishing rate
    message_credit = messages_per_second;

    if (!zsys_interrupted) {
        if (verbose) printf("[I] starting main event loop\n");
        bool should_continue_to_run = getenv("CPUPROFILE") != NULL;
        do {
            rc = zloop_start(loop);
            should_continue_to_run &= errno == EINTR && !zsys_interrupted;
            log_zmq_error(rc, __FILE__, __LINE__);
        } while (should_continue_to_run);
        if (verbose) printf("[I] main event loop terminated with return code %d\n", rc);
    }

    printf("[I] replayed %zu messages\n", replayed_messages_count);

    // clean up
    if (verbose) printf("[I] shutting down\n");

    fclose(dump_file);
    zloop_destroy(&loop);
    assert(loop == NULL);
    zsock_destroy(&publisher);
    zsys_shutdown();

    if (verbose) printf("[I] terminated\n");

    return 0;
}
