#include <errno.h>
#include <glob.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define C_TO_MILLIC(n) (n * 1000)
#define MILLIC_TO_C(n) (n / 1000)
#define TEMP_FILES_GLOB "/sys/class/thermal/thermal_zone*/temp"
#define FAN_CONTROL_FILE "/proc/acpi/ibm/fan"
#define TEMP_MAX_MCEL C_TO_MILLIC(150)
#define TEMP_INVALID INT_MIN

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
    unsigned int threshold;
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
static int temp_files_populated = 0;
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

    expect(fscanf(f, "%d", &val));
    fclose(f);

    if (val <= 0 || val > TEMP_MAX_MCEL) {
        fprintf(stderr, "%s: invalid temperature: %d\n", filename, val);
        return -ERANGE;
    }

    return val;
}

static int get_max_temp(void) {
    int max_temp = 0;
    size_t i;

    if (!temp_files_populated) {
        int ret = glob(TEMP_FILES_GLOB, 0, glob_err_handler, &temp_files);
        if (ret) {
            const char *err = "glob: Unknown error";
            switch (ret) {
                case GLOB_NOMATCH:
                    err = "Could not find temperature file";
                    break;
                case GLOB_NOSPACE:
                    err = "glob: Out of memory";
                    break;
            }
            fprintf(stderr, "%s: %s\n", prog_name, err);
            return TEMP_INVALID;
        } else {
            temp_files_populated = 1;
        }
    }

    for (i = 0; i < temp_files.gl_pathc; i++) {
        int temp = read_temp_file(temp_files.gl_pathv[i]);
        if (temp > max_temp) {
            max_temp = temp;
        }
    }

    if (max_temp == 0) {
        fprintf(stderr, "Couldn't find any valid temperature\n");
        return TEMP_INVALID;
    }

    return MILLIC_TO_C(max_temp);
}

static void write_fan_level(const char *level) {
    FILE *f = fopen(FAN_CONTROL_FILE, "we");
    if (!f) {
        fprintf(stderr, "%s: fopen: %s\n", FAN_CONTROL_FILE, strerror(errno));
        return;
    }

    expect(fprintf(f, "level %s", level));
    fclose(f);

    printf("Set fan level %s\n", level);
}

static void set_fan_level(void) {
    int max_temp = get_max_temp();
    unsigned int temp_penalty = 0;
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
            (rule->threshold - temp_penalty) < (unsigned int)max_temp) {
            if (rule != current_rule) {
                current_rule = rule;
                tick_penalty = tick_hysteresis;
                expect((size_t)snprintf(level, sizeof(level), "%d",
                                        rule->fan_level) < sizeof(level));
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
    sigset_t mask;

    sigfillset(&mask);
    expect(sigaction(SIGTERM, &sa_exit, NULL) >= 0);
    expect(sigaction(SIGINT, &sa_exit, NULL) >= 0);
    expect(setvbuf(stdout, output_buf, _IOLBF, sizeof(output_buf)) == 0);

    prog_name = argv[0];

    while (run) {
        set_fan_level();
        sleep(1);
    }

    expect(sigprocmask(SIG_SETMASK, &mask, NULL) == 0);
    write_fan_level("auto");
    if (temp_files_populated) {
        globfree(&temp_files);
    }
}
