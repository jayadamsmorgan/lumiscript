#include "lumi/bytecode.h"

#include <stdlib.h>
#include <string.h>

typedef struct lumi_serialized_header {
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
} lumi_serialized_header;

void lumi_bytecode_free(lumi_bytecode *bytecode) {
    if (bytecode == NULL) {
        return;
    }

    free(bytecode->constants);
    free(bytecode->initial_globals);
    free(bytecode->initial_keys);
    free(bytecode->init_code);
    free(bytecode->update_code);
    free(bytecode->render_code);
    memset(bytecode, 0, sizeof(*bytecode));
}

int lumi_bytecode_serialize(const lumi_bytecode *bytecode, uint8_t **out_data, size_t *out_size) {
    lumi_serialized_header header;
    uint8_t *buffer;
    size_t total_size;
    uint8_t *cursor;

    if (bytecode == NULL || out_data == NULL || out_size == NULL) {
        return 0;
    }

    header.magic = 0x4C554D49u;
    header.version = 1;
    header.program_type = (uint16_t)bytecode->program_type;
    header.constant_count = (uint32_t)bytecode->constant_count;
    header.global_count = (uint32_t)bytecode->global_count;
    header.key_count = (uint32_t)bytecode->key_count;
    header.max_stack_depth = bytecode->max_stack_depth;
    header.reserved = 0;
    header.init_size = (uint32_t)bytecode->init_size;
    header.update_size = (uint32_t)bytecode->update_size;
    header.render_size = (uint32_t)bytecode->render_size;

    total_size = sizeof(header)
        + bytecode->constant_count * sizeof(float)
        + bytecode->global_count * sizeof(float)
        + bytecode->key_count * sizeof(float)
        + bytecode->init_size
        + bytecode->update_size
        + bytecode->render_size;
    buffer = malloc(total_size);
    if (buffer == NULL) {
        return 0;
    }

    cursor = buffer;
    memcpy(cursor, &header, sizeof(header));
    cursor += sizeof(header);

    if (bytecode->constant_count > 0) {
        memcpy(cursor, bytecode->constants, bytecode->constant_count * sizeof(float));
        cursor += bytecode->constant_count * sizeof(float);
    }

    if (bytecode->global_count > 0) {
        memcpy(cursor, bytecode->initial_globals, bytecode->global_count * sizeof(float));
        cursor += bytecode->global_count * sizeof(float);
    }

    if (bytecode->key_count > 0) {
        memcpy(cursor, bytecode->initial_keys, bytecode->key_count * sizeof(float));
        cursor += bytecode->key_count * sizeof(float);
    }

    if (bytecode->init_size > 0) {
        memcpy(cursor, bytecode->init_code, bytecode->init_size);
        cursor += bytecode->init_size;
    }
    if (bytecode->update_size > 0) {
        memcpy(cursor, bytecode->update_code, bytecode->update_size);
        cursor += bytecode->update_size;
    }
    if (bytecode->render_size > 0) {
        memcpy(cursor, bytecode->render_code, bytecode->render_size);
    }

    *out_data = buffer;
    *out_size = total_size;
    return 1;
}
