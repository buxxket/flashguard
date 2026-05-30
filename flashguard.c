#define _POSIX_C_SOURCE 200809L

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrandr.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define APP_NAME "FlashGuard"
#define CONFIG_SUBPATH "flashguard/config"
#define MAX_OUTPUT_NAME 128

typedef struct {
    int fps;
    double min_brightness;
    double max_brightness;
    double curve_start_avg;
    double curve_end_avg;
    double curve_gamma;
    double curve_shoulder;
    double avg_input_gamma;
    double avg_bias;
    double brightness_smoothing;
    int grid_x;
    int grid_y;
} Config;

typedef struct {
    RROutput output;
    char name[MAX_OUTPUT_NAME];
    int x;
    int y;
    int width;
    int height;
    double last_brightness;
    double applied_brightness;
    char last_verbose_log[128];
} Monitor;

static volatile sig_atomic_t keep_running = 1;
static int verbose_logging = 0;
static Monitor *active_monitors = NULL;
static int active_monitor_count = 0;

static void log_message(FILE *stream, const char *level, const char *fmt, va_list args) {
    fprintf(stream, "%s: ", level);
    vfprintf(stream, fmt, args);
    fputc('\n', stream);
    fflush(stream);
}

static void log_info(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_message(stdout, "INFO", fmt, args);
    va_end(args);
}

static void log_warn(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_message(stderr, "WARN", fmt, args);
    va_end(args);
}

static void log_error(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_message(stderr, "ERROR", fmt, args);
    va_end(args);
}

static void log_verbose(const char *fmt, ...) {
    va_list args;

    if (!verbose_logging) {
        return;
    }

    va_start(args, fmt);
    log_message(stdout, "VERBOSE", fmt, args);
    va_end(args);
}

static void set_default_config(Config *cfg) {
    cfg->fps = 144;
    cfg->min_brightness = 0.6;
    cfg->max_brightness = 1.0;
    cfg->curve_start_avg = 0.10;
    cfg->curve_end_avg = 0.95;
    cfg->curve_gamma = 0.5;
    cfg->curve_shoulder = 0.35;
    cfg->avg_input_gamma = 1.0;
    cfg->avg_bias = 0.0;
    cfg->brightness_smoothing = 0.0;
    cfg->grid_x = 40;
    cfg->grid_y = 24;
}

static char *trim_whitespace(char *text) {
    char *end;

    while (*text != '\0' && isspace((unsigned char)*text)) {
        text++;
    }

    if (*text == '\0') {
        return text;
    }

    end = text + strlen(text) - 1;
    while (end > text && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }

    return text;
}

static int parse_int_value(const char *text, int *value) {
    char *end = NULL;
    long parsed;

    errno = 0;
    parsed = strtol(text, &end, 10);
    if (errno != 0 || end == text || *trim_whitespace(end) != '\0') {
        return 0;
    }
    if (parsed < INT_MIN || parsed > INT_MAX) {
        return 0;
    }

    *value = (int)parsed;
    return 1;
}

static int parse_double_value(const char *text, double *value) {
    char *end = NULL;
    double parsed;

    errno = 0;
    parsed = strtod(text, &end);
    if (errno != 0 || end == text || *trim_whitespace(end) != '\0') {
        return 0;
    }

    *value = parsed;
    return 1;
}

