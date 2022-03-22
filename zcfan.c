#include <errno.h>
#include <glob.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define TEMP_FILES_GLOB "/sys/class/thermal/thermal_zone*/temp"
#define TEMP_MAX_MCEL 150000

static int glob_err_handler(const char *epath, int eerrno) {
    fprintf(stderr, "glob: %s: %s\n", epath, strerror(eerrno));
    return 0;
}

static int64_t read_temp_file(const char *filename) {
    FILE *f;
    int ret;
    int64_t val;

    f = fopen(filename, "re");
    if (!f) {
        fprintf(stderr, "%s: fopen: %s", filename, strerror(errno));
        return errno;
    }

    ret = fscanf(f, "%" PRIi64, &val);
    if (!ret) {
        fprintf(stderr, "%s: fscanf: %s", filename, strerror(errno));
        return errno;
    }

    if (val <= 0 || val > TEMP_MAX_MCEL) {
        fprintf(stderr, "%s: invalid temperature: %" PRIi64 "\n", filename,
                val);
        return -ERANGE;
    }

    return val;
}

static int get_max_temp(const char *prog_name) {
    int64_t max_temp = 0;
    glob_t results;
    int ret;
    size_t i;

    ret = glob(TEMP_FILES_GLOB, 0, glob_err_handler, &results);
    if (ret != 0) {
        const char *err = "glob: Unknown error";

        switch (ret) {
            case GLOB_NOMATCH:
                err = "Could not find temperature file";
                break;
            case GLOB_NOSPACE:
                err = "glob: Out of memory";
                break;
            default:
                /*
                 * GLOB_ABORTED can't happen because we don't use GLOB_ERR and
                 * glob_err_handler always returns success
                 */
                break;
        }
        fprintf(stderr, "%s: %s\n", prog_name, err);
    }

    for (i = 0; i < results.gl_pathc; i++) {
        const char *tf = results.gl_pathv[i];
        int64_t temp = read_temp_file(tf);

        if (temp > max_temp) {
            max_temp = temp;
        }
    }

    globfree(&results);

    if (max_temp == 0) {
        fprintf(stderr, "Couldn't find any valid temperature\n");
        return -ENODATA;
    }

    printf("Max temp: %" PRIi64 "\n", max_temp);

    return 0;
}

int main(int argc, char *argv[]) { return !!get_max_temp(argv[0]); }
