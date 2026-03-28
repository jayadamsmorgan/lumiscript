#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct file_header {
    uint32_t magic;
    uint16_t version;
    uint16_t program_type;
    uint32_t constant_count;
    uint32_t global_count;
    uint32_t key_count;
    uint16_t max_stack_depth;
    uint16_t reserved;
    uint32_t init_size;
    uint32_t update_size;
    uint32_t render_size;
} file_header;

static void fail(const char *message) {
    fprintf(stderr, "%s\n", message);
    exit(1);
}

static uint8_t *read_file(const char *path, size_t *out_size) {
    FILE *file;
    long size;
    uint8_t *buffer;
    size_t read_size;

    file = fopen(path, "rb");
    if (file == NULL) {
        fail("failed to open bytecode file");
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        fail("failed to seek bytecode file");
    }
    size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        fail("failed to read bytecode size");
    }
    buffer = malloc((size_t)size);
    if (buffer == NULL) {
        fclose(file);
        fail("out of memory");
    }
    read_size = fread(buffer, 1, (size_t)size, file);
    fclose(file);
    if (read_size != (size_t)size) {
        free(buffer);
        fail("failed to read bytecode file");
    }
    *out_size = (size_t)size;
    return buffer;
}

static int parse_int_list(const char *text, int *out_values, size_t max_values) {
    char *copy;
    char *token;
    int count = 0;

    if (strcmp(text, "-") == 0) {
        return 0;
    }
    copy = malloc(strlen(text) + 1);
    if (copy == NULL) {
        fail("out of memory");
    }
    strcpy(copy, text);
    token = strtok(copy, ",");
    while (token != NULL) {
        char *end;
        long value;
        if ((size_t)count >= max_values) {
            free(copy);
            fail("too many expected values");
        }
        value = strtol(token, &end, 0);
        if (*end != '\0') {
            free(copy);
            fail("failed to parse integer list");
        }
        out_values[count++] = (int)value;
        token = strtok(NULL, ",");
    }
    free(copy);
    return count;
}

static void expect_float_array(const float *actual, uint32_t actual_count, const char *expected_text, const char *label) {
    int expected[128];
    int count;
    uint32_t i;

    count = parse_int_list(expected_text, expected, 128);
    if ((uint32_t)count != actual_count) {
        fprintf(stderr, "%s count mismatch: got %u expected %d\n", label, actual_count, count);
        exit(1);
    }
    for (i = 0; i < actual_count; ++i) {
        if (actual[i] != (float)expected[i]) {
            fprintf(stderr, "%s[%u] mismatch: got %.9g expected %d\n", label, i, actual[i], expected[i]);
            exit(1);
        }
    }
}

static void expect_byte_array(const uint8_t *actual, uint32_t actual_count, const char *expected_text, const char *label) {
    int expected[512];
    int count;
    uint32_t i;

    count = parse_int_list(expected_text, expected, 512);
    if ((uint32_t)count != actual_count) {
        fprintf(stderr, "%s size mismatch: got %u expected %d\n", label, actual_count, count);
        exit(1);
    }
    for (i = 0; i < actual_count; ++i) {
        if (actual[i] != (uint8_t)expected[i]) {
            fprintf(stderr, "%s[%u] mismatch: got %u expected %d\n", label, i, actual[i], expected[i]);
            exit(1);
        }
    }
}

int main(int argc, char **argv) {
    uint8_t *data;
    size_t data_size;
    file_header header;
    const float *constants;
    const float *globals;
    const float *keys;
    const uint8_t *init_code;
    const uint8_t *update_code;
    const uint8_t *render_code;

    if (argc != 13) {
        fprintf(stderr, "usage: %s <file> <program_type> <constant_count> <global_count> <key_count> <max_stack> <constants> <globals> <keys> <init> <update> <render>\n", argv[0]);
        return 1;
    }

    data = read_file(argv[1], &data_size);
    if (data_size < sizeof(header)) {
        free(data);
        fail("bytecode file too small");
    }
    memcpy(&header, data, sizeof(header));

    if (header.magic != 0x4C554D49u || header.version != 1 || header.reserved != 0) {
        free(data);
        fail("invalid bytecode header");
    }
    if (header.program_type != (uint16_t)strtoul(argv[2], NULL, 10)
        || header.constant_count != (uint32_t)strtoul(argv[3], NULL, 10)
        || header.global_count != (uint32_t)strtoul(argv[4], NULL, 10)
        || header.key_count != (uint32_t)strtoul(argv[5], NULL, 10)
        || header.max_stack_depth != (uint16_t)strtoul(argv[6], NULL, 10)) {
        free(data);
        fail("header field mismatch");
    }

    constants = (const float *)(data + sizeof(header));
    globals = constants + header.constant_count;
    keys = globals + header.global_count;
    init_code = (const uint8_t *)(keys + header.key_count);
    update_code = init_code + header.init_size;
    render_code = update_code + header.update_size;

    expect_float_array(constants, header.constant_count, argv[7], "constants");
    expect_float_array(globals, header.global_count, argv[8], "globals");
    expect_float_array(keys, header.key_count, argv[9], "keys");
    expect_byte_array(init_code, header.init_size, argv[10], "init");
    expect_byte_array(update_code, header.update_size, argv[11], "update");
    expect_byte_array(render_code, header.render_size, argv[12], "render");

    free(data);
    return 0;
}
