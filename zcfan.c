#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MILLIC_TO_C(n) (n / 1000)
#define FAN_CONTROL_FILE "/proc/acpi/ibm/fan"
#define TEMP_INVALID INT_MIN
#define TEMP_MIN INT_MIN + 1
#define NS_IN_SEC 1000000000L  // 1 second in nanoseconds
#define THRESHOLD_NS 200000000 // 0.2 seconds

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)
#define DEFAULT_WATCHDOG_SECS 120
#define S_DEFAULT_WATCHDOG_SECS STR(DEFAULT_WATCHDOG_SECS)

#define info(fmt, ...) fprintf(stderr, "[INF] " fmt, ##__VA_ARGS__)
#define err(fmt, ...) fprintf(stderr, "[ERR] " fmt, ##__VA_ARGS__)
#define max(x, y) ((x) > (y) ? (x) : (y))
#define expect(x)                                                              \
    do {                                                                       \
        if (!(x)) {                                                            \
            fprintf(stderr, "FATAL: !(%s) at %s:%s:%d\n", #x, __FILE__,        \
                    __func__, __LINE__);                                       \
            abort();                                                           \
        }                                                                      \
    } while (0)

#define CONFIG_MAX_STRLEN 15
#define S_CONFIG_MAX_STRLEN STR(CONFIG_MAX_STRLEN)

#define MAX_IGNORED_SENSORS 1024
#define SENSOR_NAME_MAX 256
static char ignored_sensors_arr[MAX_IGNORED_SENSORS][SENSOR_NAME_MAX];
static size_t num_to_ignore_sensors = 0;
static size_t num_ignored_sensors = 0;

#define MAX_SENSOR_FDS 4096
static int sensor_fds[MAX_SENSOR_FDS];
static size_t num_sensor_fds = 0;

/* Must be highest to lowest temp */
enum FanLevel { FAN_MAX, FAN_MED, FAN_LOW, FAN_OFF, FAN_INVALID };
struct Rule {
    char tpacpi_level[CONFIG_MAX_STRLEN + 1];
    int threshold;
    const char *name;
};
static struct Rule rules[] = {
    [FAN_MAX] = {"full-speed", 90, "maximum"},
    [FAN_MED] = {"4", 80, "medium"},
    [FAN_LOW] = {"1", 70, "low"},
    [FAN_OFF] = {"0", TEMP_MIN, "off"},
};

static struct timespec last_watchdog_ping = {0, 0};
static time_t watchdog_secs = DEFAULT_WATCHDOG_SECS;
static int temp_hysteresis = 10;
static const unsigned int tick_hysteresis = 3;
static char output_buf[512];
static const struct Rule *current_rule = NULL;
static volatile sig_atomic_t run = 1;
static int first_tick = 1; /* Stop running if errors are immediate */

enum resume_state {
    RESUME_NOT_DETECTED,
    RESUME_DETECTED,
};

static void exit_if_first_tick(void) {
    if (first_tick) {
        err("Quitting due to failure during first run\n");
        exit(1);
    }
}

static int64_t timespec_diff_ns(const struct timespec *start,
                                const struct timespec *end) {
    return ((int64_t)end->tv_sec - (int64_t)start->tv_sec) * NS_IN_SEC +
           (end->tv_nsec - start->tv_nsec);
}

static enum resume_state detect_suspend(void) {
    static struct timespec monotonic_prev, boottime_prev;
    struct timespec monotonic_now, boottime_now;

    expect(clock_gettime(CLOCK_MONOTONIC, &monotonic_now) == 0);
    expect(clock_gettime(CLOCK_BOOTTIME, &boottime_now) == 0);

    if (monotonic_prev.tv_sec == 0 && monotonic_prev.tv_nsec == 0) {
        monotonic_prev = monotonic_now;
        boottime_prev = boottime_now;
        return RESUME_NOT_DETECTED;
    }

    int64_t delta_monotonic = timespec_diff_ns(&monotonic_prev, &monotonic_now);
    int64_t delta_boottime = timespec_diff_ns(&boottime_prev, &boottime_now);

    monotonic_prev = monotonic_now;
    boottime_prev = boottime_now;

    return delta_boottime > delta_monotonic + THRESHOLD_NS
               ? RESUME_DETECTED
               : RESUME_NOT_DETECTED;
}

