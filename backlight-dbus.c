#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <systemd/sd-bus.h>

static bool debug_on = false;
static bool received_signal = false;

#define LOG_INFO(args...) if (debug_on) fprintf(stderr, args)
#define LOG_ERROR(args...) fprintf(stderr, args)
#define NAME_MAX 255
#define PATH_MAX 4096

void log_method_call_failed(const sd_bus_error *error) {
    LOG_ERROR("Failed to issue method call: %s\n", error->message);
}

void log_parse_failed(int status) {
    LOG_ERROR("Failed to parse response message: %s\n", strerror(-status));
}

void signal_handler(int signum) {
    received_signal = true;
}

int get_xdg_session_id(sd_bus *bus, const char ** result) {
    char *xdg_session_id = getenv("XDG_SESSION_ID");
    if (xdg_session_id) {
        *result = xdg_session_id;
        return 0;
    }
    LOG_INFO("XDG_SESSION_ID not set, iterating "
             "over all sessions instead...\n");
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *msg = NULL;
    // Iterate over active sessions and try to find one for the current user
    int status = sd_bus_call_method(
        bus, "org.freedesktop.login1", "/org/freedesktop/login1",
        "org.freedesktop.login1.Manager", "ListSessions",
        &error, &msg, NULL);
    if (status < 0) {
        goto method_failed;
    }
    status = sd_bus_message_enter_container(msg, 'a', "(susso)");
    if (status < 0) {
        goto parse_failed;
    }
    unsigned int uid = getuid();
    for (;;) {
        const char *session_id, *seat_id;
        unsigned int user_id;
        status = sd_bus_message_read(
            msg, "(susso)", &session_id, &user_id, NULL, &seat_id, NULL);
        if (status < 0) {
            sd_bus_message_exit_container(msg);
            goto parse_failed;
        }
        if (status == 0) {
            LOG_ERROR("Could not find session with seat for user %u\n", uid);
            status = -1;
            break;
        }
        if (user_id == uid && seat_id[0] != '\0') {
            *result = session_id;
            break;
        }
    }
    sd_bus_message_exit_container(msg);
    if (0) {
method_failed:
        log_method_call_failed(&error);
        goto finish;
parse_failed:
        log_parse_failed(status);
        goto finish;
    }
finish:
    sd_bus_error_free(&error);
    sd_bus_message_unref(msg);
    return status;
}

int read_value_from_file(char *dir, size_t dir_len, size_t dir_cap,
                         const char *filename, unsigned int *res)
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
    int num_read = fscanf(fi, "%u", res);
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

int read_brightness(const char *device_name, unsigned int *cur_brightness,
                    unsigned int *max_brightness)
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

int calculate_brightness(const char *brightness_str, unsigned int cur_brightness,
                         unsigned int max_brightness, unsigned int *res)
{
    int len = strlen(brightness_str);
    char prefix = 0;
    bool has_percent = false;
    unsigned int brightness;
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

int read_countdown(const char *s, double *res)
{
    char *endptr;
    double f = 0;
    if (s != NULL) {
        f = strtod(s, &endptr);
        if (endptr == s || *endptr != '\0' || f < 0) {
            LOG_ERROR("Invalid format for countdown\n");
            return -1;
        }
    }
    *res = f;
    return 0;
}

int setup_signal_handler() {
    struct sigaction act;
    int signals_to_catch[] = {SIGHUP, SIGINT, SIGTERM};
    act.sa_handler = signal_handler;
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    for (int i = 0; i < 3; i++) {
        if (sigaction(signals_to_catch[i], &act, NULL) < 0) {
            perror("sigaction");
            return -1;
        }
    }
    return 0;
}

int set_brightness(
        sd_bus *bus, const char *session_object_path, sd_bus_error *error,
        const char *device_name, unsigned int brightness)
{
    return sd_bus_call_method(bus,
                              "org.freedesktop.login1",
                              session_object_path,
                              "org.freedesktop.login1.Session",
                              "SetBrightness",
                              error,
                              NULL,
                              "ssu",
                              "backlight",
                              device_name,
                              brightness);
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
               *xdg_session_id = NULL,
               *brightness_str = NULL,
               *countdown_str = NULL;
    unsigned int cur_brightness,
                 max_brightness,
                 brightness,
                 num_steps,
                 delay_ms = 200;
    int status = 0,
        brightness_change;
    double countdown_sec;
    struct timespec delay_ts = {
        .tv_sec = 0,
        .tv_nsec = delay_ms * 1e6
    };

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

    if (brightness_str == NULL) {
        // Just print current values
        printf("%u %u\n", cur_brightness, max_brightness);
        goto finish;
    }

    // Calculate desired brightness
    status = calculate_brightness(
        brightness_str, cur_brightness, max_brightness, &brightness);
    if (status < 0) {
        goto finish;
    }
    brightness_change = (int)brightness - (int)cur_brightness;
    LOG_INFO("New brightness will be %u\n", brightness);

    // Calculate countdown
    status = read_countdown(countdown_str, &countdown_sec);
    if (status < 0) {
        goto finish;
    }
    num_steps = ceil(countdown_sec * 1000 / delay_ms);
    if (num_steps == 0) {
        // No countdown is equivalent to one step with delay 0
        num_steps = 1;
        delay_ts.tv_nsec = 0;
    }
    LOG_INFO("Number of steps: %u\n", num_steps);

    // Connect to the system bus
    status = sd_bus_open_system(&bus);
    if (status < 0) {
        LOG_ERROR("Failed to connect to systemd bus: %s\n", strerror(-status));
        goto finish;
    }

    // Get session ID for user
    if (xdg_session_id == NULL) {
        status = get_xdg_session_id(bus, &xdg_session_id);
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

    // Set the brightness
    for (unsigned int i = 1; i <= num_steps; i++) {
        status = nanosleep(&delay_ts, NULL);
        if (status < 0) {
            if (received_signal) {
                LOG_INFO("Received signal, restoring original brightness\n");
                status = set_brightness(
                    bus, session_object_path, &error, device_name,
                    cur_brightness);
                if (status < 0) {
                    goto method_failed;
                }
            } else {
                perror("nanosleep");
            }
            break;
        }
        unsigned int intermediate_brightness
            = cur_brightness + brightness_change * ((double)i / num_steps);
        status = set_brightness(
            bus, session_object_path, &error, device_name,
            intermediate_brightness);
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
    sd_bus_unref(bus);

    return status < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
