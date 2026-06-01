#ifndef LUMI_BYTECODE_H
#define LUMI_BYTECODE_H

#include <stddef.h>
#include <stdint.h>

typedef enum lumi_program_type {
    LUMI_PROGRAM_STATIC = 1,
    LUMI_PROGRAM_ANIMATION = 2,
} lumi_program_type;

typedef enum lumi_opcode {
    LUMI_OP_PUSH_CONST_F32 = 1,
    LUMI_OP_LOAD_INPUT,
    LUMI_OP_LOAD_GLOBAL,
    LUMI_OP_STORE_GLOBAL,
    LUMI_OP_LOAD_KEY,
    LUMI_OP_STORE_KEY,
    LUMI_OP_ADD,
    LUMI_OP_SUB,
    LUMI_OP_MUL,
    LUMI_OP_DIV,
    LUMI_OP_MOD,
    LUMI_OP_NEG,
    LUMI_OP_NOT,
    LUMI_OP_EQ,
    LUMI_OP_NE,
    LUMI_OP_LT,
    LUMI_OP_LE,
    LUMI_OP_GT,
    LUMI_OP_GE,
    LUMI_OP_AND,
    LUMI_OP_OR,
    LUMI_OP_CALL_BUILTIN,
    LUMI_OP_JUMP,
    LUMI_OP_JUMP_IF_FALSE,
    LUMI_OP_SET_COLOR,
    LUMI_OP_HALT,
    LUMI_OP_CLAMP,
    LUMI_OP_DIST,
    LUMI_OP_RGB,
    LUMI_OP_HSV,
    LUMI_OP_DUP,
} lumi_opcode;

typedef enum lumi_input_slot {
    LUMI_INPUT_X = 0,
    LUMI_INPUT_Y,
    LUMI_INPUT_DT,
    LUMI_INPUT_SPEED,
    LUMI_INPUT_PRESSED,
    LUMI_INPUT_PRESS,
    LUMI_INPUT_COUNT,
} lumi_input_slot;

typedef enum lumi_builtin_id {
    LUMI_BUILTIN_ABS = 1,
    LUMI_BUILTIN_CLAMP,
    LUMI_BUILTIN_DIST,
    LUMI_BUILTIN_RGB,
    LUMI_BUILTIN_HSV,
    LUMI_BUILTIN_SIN,
    LUMI_BUILTIN_COS,
    LUMI_BUILTIN_SQRT,
    LUMI_BUILTIN_LERP,
    LUMI_BUILTIN_MIN,
    LUMI_BUILTIN_MAX,
    LUMI_BUILTIN_POW,
    LUMI_BUILTIN_RAND,
    LUMI_BUILTIN_CEIL,
    LUMI_BUILTIN_FLOOR,
    LUMI_BUILTIN_ROUND,
} lumi_builtin_id;

typedef struct lumi_bytecode {
    lumi_program_type program_type;
    float *constants;
    size_t constant_count;
    float *initial_globals;
    size_t global_count;
    float *initial_keys;
    size_t key_count;
    uint16_t max_stack_depth;
    uint8_t *init_code;
    size_t init_size;
    uint8_t *update_code;
    size_t update_size;
    uint8_t *render_code;
    size_t render_size;
} lumi_bytecode;

void lumi_bytecode_free(lumi_bytecode *bytecode);
int lumi_bytecode_serialize(const lumi_bytecode *bytecode, uint8_t **out_data, size_t *out_size);

#endif
