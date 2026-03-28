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
    uint16_t max_stack_depth;
    uint16_t reserved;
    uint32_t code_size;
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
        fail("failed to read bytecode file size");
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

static void expect_byte_array(const uint8_t *actual, uint32_t actual_count, const char *expected_text) {
    int expected[512];
    int count;
    uint32_t i;

    count = parse_int_list(expected_text, expected, 512);
    if ((uint32_t)count != actual_count) {
        fprintf(stderr, "code size mismatch: got %u expected %d\n", actual_count, count);
        exit(1);
    }
    for (i = 0; i < actual_count; ++i) {
        if (actual[i] != (uint8_t)expected[i]) {
            fprintf(stderr, "code[%u] mismatch: got %u expected %d\n", i, actual[i], expected[i]);
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
    const uint8_t *code;
    uint32_t expected_program_type;
    uint32_t expected_constant_count;
    uint32_t expected_global_count;
    uint32_t expected_max_stack;
    size_t expected_size;

    if (argc != 9) {
        fprintf(stderr, "usage: %s <file> <program_type> <constant_count> <global_count> <max_stack> <constants> <globals> <code>\n", argv[0]);
        return 1;
    }

    data = read_file(argv[1], &data_size);
    if (data_size < sizeof(header)) {
        free(data);
        fail("bytecode file too small");
    }

    memcpy(&header, data, sizeof(header));
    if (header.magic != 0x4C554D49u) {
        free(data);
        fail("bad magic");
    }
    if (header.version != 1) {
        free(data);
        fail("bad bytecode version");
    }
    if (header.reserved != 0) {
        free(data);
        fail("reserved field must be zero");
    }

    expected_program_type = (uint32_t)strtoul(argv[2], NULL, 10);
    expected_constant_count = (uint32_t)strtoul(argv[3], NULL, 10);
    expected_global_count = (uint32_t)strtoul(argv[4], NULL, 10);
    expected_max_stack = (uint32_t)strtoul(argv[5], NULL, 10);

    if (header.program_type != expected_program_type) {
        fprintf(stderr, "program type mismatch: got %u expected %u\n", header.program_type, expected_program_type);
        return 1;
    }
    if (header.constant_count != expected_constant_count) {
        fprintf(stderr, "constant count mismatch: got %u expected %u\n", header.constant_count, expected_constant_count);
        return 1;
    }
    if (header.global_count != expected_global_count) {
        fprintf(stderr, "global count mismatch: got %u expected %u\n", header.global_count, expected_global_count);
        return 1;
    }
    if (header.max_stack_depth != expected_max_stack) {
        fprintf(stderr, "max stack mismatch: got %u expected %u\n", header.max_stack_depth, expected_max_stack);
        return 1;
    }

    expected_size = sizeof(header)
        + (size_t)header.constant_count * sizeof(float)
        + (size_t)header.global_count * sizeof(float)
        + (size_t)header.code_size;
    if (data_size != expected_size) {
        fprintf(stderr, "file size mismatch: got %zu expected %zu\n", data_size, expected_size);
        return 1;
    }

    constants = (const float *)(data + sizeof(header));
    globals = constants + header.constant_count;
    code = (const uint8_t *)(globals + header.global_count);

    expect_float_array(constants, header.constant_count, argv[6], "constants");
    expect_float_array(globals, header.global_count, argv[7], "globals");
    expect_byte_array(code, header.code_size, argv[8]);

    free(data);
    return 0;
}