static int validate_config(const Config *cfg, char *error, size_t error_size) {
    if (cfg->fps <= 0) {
        snprintf(error, error_size, "fps must be greater than 0");
        return 0;
    }
    if (cfg->grid_x <= 0 || cfg->grid_y <= 0) {
        snprintf(error, error_size, "grid_x and grid_y must be greater than 0");
        return 0;
    }
    if (cfg->min_brightness <= 0.0 || cfg->max_brightness <= 0.0 ||
        cfg->min_brightness > cfg->max_brightness) {
        snprintf(error, error_size, "brightness range must be positive and ordered");
        return 0;
    }
    if (cfg->curve_end_avg <= cfg->curve_start_avg) {
        snprintf(error, error_size, "curve_end_avg must be greater than curve_start_avg");
        return 0;
    }
    if (cfg->curve_gamma <= 0.0 || cfg->avg_input_gamma <= 0.0) {
        snprintf(error, error_size, "gamma values must be greater than 0");
        return 0;
    }
    if (cfg->curve_shoulder < 0.0 || cfg->curve_shoulder > 1.0) {
        snprintf(error, error_size, "curve_shoulder must be between 0 and 1");
        return 0;
    }
    if (cfg->brightness_smoothing < 0.0 || cfg->brightness_smoothing > 1.0) {
        snprintf(error, error_size, "brightness_smoothing must be between 0 and 1");
        return 0;
    }

    return 1;
}

static int resolve_home_directory(char *buffer, size_t size) {
    const char *home = getenv("HOME");

    if (home != NULL && home[0] != '\0') {
        return snprintf(buffer, size, "%s", home) < (int)size;
    }

    struct passwd *pwd = getpwuid(getuid());
    if (pwd == NULL || pwd->pw_dir == NULL || pwd->pw_dir[0] == '\0') {
        return 0;
    }

    return snprintf(buffer, size, "%s", pwd->pw_dir) < (int)size;
}

static int resolve_config_path(char *buffer, size_t size) {
    const char *xdg_config_home = getenv("XDG_CONFIG_HOME");
    char home[PATH_MAX];

    if (xdg_config_home != NULL && xdg_config_home[0] != '\0') {
        return snprintf(buffer, size, "%s/%s", xdg_config_home, CONFIG_SUBPATH) < (int)size;
    }

    if (!resolve_home_directory(home, sizeof(home))) {
        return 0;
    }

    return snprintf(buffer, size, "%s/.config/%s", home, CONFIG_SUBPATH) < (int)size;
}

