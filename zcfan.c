#include <errno.h>
#include <glob.h>
#include <stdio.h>
#include <string.h>

#define TEMP_FILES_GLOB "/sys/class/thermal/thermal_zone*/temp"

static int glob_err_handler(const char *epath, int eerrno) {
    fprintf(stderr, "glob: %s: %s\n", epath, strerror(eerrno));
    return 0;
}

static int get_max_temp(const char *prog_name) {
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
        printf("%s\n", results.gl_pathv[i]);
    }

    globfree(&results);
    return 0;
}

int main(int argc, char *argv[]) { get_max_temp(argv[0]); }
