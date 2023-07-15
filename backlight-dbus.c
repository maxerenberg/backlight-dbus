#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <systemd/sd-bus.h>
#include <systemd/sd-login.h>

#define LOG_INFO(args...) if (debug_on) fprintf(stderr, args)
#define LOG_ERROR(args...) fprintf(stderr, args)
#define NAME_MAX 255
#define PATH_MAX 4096
#define NANOSEC_PER_SEC 1000000000LL
#define NANOSEC_PER_MILLISEC 1000000LL
#define MILLISEC_PER_SEC 1000
#define SLEEP_MILLIS 100

static bool debug_on = false;
static volatile sig_atomic_t received_signal = false;
static int signals_to_catch[] = {SIGHUP, SIGINT, SIGTERM, 0};
static sigset_t signals_to_catch_set;

void log_method_call_failed(const sd_bus_error *error) {
    LOG_ERROR("Failed to issue method call: %s\n", error->message);
}

void log_parse_failed(int status) {
    LOG_ERROR("Failed to parse response message: %s\n", strerror(-status));
}

void signal_handler(int signum) {
    received_signal = true;
}

int get_xdg_session_id(sd_bus *bus, char **result, bool *should_free) {
    char *xdg_session_id = getenv("XDG_SESSION_ID");
    if (xdg_session_id) {
        *result = xdg_session_id;
        *should_free = false;
        return 0;
    }
    LOG_INFO("XDG_SESSION_ID not set, retrieving the primary session "
             "of the current user instead...\n");
    int sd_ret = sd_uid_get_display(getuid(), result);
    if (sd_ret < 0) {
        LOG_ERROR("Failed to retrieve primary session ID: %d\n", sd_ret);
        return sd_ret;
    }
    *should_free = true;
    return 0;
}

int read_value_from_file(char *dir, size_t dir_len, size_t dir_cap,
                         const char *filename, int *res)
{
    // Make sure we have enough space in our buffer to copy
    if (strlen(filename) > dir_cap-dir_len-1) {
        LOG_ERROR("File path is too long\n");
        return -1;
    }
    strcpy(dir+dir_len, filename);
    FILE *fi = fopen(dir, "r");
    if (fi == NULL) {
        LOG_ERROR("Could not open file %s\n", dir);
        return -1;
    }
    int num_read = fscanf(fi, "%d", res);
    fclose(fi);
    if (num_read != 1) {
        LOG_ERROR("Error reading value from file %s\n", dir);
        return -1;
    }
    return 0;
}

int get_device(const char ** res) {
    static const char *dir = "/sys/class/backlight/";
    static char device_name_alt[NAME_MAX+1];
    // Search for a device name
    DIR *dp = opendir(dir);
    if (!dp) {
        LOG_ERROR("Error opening directory %s\n", dir);
        return -1;
    }
    struct dirent *ep;
    while ((ep = readdir(dp)) && ep->d_name[0] == '.') ;
    if (!ep) {
        LOG_ERROR("Found no device names in %s\n", dir);
        closedir(dp);
        return -1;
    }
    strcpy(device_name_alt, ep->d_name);
    closedir(dp);
    *res = device_name_alt;
    return 0;
}

int read_brightness(const char *device_name, int *cur_brightness,
                    int *max_brightness)
{
    char dir[PATH_MAX];
    int size = snprintf(dir, sizeof(dir), "/sys/class/backlight/%s/", device_name);
    if (size > sizeof(dir)-1) {
        LOG_ERROR("File path is too long\n");
        return -1;
    }
    if (read_value_from_file(dir, size, sizeof(dir), "brightness",
            cur_brightness) != 0)
    {
        return -1;
    }
    if (read_value_from_file(dir, size, sizeof(dir), "max_brightness",
            max_brightness) != 0)
    {
        return -1;
    }
    return 0;
}

int calculate_target_brightness(
    const char *brightness_str, int cur_brightness, int max_brightness, int *res
) {
    int len = strlen(brightness_str);
    char prefix = 0;
    bool has_percent = false;
    int brightness;
    char *endptr;
    if (brightness_str[0] == '-' || brightness_str[0] == '+') {
        prefix = brightness_str[0];
        brightness_str++;
        len--;
    }
    if (brightness_str[len-1] == '%') {
        has_percent = true;
        len--;
    }
    brightness = strtol(brightness_str, &endptr, 10);
    if (len == 0 || endptr - brightness_str != len
            || isspace(brightness_str[0]))
    {
        LOG_ERROR("Invalid format for brightness\n");
        return -1;
    }
    if (has_percent) {
        brightness = max_brightness * brightness / 100;
    }
    if (prefix == '-') {
        brightness = cur_brightness - brightness;
    } else if (prefix == '+') {
        brightness = cur_brightness + brightness;
    }
    if (brightness > max_brightness) {
        LOG_ERROR("Brightness is out of range\n");
        return -1;
    }
    *res = brightness;
    return 0;
}