static int load_config_file(const char *path, Config *cfg_out, bool *file_found,
                            char *error, size_t error_size) {
    FILE *file;
    Config cfg;
    char line[512];
    int line_number = 0;

    set_default_config(&cfg);
    *file_found = false;

    file = fopen(path, "r");
    if (file == NULL) {
        if (errno == ENOENT) {
            *cfg_out = cfg;
            return 1;
        }

        snprintf(error, error_size, "unable to open config %s: %s", path, strerror(errno));
        return 0;
    }

    *file_found = true;

    while (fgets(line, sizeof(line), file) != NULL) {
        char *trimmed;
        char *equals;
        char *key;
        char *value;

        line_number++;
        trimmed = trim_whitespace(line);

        if (trimmed[0] == '\0' || trimmed[0] == '#') {
            continue;
        }

        equals = strchr(trimmed, '=');
        if (equals == NULL) {
            snprintf(error, error_size, "config line %d is missing '='", line_number);
            fclose(file);
            return 0;
        }

        *equals = '\0';
        key = trim_whitespace(trimmed);
        value = trim_whitespace(equals + 1);

        if (strcmp(key, "fps") == 0) {
            if (!parse_int_value(value, &cfg.fps)) {
                snprintf(error, error_size, "config line %d has invalid fps", line_number);
                fclose(file);
                return 0;
            }
        } else if (strcmp(key, "min_brightness") == 0) {
            if (!parse_double_value(value, &cfg.min_brightness)) {
                snprintf(error, error_size, "config line %d has invalid min_brightness", line_number);
                fclose(file);
                return 0;
            }
        } else if (strcmp(key, "max_brightness") == 0) {
            if (!parse_double_value(value, &cfg.max_brightness)) {
                snprintf(error, error_size, "config line %d has invalid max_brightness", line_number);
                fclose(file);
                return 0;
            }
        } else if (strcmp(key, "curve_start_avg") == 0) {
            if (!parse_double_value(value, &cfg.curve_start_avg)) {
                snprintf(error, error_size, "config line %d has invalid curve_start_avg", line_number);
                fclose(file);
                return 0;
            }
        } else if (strcmp(key, "curve_end_avg") == 0) {
            if (!parse_double_value(value, &cfg.curve_end_avg)) {
                snprintf(error, error_size, "config line %d has invalid curve_end_avg", line_number);
                fclose(file);
                return 0;
            }
        } else if (strcmp(key, "curve_gamma") == 0) {
            if (!parse_double_value(value, &cfg.curve_gamma)) {
                snprintf(error, error_size, "config line %d has invalid curve_gamma", line_number);
                fclose(file);
                return 0;
            }
        } else if (strcmp(key, "curve_shoulder") == 0) {
            if (!parse_double_value(value, &cfg.curve_shoulder)) {
                snprintf(error, error_size, "config line %d has invalid curve_shoulder", line_number);
                fclose(file);
                return 0;
            }
        } else if (strcmp(key, "avg_input_gamma") == 0) {
            if (!parse_double_value(value, &cfg.avg_input_gamma)) {
                snprintf(error, error_size, "config line %d has invalid avg_input_gamma", line_number);
                fclose(file);
                return 0;
            }
        } else if (strcmp(key, "avg_bias") == 0) {
            if (!parse_double_value(value, &cfg.avg_bias)) {
                snprintf(error, error_size, "config line %d has invalid avg_bias", line_number);
                fclose(file);
                return 0;
            }
        } else if (strcmp(key, "brightness_smoothing") == 0) {
            if (!parse_double_value(value, &cfg.brightness_smoothing)) {
                snprintf(error, error_size, "config line %d has invalid brightness_smoothing", line_number);
                fclose(file);
                return 0;
            }
        } else if (strcmp(key, "grid_x") == 0) {
            if (!parse_int_value(value, &cfg.grid_x)) {
                snprintf(error, error_size, "config line %d has invalid grid_x", line_number);
                fclose(file);
                return 0;
            }
        } else if (strcmp(key, "grid_y") == 0) {
            if (!parse_int_value(value, &cfg.grid_y)) {
                snprintf(error, error_size, "config line %d has invalid grid_y", line_number);
                fclose(file);
                return 0;
            }
        } else {
            log_warn("Ignoring unknown config key '%s' on line %d", key, line_number);
        }
    }

    if (ferror(file)) {
        snprintf(error, error_size, "error while reading config %s", path);
        fclose(file);
        return 0;
    }

    fclose(file);

    if (!validate_config(&cfg, error, error_size)) {
        return 0;
    }

    *cfg_out = cfg;
    return 1;
}

static int stat_config_file(const char *path, struct stat *stat_out) {
    if (stat(path, stat_out) == 0) {
        return 1;
    }

    return 0;
}

static int config_changed(const struct stat *current, const struct stat *previous, bool previous_valid) {
    if (!previous_valid) {
        return true;
    }

    return current->st_mtime != previous->st_mtime || current->st_size != previous->st_size;
}

static bool maybe_reload_config(const char *path, Config *active_config, struct stat *last_stat,
                                bool *last_stat_valid, bool *config_file_found) {
    struct stat current_stat;
    Config loaded_config;
    bool found = false;
    char error[256];

    if (!stat_config_file(path, &current_stat)) {
        if (*config_file_found) {
            log_warn("Config file %s is no longer available; keeping last known-good settings", path);
            *config_file_found = false;
            *last_stat_valid = false;
        }
        return false;
    }

    if (!config_changed(&current_stat, last_stat, *last_stat_valid)) {
        return false;
    }

    if (!load_config_file(path, &loaded_config, &found, error, sizeof(error))) {
        log_warn("Config reload skipped: %s", error);
        *last_stat = current_stat;
        *last_stat_valid = true;
        return false;
    }

    *active_config = loaded_config;
    *config_file_found = found;
    *last_stat = current_stat;
    *last_stat_valid = true;
    log_info("Reloaded config from %s (fps=%d)", path, active_config->fps);
    return true;
}