static void fscanf_ignore_sensor(FILE *f, long pos) {
    char sensor_name[SENSOR_NAME_MAX];
    int ret = fscanf(f, "ignore_sensor %255s ", sensor_name);
    if (ret == 1) {
        expect(num_to_ignore_sensors < MAX_IGNORED_SENSORS);
        snprintf(ignored_sensors_arr[num_to_ignore_sensors], SENSOR_NAME_MAX,
                 "%s", sensor_name);
        num_to_ignore_sensors++;
    } else {
        expect(fseek(f, pos, SEEK_SET) == 0);
    }
}

static bool is_sensor_name_ignored(DIR *sensor_dir) {
    int name_fd = openat(dirfd(sensor_dir), "name", O_RDONLY);
    if (name_fd < 0)
        return false;
    char sensor_name[SENSOR_NAME_MAX];
    ssize_t name_len = read(name_fd, sensor_name, SENSOR_NAME_MAX - 1);
    close(name_fd);
    if (name_len <= 0)
        return false;
    sensor_name[name_len] = '\0';
    sensor_name[strcspn(sensor_name, "\n")] = '\0';
    for (size_t i = 0; i < num_to_ignore_sensors; i++) {
        if (strcmp(sensor_name, ignored_sensors_arr[i]) == 0)
            return true;
    }
    return false;
}

static int full_speed_supported(void) {
    FILE *f = fopen(FAN_CONTROL_FILE, "re");
    char line[256]; // If exceeded, we'll just read again
    int found = 0;

    expect(f);

    while (fgets(line, sizeof(line), f) != NULL) {
        if (strstr(line, "full-speed") != NULL) {
            found = 1;
            break;
        }
    }

    fclose(f);
    return found;
}

static void add_sensor_fds(DIR *sensor_dir) {
    struct dirent *sensor_file;
    while ((sensor_file = readdir(sensor_dir)) != NULL) {
        if (strncmp(sensor_file->d_name, "temp", 4) != 0 ||
            !strstr(sensor_file->d_name, "_input"))
            continue;
        expect(num_sensor_fds < MAX_SENSOR_FDS);
        int temp_fd = openat(dirfd(sensor_dir), sensor_file->d_name, O_RDONLY);
        if (temp_fd < 0)
            continue;
        sensor_fds[num_sensor_fds++] = temp_fd;
    }
}

static void populate_sensor_fds(void) {
    int hwmon_fd = open("/sys/class/hwmon", O_RDONLY | O_DIRECTORY);
    if (hwmon_fd < 0) {
        err("open(/sys/class/hwmon): %s\n", strerror(errno));
        exit_if_first_tick();
    }
    DIR *hwmon_dir = fdopendir(hwmon_fd);
    if (!hwmon_dir) {
        err("fdopendir(/sys/class/hwmon): %s\n", strerror(errno));
        exit_if_first_tick();
    }

    struct dirent *hwmon_entry;
    while ((hwmon_entry = readdir(hwmon_dir)) != NULL) {
        int sensor_dir_fd = openat(dirfd(hwmon_dir), hwmon_entry->d_name,
                                   O_RDONLY | O_DIRECTORY);
        if (sensor_dir_fd < 0)
            continue;
        DIR *sensor_dir = fdopendir(sensor_dir_fd);
        if (!sensor_dir) {
            close(sensor_dir_fd);
            continue;
        }
        if (is_sensor_name_ignored(sensor_dir)) {
            num_ignored_sensors++;
            closedir(sensor_dir);
            continue;
        }
        add_sensor_fds(sensor_dir);
        closedir(sensor_dir);
    }
    closedir(hwmon_dir);
}

/* The kernel supports reading new values without reopening the FD */
static int read_temp_fd(int fd) {
    char buf[32];
    if (lseek(fd, 0, SEEK_SET) < 0)
        return TEMP_INVALID;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0)
        return TEMP_INVALID;
    buf[n] = '\0';
    int val;
    return (sscanf(buf, "%d", &val) == 1) ? val : TEMP_INVALID;
}

static int get_max_temp(void) {
    int max_temp = TEMP_INVALID;
    for (size_t i = 0; i < num_sensor_fds; i++) {
        int temp = read_temp_fd(sensor_fds[i]);
        max_temp = max(max_temp, temp);
    }

    if (max_temp == TEMP_INVALID) {
        err("Couldn't find any valid temperature\n");
        exit_if_first_tick();
        return TEMP_INVALID;
    }

    return MILLIC_TO_C(max_temp);
}

#define write_fan_level(level) write_fan("level", level)

static FILE *fan_control_fp;

