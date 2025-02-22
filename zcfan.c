#include <errno.h>
#include <glob.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MILLIC_TO_C(n) (n / 1000)
#define TEMP_FILES_GLOB "/sys/class/hwmon/hwmon*/temp*_input"
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

static struct timespec last_watchdog_ping = {0};
static time_t watchdog_secs = DEFAULT_WATCHDOG_SECS;
static int temp_hysteresis = 10;
static const unsigned int tick_hysteresis = 3;
static char output_buf[512];
static const struct Rule *current_rule = NULL;
static volatile sig_atomic_t run = 1;
static int first_tick = 1;
static glob_t temp_files;

enum resume_state { RESUME_NOT_DETECTED, RESUME_DETECTED };

static void exit_if_first_tick(void) {
    if (first_tick) {
        err("Quitting due to failure during first run\n");
        exit(1);
    }
}

static int64_t timespec_diff_ns(const struct timespec *start,
                                const struct timespec *end) {
    return ((int64_t)end->tv_sec - start->tv_sec) * NS_IN_SEC +
           (end->tv_nsec - start->tv_nsec);
}

static enum resume_state detect_suspend(void) {
    static struct timespec mono_prev = {0}, boot_prev = {0};
    struct timespec mono_now, boot_now;
    expect(clock_gettime(CLOCK_MONOTONIC, &mono_now) == 0);
    expect(clock_gettime(CLOCK_BOOTTIME, &boot_now) == 0);
    if (mono_prev.tv_sec == 0 && mono_prev.tv_nsec == 0) {
        mono_prev = mono_now;
        boot_prev = boot_now;
        return RESUME_NOT_DETECTED;
    }
    int64_t delta_mono = timespec_diff_ns(&mono_prev, &mono_now);
    int64_t delta_boot = timespec_diff_ns(&boot_prev, &boot_now);
    mono_prev = mono_now;
    boot_prev = boot_now;
    return (delta_boot > delta_mono + THRESHOLD_NS) ? RESUME_DETECTED
                                                    : RESUME_NOT_DETECTED;
}

static int glob_err_handler(const char *epath, int eerrno) {
    err("glob: %s: %s\n", epath, strerror(eerrno));
    return 0;
}

static void populate_temp_files(void) {
    int ret = glob(TEMP_FILES_GLOB, 0, glob_err_handler, &temp_files);
    if (ret == GLOB_NOMATCH) {
        err("glob: No temperature sensor files matching pattern '%s' were found.\n",
            TEMP_FILES_GLOB);
        exit_if_first_tick();
    }
    expect(ret == 0);
}

static int full_speed_supported(void) {
    FILE *f = fopen(FAN_CONTROL_FILE, "re");
    expect(f);
    char line[256];
    int supported = 0;
    while (fgets(line, sizeof(line), f))
        if (strstr(line, "full-speed"))
            supported = 1;
    fclose(f);
    return supported;
}

static int read_temp_file(const char *filename) {
    FILE *f = fopen(filename, "re");
    if (!f)
        return TEMP_INVALID;
    int val;
    if (fscanf(f, "%d", &val) != 1)
        val = TEMP_INVALID;
    fclose(f);
    return val;
}

static int get_max_temp(void) {
    int max_temp = TEMP_INVALID;
    for (size_t i = 0; i < temp_files.gl_pathc; i++)
        max_temp = max(max_temp, read_temp_file(temp_files.gl_pathv[i]));
    if (max_temp == TEMP_INVALID) {
        err("Couldn't find any valid temperature\n");
        exit_if_first_tick();
    }
    return MILLIC_TO_C(max_temp);
}

#define write_fan_level(level) write_fan("level", level)
static int write_fan(const char *command, const char *value) {
    FILE *f = fopen(FAN_CONTROL_FILE, "we");
    if (!f) {
        err("%s: fopen: %s%s\n", FAN_CONTROL_FILE, strerror(errno),
            errno == ENOENT ? " (is thinkpad_acpi loaded?)" : "");
        exit_if_first_tick();
        return -errno;
    }
    expect(setvbuf(f, NULL, _IONBF, 0) == 0);
    if (fprintf(f, "%s %s", command, value) < 0) {
        err("%s: write: %s%s\n", FAN_CONTROL_FILE, strerror(errno),
            errno == EINVAL ? " (did you enable fan_control=1?)" : "");
        exit_if_first_tick();
        fclose(f);
        return -errno;
    }
    expect(clock_gettime(CLOCK_MONOTONIC, &last_watchdog_ping) == 0);
    fclose(f);
    return 0;
}

static void write_watchdog_timeout(const time_t timeout) {
    char buf[sizeof(S_DEFAULT_WATCHDOG_SECS)];
    int ret = snprintf(buf, sizeof(buf), "%" PRIuMAX, (uintmax_t)timeout);
    expect(ret >= 0 && (size_t)ret < sizeof(buf));
    write_fan("watchdog", buf);
}

enum set_fan_status { FAN_LEVEL_NOT_SET, FAN_LEVEL_SET, FAN_LEVEL_INVALID };

static enum set_fan_status set_fan_level(void) {
    int max_temp = get_max_temp();
    static unsigned int tick_penalty = tick_hysteresis;
    if (tick_penalty)
        tick_penalty--;
    if (max_temp == TEMP_INVALID) {
        write_fan_level("full-speed");
        return FAN_LEVEL_INVALID;
    }
    int penalty = current_rule ? temp_hysteresis : 0;
    for (size_t i = 0; i < FAN_INVALID; i++) {
        const struct Rule *rule = rules + i;
        if (rule == current_rule && tick_penalty)
            return FAN_LEVEL_NOT_SET;
        if (max_temp > rule->threshold - penalty) {
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
    FILE *f = fopen(CONFIG_PATH, "re");
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
    for (size_t i = 0; i < FAN_OFF; i++)
        printf("[CFG] At %dC fan is set to %s\n", rules[i].threshold,
               rules[i].name);
}

static void stop(int sig) {
    (void)sig;
    run = 0;
}

int main(int argc, char *argv[]) {
    (void)argv;
    const struct sigaction sa_exit = {.sa_handler = stop};
    if (argc != 1) {
        printf("zcfan: Zero-configuration ThinkPad fan daemon.\n\n"
               "  [any argument]     Show this help\n\n"
               "See the zcfan(1) man page for details.\n");
        return 0;
    }
    get_config();
    print_thresholds();
    expect(sigaction(SIGTERM, &sa_exit, NULL) == 0);
    expect(sigaction(SIGINT, &sa_exit, NULL) == 0);
    expect(setvbuf(stdout, output_buf, _IOLBF, sizeof(output_buf)) == 0);
    if (!full_speed_supported()) {
        err("level \"full-speed\" not supported, using level 7\n");
        strncpy(rules[FAN_MAX].tpacpi_level, "7", CONFIG_MAX_STRLEN);
        rules[FAN_MAX].tpacpi_level[CONFIG_MAX_STRLEN] = '\0';
    }
    write_watchdog_timeout(watchdog_secs);
    populate_temp_files();
    while (run) {
        if (set_fan_level() != FAN_LEVEL_SET)
            maybe_ping_watchdog();
        sleep(1);
        first_tick = 0;
    }
    globfree(&temp_files);
    printf("[FAN] Quit requested, reenabling thinkpad_acpi fan control\n");
    if (write_fan_level("auto") == 0)
        write_watchdog_timeout(0);
}