static int mask_shift(unsigned long mask) {
    int shift = 0;

    if (mask == 0) {
        return 0;
    }

    while ((mask & 1UL) == 0) {
        mask >>= 1;
        shift++;
    }

    return shift;
}

static double extract_channel_norm(unsigned long pixel, unsigned long mask) {
    int shift;
    unsigned long raw;
    unsigned long max_value;

    if (mask == 0) {
        return 0.0;
    }

    shift = mask_shift(mask);
    raw = (pixel & mask) >> shift;
    max_value = mask >> shift;
    if (max_value == 0) {
        return 0.0;
    }

    return (double)raw / (double)max_value;
}

static double clamp(double value, double minimum, double maximum) {
    if (value < minimum) {
        return minimum;
    }
    if (value > maximum) {
        return maximum;
    }
    return value;
}

static double quantize_brightness(double brightness) {
    return floor(brightness * 1000.0 + 0.5) / 1000.0;
}

static unsigned int frame_delay_us(const Config *cfg) {
    double delay = 1000000.0 / (double)cfg->fps;

    if (delay < 1.0) {
        delay = 1.0;
    }
    if (delay > 1000000.0) {
        delay = 1000000.0;
    }

    return (unsigned int)llround(delay);
}

static void sleep_for_frame(const Config *cfg) {
    struct timespec request;
    unsigned int delay_us = frame_delay_us(cfg);

    request.tv_sec = delay_us / 1000000U;
    request.tv_nsec = (long)(delay_us % 1000000U) * 1000L;

    while (nanosleep(&request, &request) != 0 && errno == EINTR) {
    }
}

static int set_brightness_for_output(const char *output_name, double brightness) {
    char command[256];
    int written;

    written = snprintf(command, sizeof(command),
                       "xrandr --output %s --brightness %.3f >/dev/null 2>&1",
                       output_name, brightness);
    if (written < 0 || written >= (int)sizeof(command)) {
        log_warn("Skipping brightness update for output %s because the command is too long", output_name);
        return 0;
    }

    return system(command) == 0;
}

static void restore_brightness_100(void) {
    int index;

    for (index = 0; index < active_monitor_count; index++) {
        set_brightness_for_output(active_monitors[index].name, 1.0);
    }
}

static void handle_signal(int sig) {
    (void)sig;
    keep_running = 0;
}

static double apply_curve(double avg, const Config *cfg) {
    double adjusted = clamp(avg + cfg->avg_bias, 0.0, 1.0);
    double x;
    double shaped;
    double shoulder;
    double output;

    adjusted = pow(adjusted, cfg->avg_input_gamma);

    x = (adjusted - cfg->curve_start_avg) / (cfg->curve_end_avg - cfg->curve_start_avg);
    x = clamp(x, 0.0, 1.0);

    shaped = pow(x, cfg->curve_gamma);
    shoulder = shaped * shaped * (3.0 - 2.0 * shaped);
    shaped = (1.0 - cfg->curve_shoulder) * shaped + cfg->curve_shoulder * shoulder;

    output = cfg->max_brightness - shaped * (cfg->max_brightness - cfg->min_brightness);
    return clamp(output, cfg->min_brightness, cfg->max_brightness);
}

static XImage *capture_root_image(Display *dpy, Window root, int *width, int *height) {
    XWindowAttributes attr;

    if (!XGetWindowAttributes(dpy, root, &attr)) {
        return NULL;
    }

    *width = attr.width;
    *height = attr.height;
    if (*width <= 0 || *height <= 0) {
        return NULL;
    }

    return XGetImage(dpy, root, 0, 0, (unsigned int)*width, (unsigned int)*height, AllPlanes, ZPixmap);
}

