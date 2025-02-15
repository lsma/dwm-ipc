#include <getopt.h>
#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <yajl/yajl_gen.h>

#define IPC_MAGIC "DWM-IPC"
// clang-format off
#define IPC_MAGIC_ARR { 'D', 'W', 'M', '-', 'I', 'P', 'C' }
// clang-format on
#define IPC_MAGIC_LEN 7  // Not including null char

#define IPC_EVENT_TAG_CHANGE "tag_change_event"
#define IPC_EVENT_CLIENT_FOCUS_CHANGE "client_focus_change_event"
#define IPC_EVENT_LAYOUT_CHANGE "layout_change_event"
#define IPC_EVENT_MONITOR_FOCUS_CHANGE "monitor_focus_change_event"
#define IPC_EVENT_FOCUSED_TITLE_CHANGE "focused_title_change_event"
#define IPC_EVENT_FOCUSED_STATE_CHANGE "focused_state_change_event"

#define YSTR(str) yajl_gen_string(gen, (unsigned char *)str, strlen(str))
#define YINT(num) yajl_gen_integer(gen, num)
#define YDOUBLE(num) yajl_gen_double(gen, num)
#define YBOOL(v) yajl_gen_bool(gen, v)
#define YNULL() yajl_gen_null(gen)
#define YARR(body)                                                             \
  {                                                                            \
    yajl_gen_array_open(gen);                                                  \
    body;                                                                      \
    yajl_gen_array_close(gen);                                                 \
  }
#define YMAP(body)                                                             \
  {                                                                            \
    yajl_gen_map_open(gen);                                                    \
    body;                                                                      \
    yajl_gen_map_close(gen);                                                   \
  }

const char *prog_name;

typedef unsigned long Window;

const char *DEFAULT_SOCKET_PATH = "/tmp/dwm.sock";
static int sock_fd = -1;
static unsigned int ignore_reply = 0;

typedef enum IPCMessageType {
  IPC_TYPE_RUN_COMMAND = 0,
  IPC_TYPE_GET_MONITORS = 1,
  IPC_TYPE_GET_TAGS = 2,
  IPC_TYPE_GET_LAYOUTS = 3,
  IPC_TYPE_GET_DWM_CLIENT = 4,
  IPC_TYPE_SUBSCRIBE = 5,
  IPC_TYPE_EVENT = 6
} IPCMessageType;

// Every IPC message must begin with this
typedef struct dwm_ipc_header {
  uint8_t magic[IPC_MAGIC_LEN];
  uint32_t size;
  uint8_t type;
} __attribute((packed)) dwm_ipc_header_t;


static int
recv_message(uint8_t *msg_type, uint32_t *reply_size, uint8_t **reply)
{
  uint32_t read_bytes = 0;
  const int32_t to_read = sizeof(dwm_ipc_header_t);
  char header[to_read];
  char *walk = header;

  // Try to read header
  while (read_bytes < to_read) {
    ssize_t n = read(sock_fd, header + read_bytes, to_read - read_bytes);

    if (n == 0) {
      if (read_bytes == 0) {
        warnx("Unexpectedly reached EOF while reading header.");
        warnx("Read %" PRIu32 " bytes, expected %" PRIu32 " total bytes.",
              read_bytes, to_read);
        return -2;
      } else {
        warnx("Unexpectedly reached EOF while reading header.");
        warnx("Read %" PRIu32 " bytes, expected %" PRIu32 " total bytes.",
              read_bytes, to_read);
        return -3;
      }
    } else if (n == -1) {
      return -1;
    }

    read_bytes += n;
  }

  // Check if magic string in header matches
  if (memcmp(walk, IPC_MAGIC, IPC_MAGIC_LEN) != 0) {
    warnx("Invalid magic string. Got '%.*s', expected '%s'",
         IPC_MAGIC_LEN, walk, IPC_MAGIC);
    return -3;
  }

  walk += IPC_MAGIC_LEN;

  // Extract reply size
  memcpy(reply_size, walk, sizeof(uint32_t));
  walk += sizeof(uint32_t);

  // Extract message type
  memcpy(msg_type, walk, sizeof(uint8_t));
  walk += sizeof(uint8_t);

  (*reply) = malloc(*reply_size);

  // Extract payload
  read_bytes = 0;
  while (read_bytes < *reply_size) {
    ssize_t n = read(sock_fd, *reply + read_bytes, *reply_size - read_bytes);

    if (n == 0) {
      warnx("Unexpectedly reached EOF while reading payload.");
      warnx("Read %" PRIu32 " bytes, expected %" PRIu32 " bytes.",
            read_bytes, *reply_size);
      free(*reply);
      return -2;
    } else if (n == -1) {
      if (errno == EINTR || errno == EAGAIN) continue;
      free(*reply);
      return -1;
    }

    read_bytes += n;
  }

  return 0;
}