static int write_fan(const char *command, const char *value) {
    if (!fan_control_fp) {
        fan_control_fp = fopen(FAN_CONTROL_FILE, "we");
        if (!fan_control_fp) {
            err("%s: fopen: %s%s\n", FAN_CONTROL_FILE, strerror(errno),
                errno == ENOENT ? " (is thinkpad_acpi loaded?)" : "");
            exit_if_first_tick();
            return -errno;
        }
        /* Make fprintf see errors */
        expect(setvbuf(fan_control_fp, NULL, _IONBF, 0) == 0);
    }

    int ret = fprintf(fan_control_fp, "%s %s", command, value);
    if (ret < 0) {
        err("%s: write: %s%s\n", FAN_CONTROL_FILE, strerror(errno),
            errno == EINVAL ? " (did you enable fan_control=1?)" : "");
        exit_if_first_tick();
        fclose(fan_control_fp);
        fan_control_fp = NULL;
        return -errno;
    }
    expect(clock_gettime(CLOCK_MONOTONIC, &last_watchdog_ping) == 0);
    fflush(fan_control_fp);
    return 0;
}

static void write_watchdog_timeout(const time_t timeout) {
    char timeout_s[sizeof(S_DEFAULT_WATCHDOG_SECS)]; /* max timeout value */
    int ret =
        snprintf(timeout_s, sizeof(timeout_s), "%" PRIuMAX, (uintmax_t)timeout);
    expect(ret >= 0 && (size_t)ret < sizeof(timeout_s));
    write_fan("watchdog", timeout_s);
}

enum set_fan_status {
    FAN_LEVEL_NOT_SET,
    FAN_LEVEL_SET,
    FAN_LEVEL_INVALID,
};

static enum set_fan_status set_fan_level(void) {
    int max_temp = get_max_temp(), temp_penalty = 0;
    static unsigned int tick_penalty = tick_hysteresis;

    if (tick_penalty > 0) {
        tick_penalty--;
    }

    if (max_temp == TEMP_INVALID) {
        write_fan_level("full-speed");
        return FAN_LEVEL_INVALID;
    }

    for (size_t i = 0; i < FAN_INVALID; i++) {
        const struct Rule *rule = rules + i;

        if (rule == current_rule) {
            if (tick_penalty) {
                // Must wait longer until able to move down levels
                return FAN_LEVEL_NOT_SET;
            }
            temp_penalty = temp_hysteresis;
        }

        if (rule->threshold < temp_penalty ||
            (rule->threshold - temp_penalty) < max_temp) {
            if (rule != current_rule) {
                current_rule = rule;
                tick_penalty = tick_hysteresis;
                printf("[FAN] Temperature now %dC, fan set to %s\n", max_temp,
                       rule->name);
                write_fan_level(rule->tpacpi_level);
                return FAN_LEVEL_SET;
            }
            return FAN_LEVEL_NOT_SET;
        }
    }

    err("No threshold matched?\n");
    return FAN_LEVEL_INVALID;
}

#define WATCHDOG_GRACE_PERIOD_SECS 2
static void maybe_ping_watchdog(void) {
    struct timespec now;

    expect(current_rule);
    expect(clock_gettime(CLOCK_MONOTONIC, &now) == 0);

    if (detect_suspend() == RESUME_DETECTED) {
        // On resume, some models need a manual fan write again, or they will
        // revert to "auto".
        info("Clock jump detected, possible resume. Rewriting fan level\n");
        write_fan_level(current_rule->tpacpi_level);
    }

    if (now.tv_sec - last_watchdog_ping.tv_sec <
        (watchdog_secs - WATCHDOG_GRACE_PERIOD_SECS)) {
        return;
    }

    // Transitioning from level 0 -> level 0 can cause a brief fan spinup on
    // some models, so don't reset the timer by write_fan_level().
    write_watchdog_timeout(watchdog_secs);
}

#define CONFIG_PATH "/etc/zcfan.conf"
#define fscanf_int_for_key(f, pos, name, dest)                                 \
    do {                                                                       \
        int val;                                                               \
        if (fscanf(f, name " %d ", &val) == 1) {                               \
            dest = val;                                                        \
        } else {                                                               \
            expect(fseek(f, pos, SEEK_SET) == 0);                              \
        }                                                                      \
    } while (0)