static double get_luminance_for_region(XImage *image, int image_width, int image_height,
                                       int region_x, int region_y, int region_width,
                                       int region_height, const Config *cfg) {
    int x0;
    int y0;
    int x1;
    int y1;
    int margin_x;
    int margin_y;
    int start_x;
    int end_x;
    int start_y;
    int end_y;
    double sum = 0.0;
    int total;
    int gx;
    int gy;

    if (image == NULL || region_width <= 0 || region_height <= 0) {
        return 0.5;
    }

    x0 = region_x < 0 ? 0 : region_x;
    y0 = region_y < 0 ? 0 : region_y;
    x1 = region_x + region_width;
    y1 = region_y + region_height;

    if (x1 > image_width) {
        x1 = image_width;
    }
    if (y1 > image_height) {
        y1 = image_height;
    }
    if (x1 <= x0 || y1 <= y0) {
        return 0.5;
    }

    margin_x = (x1 - x0) / 20;
    margin_y = (y1 - y0) / 20;
    start_x = x0 + margin_x;
    end_x = x1 - 1 - margin_x;
    start_y = y0 + margin_y;
    end_y = y1 - 1 - margin_y;

    if (end_x <= start_x) {
        start_x = x0;
        end_x = x1 - 1;
    }
    if (end_y <= start_y) {
        start_y = y0;
        end_y = y1 - 1;
    }

    total = cfg->grid_x * cfg->grid_y;

    for (gy = 0; gy < cfg->grid_y; gy++) {
        double fy = (cfg->grid_y == 1) ? 0.5 : (double)gy / (double)(cfg->grid_y - 1);
        int y = start_y + (int)((end_y - start_y) * fy);

        for (gx = 0; gx < cfg->grid_x; gx++) {
            double fx = (cfg->grid_x == 1) ? 0.5 : (double)gx / (double)(cfg->grid_x - 1);
            int x = start_x + (int)((end_x - start_x) * fx);
            unsigned long pixel = XGetPixel(image, x, y);
            double r = extract_channel_norm(pixel, image->red_mask);
            double g = extract_channel_norm(pixel, image->green_mask);
            double b = extract_channel_norm(pixel, image->blue_mask);
            double luminance = 0.299 * r + 0.587 * g + 0.114 * b;

            sum += luminance;
        }
    }

    return sum / (double)total;
}

static void free_monitors(Monitor *monitors) {
    free(monitors);
}

static void copy_output_name(Monitor *monitor, XRROutputInfo *output_info) {
    int name_length = output_info->nameLen;

    if (name_length >= MAX_OUTPUT_NAME) {
        name_length = MAX_OUTPUT_NAME - 1;
    }

    memcpy(monitor->name, output_info->name, (size_t)name_length);
    monitor->name[name_length] = '\0';
}

static void inherit_monitor_state(Monitor *new_monitors, int new_count,
                                  const Monitor *old_monitors, int old_count,
                                  const Config *cfg) {
    int new_index;

    for (new_index = 0; new_index < new_count; new_index++) {
        int old_index;

        new_monitors[new_index].last_brightness = cfg->max_brightness;
        new_monitors[new_index].applied_brightness = -1.0;
        new_monitors[new_index].last_verbose_log[0] = '\0';

        for (old_index = 0; old_index < old_count; old_index++) {
            if (strcmp(new_monitors[new_index].name, old_monitors[old_index].name) == 0) {
                new_monitors[new_index].last_brightness = old_monitors[old_index].last_brightness;
                new_monitors[new_index].applied_brightness = old_monitors[old_index].applied_brightness;
                snprintf(new_monitors[new_index].last_verbose_log,
                         sizeof(new_monitors[new_index].last_verbose_log), "%s",
                         old_monitors[old_index].last_verbose_log);
                break;
            }
        }
    }
}