static int
read_socket(IPCMessageType *msg_type, uint32_t *msg_size, char **msg)
{
  int ret = -1;

  while (ret != 0) {
    ret = recv_message((uint8_t *)msg_type, msg_size, (uint8_t **)msg);

    if (ret < 0) {
      // Try again (non-fatal error)
      if (ret == -1 && (errno == EINTR || errno == EAGAIN)) continue;

      err(EXIT_FAILURE, "Error receiving response from socket. The connection might have been lost.");
      exit(2);
    }
  }

  return 0;
}

static ssize_t
write_socket(const void *buf, size_t count)
{
  size_t written = 0;

  while (written < count) {
    const ssize_t n =
        write(sock_fd, ((uint8_t *)buf) + written, count - written);

    if (n == -1) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        continue;
      else
        return n;
    }
    written += n;
  }
  return written;
}

int
connect_to_socket(const char *socket_path)
{
  struct sockaddr_un addr;

  int sock = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sock == -1) {
    err(EXIT_FAILURE, "Could not create socket");
  }

  // Initialize struct to 0
  memset(&addr, 0, sizeof(struct sockaddr_un));

  addr.sun_family = AF_UNIX;
  if (socket_path != NULL) {
    strcpy(addr.sun_path, socket_path);
  } else {
    strcpy(addr.sun_path, DEFAULT_SOCKET_PATH);
  }

  if (connect(sock, (const struct sockaddr *)&addr, sizeof(struct sockaddr_un)) < 0) {
    close(sock);
    return -1;
  }

  return sock;
}

static int
send_message(IPCMessageType msg_type, uint32_t msg_size, uint8_t *msg)
{
  dwm_ipc_header_t header = {
      .magic = IPC_MAGIC_ARR, .size = msg_size, .type = msg_type};

  size_t header_size = sizeof(dwm_ipc_header_t);
  size_t total_size = header_size + msg_size;

  uint8_t buffer[total_size];

  // Copy header to buffer
  memcpy(buffer, &header, header_size);
  // Copy message to buffer
  memcpy(buffer + header_size, msg, header.size);

  write_socket(buffer, total_size);

  return 0;
}

static int
is_float(const char *s)
{
  size_t len = strlen(s);
  int is_dot_used = 0;
  int is_minus_used = 0;

  // Floats can only have one decimal point in between or digits
  // Optionally, floats can also be below zero (negative)
  for (int i = 0; i < len; i++) {
    if (isdigit(s[i]))
      continue;
    else if (!is_dot_used && s[i] == '.' && i != 0 && i != len - 1) {
      is_dot_used = 1;
      continue;
    } else if (!is_minus_used && s[i] == '-' && i == 0) {
      is_minus_used = 1;
      continue;
    } else
      return 0;
  }

  return 1;
}

static int
is_unsigned_int(const char *s)
{
  size_t len = strlen(s);

  // Unsigned int can only have digits
  for (int i = 0; i < len; i++) {
    if (isdigit(s[i]))
      continue;
    else
      return 0;
  }

  return 1;
}

static int
is_signed_int(const char *s)
{
  size_t len = strlen(s);

  // Signed int can only have digits and a negative sign at the start
  for (int i = 0; i < len; i++) {
    if (isdigit(s[i]))
      continue;
    else if (i == 0 && s[i] == '-') {
      continue;
    } else
      return 0;
  }

  return 1;
}

static void
flush_socket_reply()
{
  IPCMessageType reply_type;
  uint32_t reply_size;
  char *reply;

  read_socket(&reply_type, &reply_size, &reply);

  free(reply);
}

static void
print_socket_reply()
{
  IPCMessageType reply_type;
  uint32_t reply_size;
  char *reply;

  read_socket(&reply_type, &reply_size, &reply);

  printf("%.*s\n", reply_size, reply);
  fflush(stdout);
  free(reply);
}