int read_countdown(const char *s, float *res) {
    if (s == NULL) {
        *res = 0;
        return 0;
    }
    char *endptr;
    float f = strtof(s, &endptr);
    if (endptr == s || *endptr != '\0' || f < 0) {
        LOG_ERROR("Invalid format for countdown\n");
        return -1;
    }
    *res = f;
    return 0;
}

void add_nanoseconds_to_timespec(const struct timespec *in_ts, long nanosecs, struct timespec *out_ts) {
    out_ts->tv_sec = in_ts->tv_sec;
    out_ts->tv_nsec = in_ts->tv_nsec + nanosecs;
    out_ts->tv_sec += out_ts->tv_nsec / NANOSEC_PER_SEC;
    out_ts->tv_nsec %= NANOSEC_PER_SEC;
}

int timespec_cmp(const struct timespec *a, const struct timespec *b) {
    if (a->tv_sec < b->tv_sec) return -1;
    if (a->tv_sec > b->tv_sec) return 1;
    if (a->tv_nsec < b->tv_nsec) return -1;
    if (a->tv_nsec > b->tv_nsec) return 1;
    return 0;
}

int timespec_diff_in_millis(const struct timespec *a, const struct timespec *b) {
    int sec_diff = a->tv_sec - b->tv_sec;
    long nsec_diff = a->tv_nsec - b->tv_nsec;
    return sec_diff * MILLISEC_PER_SEC + nsec_diff / NANOSEC_PER_MILLISEC;
}

int setup_signal_handler(void) {
    struct sigaction act;
    act.sa_handler = signal_handler;
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    for (int i = 0; signals_to_catch[i] != 0; i++) {
        if (sigaction(signals_to_catch[i], &act, NULL) < 0) {
            perror("sigaction");
            return -1;
        }
    }
    return 0;
}

void initialize_signals_to_catch_set(void) {
    sigset_t *set = &signals_to_catch_set;
    sigemptyset(set);
    for (int i = 0; signals_to_catch[i] != 0; i++) {
        sigaddset(set, signals_to_catch[i]);
    }
}

int block_signals(void) {
    if (sigprocmask(SIG_BLOCK, &signals_to_catch_set, NULL) < 0) {
        perror("sigprocmask");
        return -1;
    }
    return 0;
}

int unblock_signals(void) {
    if (sigprocmask(SIG_UNBLOCK, &signals_to_catch_set, NULL) < 0) {
        perror("sigprocmask");
        return -1;
    }
    return 0;
}

int set_brightness(
        sd_bus *bus, const char *session_object_path, sd_bus_error *error,
        const char *device_name, int brightness)
{
    // For some reason the DBus library doesn't seem to be able to
    // send messages anymore once it gets interrupted by one of the
    // termination signals. This is a problem for us because we want to
    // restore the original brightness after such a signal is received.
    // So we will block those signals when sending a message.
    block_signals();
    int ret = sd_bus_call_method(bus,
                              "org.freedesktop.login1",
                              session_object_path,
                              "org.freedesktop.login1.Session",
                              "SetBrightness",
                              error,
                              NULL,
                              "ssu",
                              "backlight",
                              device_name,
                              (unsigned int)brightness);
    unblock_signals();
    return ret;
}

