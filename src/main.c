#include "lumi/compiler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file(const char *path, size_t *out_size) {
    FILE *file;
    long size;
    size_t read_size;
    char *buffer;

    file = fopen(path, "rb");
    if (file == NULL) {
        return NULL;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }

    buffer = malloc((size_t)size + 1);
    if (buffer == NULL) {
        fclose(file);
        return NULL;
    }

    read_size = fread(buffer, 1, (size_t)size, file);
    fclose(file);
    if (read_size != (size_t)size) {
        free(buffer);
        return NULL;
    }

    buffer[size] = '\0';
    if (out_size != NULL) {
        *out_size = (size_t)size;
    }
    return buffer;
}

static int write_file(const char *path, const uint8_t *data, size_t size) {
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        return 0;
    }
    if (fwrite(data, 1, size, file) != size) {
        fclose(file);
        return 0;
    }
    fclose(file);
    return 1;
}

static void print_usage(const char *program) {
    fprintf(stderr, "usage: %s [-O[0-3]] <input.lumi> -o <output.lbc>\n", program);
}

static int parse_optimization(const char *arg, int *out_level) {
    const char *level_text;
    if (strcmp(arg, "-O") == 0) {
        *out_level = 1;
        return 1;
    }
    if (strncmp(arg, "-O", 2) != 0) {
        return 0;
    }
    level_text = arg + 2;
    if (level_text[0] == '=') {
        level_text++;
    }
    if (level_text[0] == '\0' || level_text[1] != '\0' || level_text[0] < '0' || level_text[0] > '3') {
        return 0;
    }
    *out_level = level_text[0] - '0';
    return 1;
}

int main(int argc, char **argv) {
    const char *input_path;
    const char *output_path;
    char *source;
    lumi_compile_result result;
    lumi_compile_options options;
    uint8_t *data;
    size_t data_size;
    int i;

    input_path = NULL;
    output_path = NULL;
    options.optimization_level = 0;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 >= argc || output_path != NULL) {
                print_usage(argv[0]);
                return 1;
            }
            output_path = argv[++i];
        } else if (strncmp(argv[i], "-O", 2) == 0) {
            if (!parse_optimization(argv[i], &options.optimization_level)) {
                fprintf(stderr, "invalid optimization level: %s\n", argv[i]);
                return 1;
            }
        } else if (argv[i][0] != '-') {
            if (input_path != NULL) {
                print_usage(argv[0]);
                return 1;
            }
            input_path = argv[i];
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }

    if (input_path == NULL || output_path == NULL) {
        print_usage(argv[0]);
        return 1;
    }

    source = read_file(input_path, NULL);
    if (source == NULL) {
        fprintf(stderr, "failed to read %s\n", input_path);
        return 1;
    }

    if (!lumi_compile_source_with_options(source, &options, &result)) {
        fprintf(stderr, "%zu:%zu: %s\n", result.error_line, result.error_column, result.error_message);
        free(source);
        lumi_compile_result_free(&result);
        return 1;
    }

    if (!lumi_bytecode_serialize(&result.bytecode, &data, &data_size)) {
        fprintf(stderr, "failed to serialize bytecode\n");
        free(source);
        lumi_compile_result_free(&result);
        return 1;
    }

    if (!write_file(output_path, data, data_size)) {
        fprintf(stderr, "failed to write %s\n", output_path);
        free(data);
        free(source);
        lumi_compile_result_free(&result);
        return 1;
    }

    free(data);
    free(source);
    lumi_compile_result_free(&result);
    return 0;
}