static int
run_command(const char *name, char *args[], int argc)
{
  const unsigned char *msg;
  size_t msg_size;

  yajl_gen gen = yajl_gen_alloc(NULL);

  // Message format:
  // {
  //   "command": "<name>",
  //   "args": [ ... ]
  // }
  // clang-format off
  YMAP(
    YSTR("command"); YSTR(name);
    YSTR("args"); YARR(
      for (int i = 0; i < argc; i++) {
        if (is_signed_int(args[i])) {
          long long num = atoll(args[i]);
          YINT(num);
        } else if (is_float(args[i])) {
          float num = atof(args[i]);
          YDOUBLE(num);
        } else {
          YSTR(args[i]);
        }
      }
    )
  )
  // clang-format on

  yajl_gen_get_buf(gen, &msg, &msg_size);

  send_message(IPC_TYPE_RUN_COMMAND, msg_size, (uint8_t *)msg);

  if (!ignore_reply)
    print_socket_reply();
  else
    flush_socket_reply();

  yajl_gen_free(gen);

  return 0;
}

static int
get_monitors()
{
  send_message(IPC_TYPE_GET_MONITORS, 1, (uint8_t *)"");
  print_socket_reply();
  return 0;
}

static int
get_tags()
{
  send_message(IPC_TYPE_GET_TAGS, 1, (uint8_t *)"");
  print_socket_reply();

  return 0;
}

static int
get_layouts()
{
  send_message(IPC_TYPE_GET_LAYOUTS, 1, (uint8_t *)"");
  print_socket_reply();

  return 0;
}

static int
get_dwm_client(Window win)
{
  const unsigned char *msg;
  size_t msg_size;

  yajl_gen gen = yajl_gen_alloc(NULL);

  // Message format:
  // {
  //   "client_window_id": "<win>"
  // }
  // clang-format off
  YMAP(
    YSTR("client_window_id"); YINT(win);
  )
  // clang-format on

  yajl_gen_get_buf(gen, &msg, &msg_size);

  send_message(IPC_TYPE_GET_DWM_CLIENT, msg_size, (uint8_t *)msg);

  print_socket_reply();

  yajl_gen_free(gen);

  return 0;
}

static int
subscribe(const char *event)
{
  const unsigned char *msg;
  size_t msg_size;

  yajl_gen gen = yajl_gen_alloc(NULL);

  // Message format:
  // {
  //   "event": "<event>",
  //   "action": "subscribe"
  // }
  // clang-format off
  YMAP(
    YSTR("event"); YSTR(event);
    YSTR("action"); YSTR("subscribe");
  )
  // clang-format on

  yajl_gen_get_buf(gen, &msg, &msg_size);

  send_message(IPC_TYPE_SUBSCRIBE, msg_size, (uint8_t *)msg);

  if (!ignore_reply)
    print_socket_reply();
  else
    flush_socket_reply();

  yajl_gen_free(gen);

  return 0;
}

static void
usage_error(const char *format, ...)
{
  va_list args;
  va_start(args, format);

  if (format)
    vwarnx(format, args);
  fprintf(stderr, "usage: %s <command> [...]\n", prog_name);
  fprintf(stderr, "Try '%s help'\n", prog_name);

  va_end(args);
  exit(EXIT_FAILURE);
}

static void
print_usage()
{
  printf("usage: %s [-s <socket>] [-i] [-m] [-t <command>] <message>\n", prog_name);
  puts("Communicate with DWM, the suckless window manager.");
  puts("");
  puts("Commands:");
  puts("  -t run_command <name> [args...] Run an IPC command");
  puts("");
  puts("  -t get_monitors                 Get monitor properties");
  puts("");
  puts("  -t get_tags                     Get list of tags");
  puts("");
  puts("  -t get_layouts                  Get list of layouts");
  puts("");
  puts("  -t get_dwm_client <window_id>   Get dwm client proprties");
  puts("");
  puts("  -t subscribe [events...]        Subscribe to specified events");
  puts("                                  Options: " IPC_EVENT_TAG_CHANGE ",");
  puts("                                  " IPC_EVENT_LAYOUT_CHANGE ",");
  puts("                                  " IPC_EVENT_CLIENT_FOCUS_CHANGE ",");
  puts("                                  " IPC_EVENT_MONITOR_FOCUS_CHANGE ",");
  puts("                                  " IPC_EVENT_FOCUSED_TITLE_CHANGE ",");
  puts("                                  " IPC_EVENT_FOCUSED_STATE_CHANGE);
  puts("");
  puts("Other options:");
  puts("  -h, --help                      Display this message");
  puts("  -i, --ignore-reply              Don't print \"success\" reply messages from");
  puts("                                    run_command and subscribe.");
  puts("  -m, --monitor                   Use with the subscribe command to keep");
  puts("                                    listening for dwm events instead of exiting");
  puts("                                    immediately after recieving a reply (which");
  puts("                                    is thedefault behavior)."); 
  puts("");
}