int main(int argc, char *argv[]) {
    static const char *usage_fmt_str
        = "Usage: %s [options] [brightness]\n\n"
          "  -d DEVICE_NAME     e.g. 'intel_backlight'\n"
          "  -x XDG_SESSION_ID  session ID for current user\n"
          "  -t COUNTDOWN       countdown in seconds \n"
          "  -v                 enable debug output\n"
          "  -h                 show help message and quit\n";
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *get_session_msg = NULL;
    sd_bus *bus = NULL;
    const char *device_name = NULL,
               *session_object_path = NULL,
               *brightness_str = NULL,
               *countdown_str = NULL;
    char *xdg_session_id = NULL;
    bool should_free_session_id = false;
    int orig_brightness,
        cur_brightness,
        max_brightness,
        target_brightness;
    // TODO: use dynamic delay to prevent clock drift
    struct timespec delay_ts = {
        .tv_sec = 0,
        .tv_nsec = SLEEP_MILLIS * 1000
    };
    int status = 0;
    float countdown_sec;
    int total_millis;
    struct timespec start_time,
                    current_time,
                    target_time;

    // Parse arguments
    for (int i = 1; i < argc;) {
        int opt_len = strlen(argv[i]);
        if (argv[i][0] != '-') {
            if (brightness_str) goto bad_args;
            brightness_str = argv[i++];
            continue;
        }
        if (opt_len < 2) goto bad_args;
        if (argv[i][1] == 'h') goto show_usage;
        if (argv[i][1] == 'v') {
            debug_on = true;
            i++;
            continue;
        }
        if (!brightness_str && argv[i][1] >= '0' && argv[i][1] <= '9') {
            brightness_str = argv[i++];
            continue;
        }
        if (opt_len != 2) goto bad_args;
        if (i == argc-1) goto bad_args;
        switch (argv[i][1]) {
            case 'd':
                device_name = argv[i+1];
                break;
            case 'x':
                xdg_session_id = argv[i+1];
                break;
            case 't':
                countdown_str = argv[i+1];
                break;
            default:
                goto bad_args;
        }
        i += 2;
    }

    // Find device name
    if (device_name == NULL) {
        status = get_device(&device_name);
        if (status < 0) {
            goto finish;
        }
    }
    LOG_INFO("Using device %s\n", device_name);

    // Get current brightness levels
    status = read_brightness(device_name, &cur_brightness, &max_brightness);
    if (status < 0) {
        goto finish;
    }
    orig_brightness = cur_brightness;

    if (brightness_str == NULL) {
        // Just print current values
        printf("%u %u\n", cur_brightness, max_brightness);
        goto finish;
    }

    // Calculate desired brightness
    status = calculate_target_brightness(
        brightness_str, cur_brightness, max_brightness, &target_brightness);
    if (status < 0) {
        goto finish;
    }
    LOG_INFO("New brightness will be %u\n", target_brightness);

    // Calculate countdown
    status = read_countdown(countdown_str, &countdown_sec);
    if (status < 0) {
        goto finish;
    }
    total_millis = countdown_sec * MILLISEC_PER_SEC;
    if (clock_gettime(CLOCK_BOOTTIME, &current_time) < 0) {
        perror("clock_gettime");
        goto finish;
    }
    add_nanoseconds_to_timespec(&current_time, (long)(countdown_sec * NANOSEC_PER_SEC), &target_time);

    // Connect to the system bus
    status = sd_bus_open_system(&bus);
    if (status < 0) {
        LOG_ERROR("Failed to connect to systemd bus: %s\n", strerror(-status));
        goto finish;
    }

    // Get session ID for user
    if (xdg_session_id == NULL) {
        status = get_xdg_session_id(bus, &xdg_session_id,
                                    &should_free_session_id);
        if (status < 0) {
            goto finish;
        }
    }
    LOG_INFO("Session ID: %s\n", xdg_session_id);

    // Get the session path
    status = sd_bus_call_method(bus,
                                "org.freedesktop.login1",
                                "/org/freedesktop/login1",
                                "org.freedesktop.login1.Manager",
                                "GetSession",
                                &error,
                                &get_session_msg,
                                "s",
                                xdg_session_id);
    if (should_free_session_id) {
        free(xdg_session_id);
    }
    if (status < 0) {
        goto method_failed;
    }

    // Parse the response message
    status = sd_bus_message_read(get_session_msg, "o", &session_object_path);
    if (status < 0) {
        goto parse_failed;
    }
    LOG_INFO("Session object path: %s\n", session_object_path);

    // Set up the signal handler
    status = setup_signal_handler();
    if (status < 0) {
        goto finish;
    }
    initialize_signals_to_catch_set();

    // Set the brightness
    memcpy(&start_time, &current_time, sizeof(start_time));
    while (!received_signal && timespec_cmp(&current_time, &target_time) < 0) {
        status = nanosleep(&delay_ts, NULL);
        clock_gettime(CLOCK_BOOTTIME, &current_time);
        if (status < 0) {
            if (!received_signal) {
                perror("nanosleep");
            }
            break;
        }
        int millis_elapsed = timespec_diff_in_millis(&current_time, &start_time);
        if (millis_elapsed >= total_millis) break;
        // next = (1 - (millis_elapsed / total_millis)) * orig_brightness
        //      = orig_brightness - (millis_elapsed * orig_brightness) / total_millis
        int next_brightness = (int)(orig_brightness - ((int64_t)millis_elapsed * orig_brightness) / total_millis);
        if (next_brightness != cur_brightness) {
            status = set_brightness(
                bus, session_object_path, &error, device_name, next_brightness);
            if (status < 0) {
                goto method_failed;
            }
            cur_brightness = next_brightness;
        }
    }

    if (received_signal) {
        LOG_INFO("Received signal, restoring original brightness\n");
        if (cur_brightness != orig_brightness) {
            status = set_brightness(
                bus, session_object_path, &error, device_name, orig_brightness);
            if (status < 0) {
                goto method_failed;
            }
        }
    } else if (cur_brightness != target_brightness) {
        // We might need one more step
        status = set_brightness(
            bus, session_object_path, &error, device_name, target_brightness);
        if (status < 0) {
            goto method_failed;
        }
    }

    if (0) {
bad_args:
        status = -1;
show_usage:
        LOG_ERROR(usage_fmt_str, argv[0]);
        goto finish;
method_failed:
        log_method_call_failed(&error);
        goto finish;
parse_failed:
        log_parse_failed(status);
        goto finish;
    }
finish:
    sd_bus_error_free(&error);
    sd_bus_message_unref(get_session_msg);
    sd_bus_close_unref(bus);

    return status < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
