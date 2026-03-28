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

int main(int argc, char **argv) {
    const char *input_path;
    const char *output_path;
    char *source;
    lumi_compile_result result;
    uint8_t *data;
    size_t data_size;

    if (argc != 4 || strcmp(argv[2], "-o") != 0) {
        fprintf(stderr, "usage: %s <input.lumi> -o <output.lbc>\n", argv[0]);
        return 1;
    }

    input_path = argv[1];
    output_path = argv[3];
    source = read_file(input_path, NULL);
    if (source == NULL) {
        fprintf(stderr, "failed to read %s\n", input_path);
        return 1;
    }

    if (!lumi_compile_source(source, &result)) {
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