#define fscanf_str_for_key(f, pos, name, dest)                                 \
    do {                                                                       \
        char val[CONFIG_MAX_STRLEN + 1];                                       \
        if (fscanf(f, name " %" S_CONFIG_MAX_STRLEN "s ", val) == 1) {         \
            strncpy(dest, val, CONFIG_MAX_STRLEN);                             \
            dest[CONFIG_MAX_STRLEN] = '\0';                                    \
        } else {                                                               \
            expect(fseek(f, pos, SEEK_SET) == 0);                              \
        }                                                                      \
    } while (0)

static void get_config(void) {
    FILE *f;

    f = fopen(CONFIG_PATH, "re");
    if (!f) {
        if (errno != ENOENT) {
            err("%s: fopen: %s\n", CONFIG_PATH, strerror(errno));
            exit_if_first_tick();
        }
        return;
    }

    while (!feof(f)) {
        long pos = ftell(f);
        int ch;
        expect(pos >= 0);
        fscanf_int_for_key(f, pos, "max_temp", rules[FAN_MAX].threshold);
        fscanf_int_for_key(f, pos, "med_temp", rules[FAN_MED].threshold);
        fscanf_int_for_key(f, pos, "low_temp", rules[FAN_LOW].threshold);
        fscanf_int_for_key(f, pos, "watchdog_secs", watchdog_secs);
        fscanf_int_for_key(f, pos, "temp_hysteresis", temp_hysteresis);
        fscanf_str_for_key(f, pos, "max_level", rules[FAN_MAX].tpacpi_level);
        fscanf_str_for_key(f, pos, "med_level", rules[FAN_MED].tpacpi_level);
        fscanf_str_for_key(f, pos, "low_level", rules[FAN_LOW].tpacpi_level);
        fscanf_ignore_sensor(f, pos);
        if (ftell(f) == pos) {
            while ((ch = fgetc(f)) != EOF && ch != '\n') {}
        }
    }

    /* Maximum value handled by the kernel is 120, and
     * (watchdog_secs - WATCHDOG_GRACE_PERIOD_SECS) must stay positive. */
    if (watchdog_secs < WATCHDOG_GRACE_PERIOD_SECS ||
        watchdog_secs > DEFAULT_WATCHDOG_SECS) {
        err("%s: value for the watchdog_secs directive has to be between %d and %d\n",
            CONFIG_PATH, WATCHDOG_GRACE_PERIOD_SECS, DEFAULT_WATCHDOG_SECS);
        exit(1);
    }

    fclose(f);
}

static void print_thresholds(void) {
    for (size_t i = 0; i < FAN_OFF; i++) {
        const struct Rule *rule = rules + i;
        printf("[CFG] At %dC fan is set to %s\n", rule->threshold, rule->name);
    }
    printf("[CFG] Ignored %zu present sensors based on config\n",
           num_ignored_sensors);
}

static void stop(int sig) {
    (void)sig;
    run = 0;
}

int main(int argc, char *argv[]) {
    const struct sigaction sa_exit = {
        .sa_handler = stop,
    };

    (void)argv;

    if (argc != 1) {
        printf("zcfan: Zero-configuration ThinkPad fan daemon.\n\n");
        printf("  [any argument]     Show this help\n\n");
        printf("See the zcfan(1) man page for details.\n");
        return 0;
    }

    get_config();
    expect(sigaction(SIGTERM, &sa_exit, NULL) == 0);
    expect(sigaction(SIGINT, &sa_exit, NULL) == 0);
    expect(setvbuf(stdout, output_buf, _IOLBF, sizeof(output_buf)) == 0);

    if (!full_speed_supported()) {
        err("level \"full-speed\" not supported, using level 7\n");
        strncpy(rules[FAN_MAX].tpacpi_level, "7", CONFIG_MAX_STRLEN);
        rules[FAN_MAX].tpacpi_level[CONFIG_MAX_STRLEN] = '\0';
    }

    write_watchdog_timeout(watchdog_secs);
    populate_sensor_fds();
    print_thresholds();

    while (run) {
        enum set_fan_status set = set_fan_level();
        if (set != FAN_LEVEL_SET) {
            maybe_ping_watchdog();
        }

        if (run) {
            sleep(1);
            first_tick = 0;
        }
    }

    printf("[FAN] Quit requested, reenabling thinkpad_acpi fan control\n");
    if (write_fan_level("auto") == 0) {
        write_watchdog_timeout(0);
    }
    for (size_t i = 0; i < num_sensor_fds; i++) {
        close(sensor_fds[i]);
    }
    if (fan_control_fp) {
        fclose(fan_control_fp);
    }
}