int
main(int argc, char *argv[])
{
  prog_name = argv[0];
  int o, option_index = 0;
  char *socket_path = NULL;
  int monitor = 0;
  IPCMessageType message_type = IPC_TYPE_RUN_COMMAND;

  static struct option long_options[] = {
    {"socket", required_argument, 0, 's'},
    {"type", required_argument, 0, 't'},
    {"ignore-reply", no_argument, 0, 'i'},
    {"monitor", no_argument, 0, 'm'},
    {"help", no_argument, 0, 'h'},
    {0, 0, 0, 0}};

  char *options_string = "+s:t:him";

  while ((o = getopt_long(argc, argv, options_string, long_options, &option_index)) != -1) {
    if (o == 's') {
      free(socket_path);
      socket_path = strdup(optarg);
    } else if (o == 't') {
      if (strcasecmp(optarg, "command") == 0) {
        message_type = IPC_TYPE_RUN_COMMAND;
      } else if (strcasecmp(optarg, "run_command") == 0) {
        message_type = IPC_TYPE_RUN_COMMAND;
      } else if (strcasecmp(optarg, "get_monitors") == 0) {
        message_type = IPC_TYPE_GET_MONITORS;
      } else if (strcasecmp(optarg, "get_tags") == 0) {
        message_type = IPC_TYPE_GET_TAGS;
      } else if (strcasecmp(optarg, "get_layouts") == 0) {
        message_type = IPC_TYPE_GET_LAYOUTS;
      } else if (strcasecmp(optarg, "get_dwm_client") == 0) {
        message_type = IPC_TYPE_GET_DWM_CLIENT;
      } else if (strcasecmp(optarg, "subscribe") == 0) {
        message_type = IPC_TYPE_SUBSCRIBE;
      } else
        usage_error("Unknown message type (known types: command, get_monitors, get_tags, get_layouts, get_dwm_client, subscribe)");
    } else if (o == 'i') {
      ignore_reply = 1;
    } else if (o == 'm') {
      monitor = 1;
    } else if (o == 'h') {
      print_usage();
      return 0;
    } else if (o == '?') {
      exit(EXIT_FAILURE);
    }
  }

  if (monitor && message_type != IPC_TYPE_SUBSCRIBE)
    usage_error("The monitor option -m is used with \"-t subscribe\" exclusively.");

  sock_fd = connect_to_socket(socket_path);
  if (sock_fd == -1) {
    err(EXIT_FAILURE, "Failed to connect to socket");
    return 1;
  }

  if (message_type == IPC_TYPE_RUN_COMMAND) {
    if (optind >= argc) usage_error("No command specified");
    // Command name
    char *command = argv[optind];
    // Command arguments are everything after command name
    char **command_args = argv + ++optind;
    // Number of command arguments
    int command_argc = argc - optind;
    run_command(command, command_args, command_argc);
  } else if (message_type == IPC_TYPE_GET_MONITORS) {
    get_monitors();
  } else if (message_type == IPC_TYPE_GET_TAGS) {
    get_tags();
  } else if (message_type == IPC_TYPE_GET_LAYOUTS) {
    get_layouts();
  } else if (message_type == IPC_TYPE_GET_DWM_CLIENT) {
  if (optind < argc) {
      if (is_unsigned_int(argv[optind])) {
        Window win = atol(argv[optind]);
        get_dwm_client(win);
      } else
        usage_error("Expected unsigned integer argument");
    } else
      usage_error("Expected the window id");
  } else if (message_type == IPC_TYPE_SUBSCRIBE) {
    if (optind < argc) {
      for (int j = optind; j < argc; j++) subscribe(argv[j]);
    } else
      usage_error("Expected event name");
    // Keep listening for events forever
    do {
      print_socket_reply();
    } while (monitor);
  }

  return 0;
}