static int query_active_monitors(Display *dpy, Window root, const Config *cfg,
                                 const Monitor *previous_monitors, int previous_count,
                                 Monitor **monitors_out, int *count_out) {
    XRRScreenResources *resources;
    Monitor *monitors = NULL;
    int count = 0;
    int index;

    resources = XRRGetScreenResourcesCurrent(dpy, root);
    if (resources == NULL) {
        log_error("Unable to query RandR screen resources");
        return 0;
    }

    for (index = 0; index < resources->noutput; index++) {
        XRROutputInfo *output_info = XRRGetOutputInfo(dpy, resources, resources->outputs[index]);
        XRRCrtcInfo *crtc_info;
        Monitor *resized;

        if (output_info == NULL) {
            continue;
        }

        if (output_info->connection != RR_Connected || output_info->crtc == None || output_info->nameLen <= 0) {
            XRRFreeOutputInfo(output_info);
            continue;
        }

        crtc_info = XRRGetCrtcInfo(dpy, resources, output_info->crtc);
        if (crtc_info == NULL) {
            XRRFreeOutputInfo(output_info);
            continue;
        }

        if (crtc_info->width <= 0 || crtc_info->height <= 0) {
            XRRFreeCrtcInfo(crtc_info);
            XRRFreeOutputInfo(output_info);
            continue;
        }

        resized = realloc(monitors, sizeof(*monitors) * (size_t)(count + 1));
        if (resized == NULL) {
            XRRFreeCrtcInfo(crtc_info);
            XRRFreeOutputInfo(output_info);
            free_monitors(monitors);
            XRRFreeScreenResources(resources);
            log_error("Out of memory while building monitor list");
            return 0;
        }

        monitors = resized;
        memset(&monitors[count], 0, sizeof(monitors[count]));
        monitors[count].output = resources->outputs[index];
        monitors[count].x = crtc_info->x;
        monitors[count].y = crtc_info->y;
        monitors[count].width = crtc_info->width;
        monitors[count].height = crtc_info->height;
        copy_output_name(&monitors[count], output_info);
        count++;

        XRRFreeCrtcInfo(crtc_info);
        XRRFreeOutputInfo(output_info);
    }

    XRRFreeScreenResources(resources);

    if (count == 0) {
        free_monitors(monitors);
        log_error("No active connected monitors were found");
        return 0;
    }

    inherit_monitor_state(monitors, count, previous_monitors, previous_count, cfg);
    *monitors_out = monitors;
    *count_out = count;
    return 1;
}

static void log_monitor_list(const Monitor *monitors, int count) {
    int index;

    for (index = 0; index < count; index++) {
        log_info("Monitor %s at %dx%d+%d+%d",
                 monitors[index].name,
                 monitors[index].width,
                 monitors[index].height,
                 monitors[index].x,
                 monitors[index].y);
    }
}

static bool monitor_lists_differ(const Monitor *left, int left_count,
                                 const Monitor *right, int right_count) {
    int index;

    if (left_count != right_count) {
        return true;
    }

    for (index = 0; index < left_count; index++) {
        if (strcmp(left[index].name, right[index].name) != 0 ||
            left[index].x != right[index].x ||
            left[index].y != right[index].y ||
            left[index].width != right[index].width ||
            left[index].height != right[index].height) {
            return true;
        }
    }

    return false;
}

static bool maybe_refresh_monitors(Display *dpy, Window root, const Config *cfg, unsigned long loop_count) {
    Monitor *new_monitors = NULL;
    int new_count = 0;
    unsigned long refresh_interval = (unsigned long)(cfg->fps > 0 ? cfg->fps : 60);

    if (loop_count == 0 || (loop_count % refresh_interval) != 0) {
        return false;
    }

    if (!query_active_monitors(dpy, root, cfg, active_monitors, active_monitor_count,
                               &new_monitors, &new_count)) {
        return false;
    }

    if (!monitor_lists_differ(new_monitors, new_count, active_monitors, active_monitor_count)) {
        free_monitors(new_monitors);
        return false;
    }

    free_monitors(active_monitors);
    active_monitors = new_monitors;
    active_monitor_count = new_count;
    log_info("Monitor topology updated");
    log_monitor_list(active_monitors, active_monitor_count);
    return true;
}

static void print_usage(const char *program_name) {
    fprintf(stdout,
            "Usage: %s [--verbose]\n"
            "Config path: $XDG_CONFIG_HOME/%s or ~/.config/%s\n",
            program_name,
            CONFIG_SUBPATH,
            CONFIG_SUBPATH);
}

