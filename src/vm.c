#include "lumi/vm.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef union lumi_cell {
    uint32_t u32;
    float f32;
} lumi_cell;

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

typedef enum exec_mode {
    EXEC_INIT = 0,
    EXEC_UPDATE,
    EXEC_RENDER,
} exec_mode;

static void set_error(lumi_vm_error *error, const char *message, size_t detail) {
    if (error != NULL) {
        error->message = message;
        error->detail = detail;
    }
}

static uint16_t read_u16(const uint8_t *data) {
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static int size_mul_overflow(size_t a, size_t b, size_t *out) {
    if (a != 0 && b > ((size_t)-1) / a) {
        return 1;
    }
    *out = a * b;
    return 0;
}

static int is_truthy(lumi_cell value) {
    return value.f32 != 0.0f;
}

static float input_value(uint8_t slot, const lumi_vm_inputs *inputs) {
    switch (slot) {
        case LUMI_INPUT_X: return inputs->x;
        case LUMI_INPUT_Y: return inputs->y;
        case LUMI_INPUT_DT: return inputs->dt;
        case LUMI_INPUT_SPEED: return inputs->speed;
        case LUMI_INPUT_PRESSED: return inputs->pressed;
        case LUMI_INPUT_PRESS: return inputs->press;
        default: return 0.0f;
    }
}

static int input_allowed(exec_mode mode, uint8_t slot) {
    if (mode == EXEC_RENDER) {
        return 1;
    }
    if (mode == EXEC_UPDATE) {
        return slot == LUMI_INPUT_DT || slot == LUMI_INPUT_SPEED;
    }
    return 0;
}

static uint8_t clamp_u8(float value) {
    if (value <= 0.0f) {
        return 0;
    }
    if (value >= 255.0f) {
        return 255;
    }
    return (uint8_t)(value + 0.5f);
}

static uint32_t pack_rgb(float r, float g, float b) {
    return ((uint32_t)clamp_u8(r) << 16) | ((uint32_t)clamp_u8(g) << 8) | (uint32_t)clamp_u8(b);
}

static uint32_t hsv_to_rgb(float h, float s, float v) {
    float hh;
    float c;
    float x;
    float m;
    float r;
    float g;
    float b;

    while (h < 0.0f) {
        h += 360.0f;
    }
    while (h >= 360.0f) {
        h -= 360.0f;
    }
    s = s < 0.0f ? 0.0f : (s > 100.0f ? 100.0f : s);
    v = v < 0.0f ? 0.0f : (v > 100.0f ? 100.0f : v);
    s /= 100.0f;
    v /= 100.0f;
    c = v * s;
    hh = h / 60.0f;
    x = c * (1.0f - fabsf(fmodf(hh, 2.0f) - 1.0f));
    m = v - c;

    if (hh < 1.0f) {
        r = c; g = x; b = 0.0f;
    } else if (hh < 2.0f) {
        r = x; g = c; b = 0.0f;
    } else if (hh < 3.0f) {
        r = 0.0f; g = c; b = x;
    } else if (hh < 4.0f) {
        r = 0.0f; g = x; b = c;
    } else if (hh < 5.0f) {
        r = x; g = 0.0f; b = c;
    } else {
        r = c; g = 0.0f; b = x;
    }

    return pack_rgb((r + m) * 255.0f, (g + m) * 255.0f, (b + m) * 255.0f);
}

int lumi_vm_measure_bytecode(const uint8_t *data, size_t data_size, lumi_vm_requirements *out_requirements, lumi_vm_error *out_error) {
    lumi_serialized_header header;
    size_t expected_size;

    if (data == NULL || out_requirements == NULL) {
        set_error(out_error, "invalid arguments", 0);
        return 0;
    }
    if (data_size < sizeof(header)) {
        set_error(out_error, "bytecode too small", data_size);
        return 0;
    }
    memcpy(&header, data, sizeof(header));
    if (header.magic != 0x4C554D49u) {
        set_error(out_error, "invalid bytecode magic", header.magic);
        return 0;
    }
    if (header.version != 1) {
        set_error(out_error, "unsupported bytecode version", header.version);
        return 0;
    }
    if (header.program_type != LUMI_PROGRAM_STATIC && header.program_type != LUMI_PROGRAM_ANIMATION) {
        set_error(out_error, "unsupported program type", header.program_type);
        return 0;
    }
    if (header.reserved != 0) {
        set_error(out_error, "reserved header field must be zero", header.reserved);
        return 0;
    }
    expected_size = sizeof(header)
        + (size_t)header.constant_count * sizeof(float)
        + (size_t)header.global_count * sizeof(float)
        + (size_t)header.key_count * sizeof(float)
        + (size_t)header.init_size
        + (size_t)header.update_size
        + (size_t)header.render_size;
    if (expected_size != data_size) {
        set_error(out_error, "bytecode size mismatch", expected_size);
        return 0;
    }
    out_requirements->program_type = (lumi_program_type)header.program_type;
    out_requirements->constant_count = header.constant_count;
    out_requirements->global_count = header.global_count;
    out_requirements->key_count = header.key_count;
    out_requirements->init_size = header.init_size;
    out_requirements->update_size = header.update_size;
    out_requirements->render_size = header.render_size;
    out_requirements->max_stack_depth = header.max_stack_depth;
    set_error(out_error, NULL, 0);
    return 1;
}

int lumi_vm_can_load(const lumi_vm_requirements *requirements, const lumi_vm_program_storage *program_storage,
    const lumi_vm_state_storage *state_storage, size_t key_slots, lumi_vm_error *out_error) {
    size_t total_key_cells;

    if (requirements == NULL || program_storage == NULL || state_storage == NULL) {
        set_error(out_error, "invalid arguments", 0);
        return 0;
    }
    if (size_mul_overflow(requirements->key_count, key_slots, &total_key_cells)) {
        set_error(out_error, "runtime key storage size overflow", key_slots);
        return 0;
    }
    if (requirements->constant_count > program_storage->constant_capacity) {
        set_error(out_error, "constant storage too small", requirements->constant_count);
        return 0;
    }
    if (requirements->global_count > program_storage->global_capacity) {
        set_error(out_error, "program global storage too small", requirements->global_count);
        return 0;
    }
    if (requirements->key_count > program_storage->key_capacity) {
        set_error(out_error, "program key storage too small", requirements->key_count);
        return 0;
    }
    if (requirements->init_size > program_storage->init_capacity) {
        set_error(out_error, "init code storage too small", requirements->init_size);
        return 0;
    }
    if (requirements->update_size > program_storage->update_capacity) {
        set_error(out_error, "update code storage too small", requirements->update_size);
        return 0;
    }
    if (requirements->render_size > program_storage->render_capacity) {
        set_error(out_error, "render code storage too small", requirements->render_size);
        return 0;
    }
    if (requirements->global_count > state_storage->global_capacity) {
        set_error(out_error, "runtime global storage too small", requirements->global_count);
        return 0;
    }
    if (total_key_cells > state_storage->key_cell_capacity) {
        set_error(out_error, "runtime key storage too small", total_key_cells);
        return 0;
    }
    if (requirements->max_stack_depth > state_storage->stack_capacity) {
        set_error(out_error, "runtime stack too small", requirements->max_stack_depth);
        return 0;
    }
    set_error(out_error, NULL, 0);
    return 1;
}

int lumi_vm_load_program(const uint8_t *data, size_t data_size, const lumi_vm_program_storage *storage,
    lumi_vm_program *out_program, lumi_vm_error *out_error) {
    lumi_vm_requirements req;
    const uint8_t *cursor = data + sizeof(lumi_serialized_header);

    if (storage == NULL || out_program == NULL) {
        set_error(out_error, "invalid arguments", 0);
        return 0;
    }
    if (!lumi_vm_measure_bytecode(data, data_size, &req, out_error)) {
        return 0;
    }
    if (req.constant_count > storage->constant_capacity || req.global_count > storage->global_capacity
        || req.key_count > storage->key_capacity || req.init_size > storage->init_capacity
        || req.update_size > storage->update_capacity || req.render_size > storage->render_capacity) {
        set_error(out_error, "program storage too small", 0);
        return 0;
    }
    memcpy(storage->constants, cursor, req.constant_count * sizeof(float));
    cursor += req.constant_count * sizeof(float);
    memcpy(storage->initial_globals, cursor, req.global_count * sizeof(float));
    cursor += req.global_count * sizeof(float);
    memcpy(storage->initial_keys, cursor, req.key_count * sizeof(float));
    cursor += req.key_count * sizeof(float);
    memcpy(storage->init_code, cursor, req.init_size);
    cursor += req.init_size;
    memcpy(storage->update_code, cursor, req.update_size);
    cursor += req.update_size;
    memcpy(storage->render_code, cursor, req.render_size);

    out_program->program_type = req.program_type;
    out_program->constants = storage->constants;
    out_program->constant_count = req.constant_count;
    out_program->initial_globals = storage->initial_globals;
    out_program->global_count = req.global_count;
    out_program->initial_keys = storage->initial_keys;
    out_program->key_count = req.key_count;
    out_program->init_code = storage->init_code;
    out_program->init_size = req.init_size;
    out_program->update_code = storage->update_code;
    out_program->update_size = req.update_size;
    out_program->render_code = storage->render_code;
    out_program->render_size = req.render_size;
    out_program->max_stack_depth = req.max_stack_depth;
    set_error(out_error, NULL, 0);
    return 1;
}

int lumi_vm_init_state(const lumi_vm_program *program, const lumi_vm_state_storage *storage,
    size_t key_slots, lumi_vm_state *out_state, lumi_vm_error *out_error) {
    size_t total_key_cells;
    if (program == NULL || storage == NULL || out_state == NULL) {
        set_error(out_error, "invalid arguments", 0);
        return 0;
    }
    if (program->global_count > storage->global_capacity) {
        set_error(out_error, "runtime global storage too small", program->global_count);
        return 0;
    }
    if (size_mul_overflow(program->key_count, key_slots, &total_key_cells)) {
        set_error(out_error, "runtime key storage size overflow", key_slots);
        return 0;
    }
    if (total_key_cells > storage->key_cell_capacity) {
        set_error(out_error, "runtime key storage too small", total_key_cells);
        return 0;
    }
    if (program->max_stack_depth > storage->stack_capacity) {
        set_error(out_error, "runtime stack too small", program->max_stack_depth);
        return 0;
    }
    out_state->globals = storage->globals;
    out_state->global_count = program->global_count;
    out_state->keys = storage->keys;
    out_state->key_count = program->key_count;
    out_state->key_slots = key_slots;
    out_state->stack = storage->stack;
    out_state->stack_capacity = storage->stack_capacity;
    lumi_vm_reset_state(program, out_state);
    set_error(out_error, NULL, 0);
    return 1;
}

void lumi_vm_reset_state(const lumi_vm_program *program, lumi_vm_state *state) {
    size_t i;
    size_t key;
    for (i = 0; i < program->global_count; ++i) {
        lumi_cell cell;
        cell.f32 = program->initial_globals[i];
        state->globals[i] = cell.u32;
    }
    for (key = 0; key < state->key_slots; ++key) {
        for (i = 0; i < program->key_count; ++i) {
            lumi_cell cell;
            cell.f32 = program->initial_keys[i];
            state->keys[key * program->key_count + i] = cell.u32;
        }
    }
}

static int push(lumi_cell *stack, size_t *sp, size_t capacity, lumi_cell value, lumi_vm_error *error) {
    if (*sp >= capacity) {
        set_error(error, "stack overflow", *sp);
        return 0;
    }
    stack[(*sp)++] = value;
    return 1;
}

static int pop(lumi_cell *stack, size_t *sp, lumi_cell *out_value, lumi_vm_error *error) {
    if (*sp == 0) {
        set_error(error, "stack underflow", 0);
        return 0;
    }
    *out_value = stack[--(*sp)];
    return 1;
}

static int execute_section(const lumi_vm_program *program, lumi_vm_state *state, const uint8_t *code, size_t code_size,
    exec_mode mode, size_t key_index, const lumi_vm_inputs *inputs, lumi_vm_output *out_output, lumi_vm_error *out_error) {
    size_t pc = 0;
    size_t sp = 0;
    size_t steps = 0;
    lumi_cell *stack = (lumi_cell *)state->stack;
    uint32_t *key_base = NULL;

    if (mode == EXEC_RENDER) {
        if (key_index >= state->key_slots) {
            set_error(out_error, "key index out of range", key_index);
            return 0;
        }
        key_base = state->keys + key_index * state->key_count;
    }

    memset(out_output, 0, sizeof(*out_output));
    while (pc < code_size) {
        uint8_t op = code[pc++];
        lumi_cell a;
        lumi_cell b;
        lumi_cell result;
        uint16_t index;
        steps++;

        switch (op) {
            case LUMI_OP_PUSH_CONST_F32:
                if (pc + 1 >= code_size) {
                    set_error(out_error, "truncated const operand", pc);
                    return 0;
                }
                index = read_u16(code + pc);
                pc += 2;
                if (index >= program->constant_count) {
                    set_error(out_error, "constant index out of range", index);
                    return 0;
                }
                result.f32 = program->constants[index];
                if (!push(stack, &sp, state->stack_capacity, result, out_error)) {
                    return 0;
                }
                break;
            case LUMI_OP_LOAD_INPUT:
                if (pc >= code_size) {
                    set_error(out_error, "truncated input operand", pc);
                    return 0;
                }
                if (!input_allowed(mode, code[pc])) {
                    set_error(out_error, "input not available in this section", code[pc]);
                    return 0;
                }
                result.f32 = input_value(code[pc++], inputs);
                if (!push(stack, &sp, state->stack_capacity, result, out_error)) {
                    return 0;
                }
                break;
            case LUMI_OP_LOAD_GLOBAL:
            case LUMI_OP_STORE_GLOBAL:
            case LUMI_OP_LOAD_KEY:
            case LUMI_OP_STORE_KEY:
                if (pc + 1 >= code_size) {
                    set_error(out_error, "truncated variable operand", pc);
                    return 0;
                }
                index = read_u16(code + pc);
                pc += 2;
                if ((op == LUMI_OP_LOAD_GLOBAL || op == LUMI_OP_STORE_GLOBAL) && index >= state->global_count) {
                    set_error(out_error, "global index out of range", index);
                    return 0;
                }
                if ((op == LUMI_OP_LOAD_KEY || op == LUMI_OP_STORE_KEY)) {
                    if (mode != EXEC_RENDER) {
                        set_error(out_error, "key variable access outside render", index);
                        return 0;
                    }
                    if (index >= state->key_count) {
                        set_error(out_error, "key index out of range", index);
                        return 0;
                    }
                }
                if (op == LUMI_OP_LOAD_GLOBAL) {
                    result.u32 = state->globals[index];
                    if (!push(stack, &sp, state->stack_capacity, result, out_error)) {
                        return 0;
                    }
                } else if (op == LUMI_OP_STORE_GLOBAL) {
                    if (!pop(stack, &sp, &result, out_error)) {
                        return 0;
                    }
                    state->globals[index] = result.u32;
                } else if (op == LUMI_OP_LOAD_KEY) {
                    result.u32 = key_base[index];
                    if (!push(stack, &sp, state->stack_capacity, result, out_error)) {
                        return 0;
                    }
                } else {
                    if (!pop(stack, &sp, &result, out_error)) {
                        return 0;
                    }
                    key_base[index] = result.u32;
                }
                break;
            case LUMI_OP_ADD:
            case LUMI_OP_SUB:
            case LUMI_OP_MUL:
            case LUMI_OP_DIV:
            case LUMI_OP_MOD:
            case LUMI_OP_EQ:
            case LUMI_OP_NE:
            case LUMI_OP_LT:
            case LUMI_OP_LE:
            case LUMI_OP_GT:
            case LUMI_OP_GE:
            case LUMI_OP_AND:
            case LUMI_OP_OR:
                if (!pop(stack, &sp, &b, out_error) || !pop(stack, &sp, &a, out_error)) {
                    return 0;
                }
                switch (op) {
                    case LUMI_OP_ADD: result.f32 = a.f32 + b.f32; break;
                    case LUMI_OP_SUB: result.f32 = a.f32 - b.f32; break;
                    case LUMI_OP_MUL: result.f32 = a.f32 * b.f32; break;
                    case LUMI_OP_DIV: result.f32 = b.f32 == 0.0f ? 0.0f : a.f32 / b.f32; break;
                    case LUMI_OP_MOD: result.f32 = b.f32 == 0.0f ? 0.0f : fmodf(a.f32, b.f32); break;
                    case LUMI_OP_EQ: result.f32 = a.f32 == b.f32 ? 1.0f : 0.0f; break;
                    case LUMI_OP_NE: result.f32 = a.f32 != b.f32 ? 1.0f : 0.0f; break;
                    case LUMI_OP_LT: result.f32 = a.f32 < b.f32 ? 1.0f : 0.0f; break;
                    case LUMI_OP_LE: result.f32 = a.f32 <= b.f32 ? 1.0f : 0.0f; break;
                    case LUMI_OP_GT: result.f32 = a.f32 > b.f32 ? 1.0f : 0.0f; break;
                    case LUMI_OP_GE: result.f32 = a.f32 >= b.f32 ? 1.0f : 0.0f; break;
                    case LUMI_OP_AND: result.f32 = (is_truthy(a) && is_truthy(b)) ? 1.0f : 0.0f; break;
                    case LUMI_OP_OR: result.f32 = (is_truthy(a) || is_truthy(b)) ? 1.0f : 0.0f; break;
                    default: result.f32 = 0.0f; break;
                }
                if (!push(stack, &sp, state->stack_capacity, result, out_error)) {
                    return 0;
                }
                break;
            case LUMI_OP_NEG:
            case LUMI_OP_NOT:
                if (!pop(stack, &sp, &a, out_error)) {
                    return 0;
                }
                result.f32 = op == LUMI_OP_NEG ? -a.f32 : (is_truthy(a) ? 0.0f : 1.0f);
                if (!push(stack, &sp, state->stack_capacity, result, out_error)) {
                    return 0;
                }
                break;
            case LUMI_OP_CALL_BUILTIN: {
                uint8_t builtin;
                uint8_t arg_count;
                lumi_cell args[4];
                size_t i;
                if (pc + 1 >= code_size) {
                    set_error(out_error, "truncated builtin operand", pc);
                    return 0;
                }
                builtin = code[pc++];
                arg_count = code[pc++];
                if (arg_count > 4) {
                    set_error(out_error, "builtin arg count too large", arg_count);
                    return 0;
                }
                for (i = 0; i < arg_count; ++i) {
                    if (!pop(stack, &sp, &args[arg_count - 1 - i], out_error)) {
                        return 0;
                    }
                }
                switch (builtin) {
                    case LUMI_BUILTIN_ABS: result.f32 = fabsf(args[0].f32); break;
                    case LUMI_BUILTIN_SIN: result.f32 = sinf(args[0].f32); break;
                    case LUMI_BUILTIN_COS: result.f32 = cosf(args[0].f32); break;
                    case LUMI_BUILTIN_SQRT:
                        if (args[0].f32 < 0.0f) {
                            set_error(out_error, "sqrt expects non-negative input", steps);
                            return 0;
                        }
                        result.f32 = sqrtf(args[0].f32);
                        break;
                    case LUMI_BUILTIN_CLAMP:
                        result.f32 = args[0].f32;
                        if (result.f32 < args[1].f32) {
                            result.f32 = args[1].f32;
                        }
                        if (result.f32 > args[2].f32) {
                            result.f32 = args[2].f32;
                        }
                        break;
                    case LUMI_BUILTIN_DIST: {
                        float dx = args[0].f32 - args[2].f32;
                        float dy = args[1].f32 - args[3].f32;
                        result.f32 = sqrtf(dx * dx + dy * dy);
                        break;
                    }
                    case LUMI_BUILTIN_LERP:
                        result.f32 = args[0].f32 + (args[1].f32 - args[0].f32) * args[2].f32;
                        break;
                    case LUMI_BUILTIN_MIN:
                        result.f32 = args[0].f32 < args[1].f32 ? args[0].f32 : args[1].f32;
                        break;
                    case LUMI_BUILTIN_MAX:
                        result.f32 = args[0].f32 > args[1].f32 ? args[0].f32 : args[1].f32;
                        break;
                    case LUMI_BUILTIN_POW:
                        result.f32 = powf(args[0].f32, args[1].f32);
                        break;
                    case LUMI_BUILTIN_RGB: result.u32 = pack_rgb(args[0].f32, args[1].f32, args[2].f32); break;
                    case LUMI_BUILTIN_HSV: result.u32 = hsv_to_rgb(args[0].f32, args[1].f32, args[2].f32); break;
                    default:
                        set_error(out_error, "unknown builtin", builtin);
                        return 0;
                }
                if (!push(stack, &sp, state->stack_capacity, result, out_error)) {
                    return 0;
                }
                break;
            }
            case LUMI_OP_JUMP:
                if (pc + 1 >= code_size) {
                    set_error(out_error, "truncated jump operand", pc);
                    return 0;
                }
                index = read_u16(code + pc);
                if (index >= code_size) {
                    set_error(out_error, "jump target out of range", index);
                    return 0;
                }
                pc = index;
                break;
            case LUMI_OP_JUMP_IF_FALSE:
                if (pc + 1 >= code_size) {
                    set_error(out_error, "truncated conditional jump operand", pc);
                    return 0;
                }
                index = read_u16(code + pc);
                pc += 2;
                if (!pop(stack, &sp, &a, out_error)) {
                    return 0;
                }
                if (!is_truthy(a)) {
                    if (index >= code_size) {
                        set_error(out_error, "jump target out of range", index);
                        return 0;
                    }
                    pc = index;
                }
                break;
            case LUMI_OP_SET_COLOR:
                if (mode != EXEC_RENDER) {
                    set_error(out_error, "color assignment outside render", 0);
                    return 0;
                }
                if (!pop(stack, &sp, &a, out_error)) {
                    return 0;
                }
                out_output->color = a.u32;
                out_output->has_color = 1;
                break;
            case LUMI_OP_HALT:
                out_output->instructions_executed = steps;
                set_error(out_error, NULL, 0);
                return 1;
            default:
                set_error(out_error, "unknown opcode", op);
                return 0;
        }
    }
    set_error(out_error, "program terminated without halt", pc);
    return 0;
}

int lumi_vm_run_init(const lumi_vm_program *program, lumi_vm_state *state, lumi_vm_output *out_output, lumi_vm_error *out_error) {
    lumi_vm_inputs inputs;
    memset(&inputs, 0, sizeof(inputs));
    return execute_section(program, state, program->init_code, program->init_size, EXEC_INIT, 0, &inputs, out_output, out_error);
}

int lumi_vm_run_update(const lumi_vm_program *program, lumi_vm_state *state, const lumi_vm_inputs *inputs,
    lumi_vm_output *out_output, lumi_vm_error *out_error) {
    return execute_section(program, state, program->update_code, program->update_size, EXEC_UPDATE, 0, inputs, out_output, out_error);
}

int lumi_vm_run_render(const lumi_vm_program *program, lumi_vm_state *state, size_t key_index,
    const lumi_vm_inputs *inputs, lumi_vm_output *out_output, lumi_vm_error *out_error) {
    return execute_section(program, state, program->render_code, program->render_size, EXEC_RENDER, key_index, inputs, out_output, out_error);
}
