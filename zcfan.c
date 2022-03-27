#include <errno.h>
#include <glob.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MILLIC_TO_C(n) (n / 1000)
#define TEMP_FILES_GLOB "/sys/class/thermal/thermal_zone*/temp"
#define FAN_CONTROL_FILE "/proc/acpi/ibm/fan"
#define TEMP_INVALID INT_MIN

#define max(x, y) ((x) > (y) ? (x) : (y))
#define expect(x)                                                              \
    do {                                                                       \
        if (!(x)) {                                                            \
            fprintf(stderr, "FATAL: !(%s) at %s:%s:%d\n", #x, __FILE__,        \
                    __func__, __LINE__);                                       \
            abort();                                                           \
        }                                                                      \
    } while (0)

enum FanLevel {
    FAN_OFF = 0,
    FAN_LOW = 2,
    FAN_MED = 4,
    FAN_MAX = 7,
};

struct Rule {
    int threshold;
    enum FanLevel fan_level;
};

/* Must be highest to lowest temp */
static const struct Rule rules[] = {
    {90, FAN_MAX},
    {80, FAN_MED},
    {70, FAN_LOW},
    {0, FAN_OFF},
};

static const char *prog_name = NULL;
static const unsigned int fan_hysteresis = 10;
static const unsigned int tick_hysteresis = 5;
static char output_buf[512];
static const struct Rule *current_rule = NULL;
static glob_t temp_files;
static volatile sig_atomic_t run = 1;

static int glob_err_handler(const char *epath, int eerrno) {
    fprintf(stderr, "glob: %s: %s\n", epath, strerror(eerrno));
    return 0;
}

static int read_temp_file(const char *filename) {
    FILE *f = fopen(filename, "re");
    int val;

    if (!f) {
        fprintf(stderr, "%s: fopen: %s\n", filename, strerror(errno));
        return errno;
    }

    expect(fscanf(f, "%d", &val) == 1);
    fclose(f);

    return val;
}

static int get_max_temp(void) {
    int max_temp = TEMP_INVALID;
    int ret = glob(TEMP_FILES_GLOB, 0, glob_err_handler, &temp_files);
    size_t i;

    if (ret) {
        expect(ret == GLOB_NOMATCH);
        fprintf(stderr, "%s: Could not find temperature file\n", prog_name);
        return TEMP_INVALID;
    }

    for (i = 0; i < temp_files.gl_pathc; i++) {
        int temp = read_temp_file(temp_files.gl_pathv[i]);
        max_temp = max(max_temp, temp);
    }
    globfree(&temp_files);

    if (max_temp == TEMP_INVALID) {
        fprintf(stderr, "Couldn't find any valid temperature\n");
        return TEMP_INVALID;
    }

    return MILLIC_TO_C(max_temp);
}

static void write_fan_level(const char *level) {
    FILE *f = fopen(FAN_CONTROL_FILE, "we");
    int ret;

    if (!f) {
        fprintf(stderr, "%s: fopen: %s\n", FAN_CONTROL_FILE, strerror(errno));
        return;
    }

    expect(setvbuf(f, NULL, _IONBF, 0) == 0); /* Make fprintf see errors */
    ret = fprintf(f, "level %s", level);
    if (ret < 0) {
        fprintf(stderr, "%s: write: %s%s\n", FAN_CONTROL_FILE, strerror(errno),
                errno == EINVAL ? " (did you enable fan_control=1?)" : "");
        return;
    }
    fclose(f);

    printf("Set fan level %s\n", level);
}

static void set_fan_level(void) {
    int max_temp = get_max_temp(), temp_penalty = 0, ret;
    static unsigned int tick_penalty = tick_hysteresis;
    size_t i;

    if (tick_penalty > 0) {
        tick_penalty--;
    }

    if (max_temp == TEMP_INVALID) {
        write_fan_level("full-speed");
        return;
    }

    for (i = 0; i < (sizeof(rules) / sizeof(rules[0])); i++) {
        const struct Rule *rule = rules + i;
        char level[sizeof("disengaged")];

        if (rule == current_rule) {
            if (tick_penalty) {
                return; /* Must wait longer until able to move down levels */
            }
            temp_penalty = fan_hysteresis;
        }

        if (rule->threshold < temp_penalty ||
            (rule->threshold - temp_penalty) < max_temp) {
            if (rule != current_rule) {
                current_rule = rule;
                tick_penalty = tick_hysteresis;
                ret = snprintf(level, sizeof(level), "%d", rule->fan_level);
                expect(ret >= 0 && (size_t)ret < sizeof(level));
                write_fan_level(level);
            }
            return;
        }
    }

    fprintf(stderr, "No rule matched?\n");
}

static void stop(int sig) { run = 0; }

int main(int argc, char *argv[]) {
    const struct sigaction sa_exit = {
        .sa_handler = stop,
    };

    expect(sigaction(SIGTERM, &sa_exit, NULL) == 0);
    expect(sigaction(SIGINT, &sa_exit, NULL) == 0);
    expect(setvbuf(stdout, output_buf, _IOLBF, sizeof(output_buf)) == 0);

    prog_name = argv[0];

    while (run) {
        set_fan_level();
        if (run) {
            sleep(1);
        }
    }

    write_fan_level("auto");
}