int main(int argc, char **argv) {
    Display *dpy;
    Window root;
    Config config;
    char config_path[PATH_MAX];
    struct stat config_stat;
    bool config_stat_valid = false;
    bool config_file_found = false;
    bool loaded_from_file = false;
    char config_error[256];
    unsigned long loop_count = 0;
    int arg_index;

    for (arg_index = 1; arg_index < argc; arg_index++) {
        if (strcmp(argv[arg_index], "--verbose") == 0) {
            verbose_logging = 1;
        } else if (strcmp(argv[arg_index], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            log_error("Unknown argument: %s", argv[arg_index]);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (!resolve_config_path(config_path, sizeof(config_path))) {
        log_error("Unable to resolve the config path for %s", APP_NAME);
        return 1;
    }

    if (!load_config_file(config_path, &config, &loaded_from_file, config_error, sizeof(config_error))) {
        log_error("Unable to load config: %s", config_error);
        return 1;
    }

    config_file_found = loaded_from_file;
    if (loaded_from_file) {
        log_info("Loaded config from %s", config_path);
        if (stat_config_file(config_path, &config_stat)) {
            config_stat_valid = true;
        }
    } else {
        log_info("No config found at %s; using built-in defaults", config_path);
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    dpy = XOpenDisplay(NULL);
    if (dpy == NULL) {
        log_error("Cannot open display");
        return 1;
    }

    root = DefaultRootWindow(dpy);

    if (!query_active_monitors(dpy, root, &config, NULL, 0, &active_monitors, &active_monitor_count)) {
        XCloseDisplay(dpy);
        return 1;
    }

    log_info("%s started", APP_NAME);
    log_monitor_list(active_monitors, active_monitor_count);

    while (keep_running) {
        int root_width = 0;
        int root_height = 0;
        XImage *image;
        int monitor_index;

        maybe_reload_config(config_path, &config, &config_stat, &config_stat_valid, &config_file_found);
        maybe_refresh_monitors(dpy, root, &config, loop_count);

        image = capture_root_image(dpy, root, &root_width, &root_height);
        if (image == NULL) {
            log_warn("Unable to capture the root window image");
            sleep_for_frame(&config);
            loop_count++;
            continue;
        }

        for (monitor_index = 0; monitor_index < active_monitor_count; monitor_index++) {
            Monitor *monitor = &active_monitors[monitor_index];
            double avg = get_luminance_for_region(image, root_width, root_height,
                                                  monitor->x, monitor->y,
                                                  monitor->width, monitor->height,
                                                  &config);
            double target = apply_curve(avg, &config);
            double brightness = monitor->last_brightness * config.brightness_smoothing +
                                target * (1.0 - config.brightness_smoothing);
            char verbose_line[128];

            brightness = clamp(brightness, config.min_brightness, config.max_brightness);
            brightness = quantize_brightness(brightness);
            monitor->last_brightness = brightness;

            snprintf(verbose_line, sizeof(verbose_line),
                     "%.90s avg=%.3f brightness=%.3f",
                     monitor->name, avg, brightness);

            if (strcmp(verbose_line, monitor->last_verbose_log) != 0) {
                log_verbose("%s", verbose_line);
                snprintf(monitor->last_verbose_log, sizeof(monitor->last_verbose_log), "%s", verbose_line);
            }

            if (monitor->applied_brightness < 0.0 ||
                fabs(monitor->applied_brightness - brightness) >= 0.001) {
                if (set_brightness_for_output(monitor->name, brightness)) {
                    monitor->applied_brightness = brightness;
                } else {
                    log_warn("Failed to set brightness for %s", monitor->name);
                }
            }
        }

        XDestroyImage(image);
        sleep_for_frame(&config);
        loop_count++;
    }

    restore_brightness_100();
    free_monitors(active_monitors);
    active_monitors = NULL;
    active_monitor_count = 0;
    XCloseDisplay(dpy);
    log_info("%s stopped", APP_NAME);
    return 0;
}