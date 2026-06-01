#include "internal.h"
#include "lumi/compiler.h"

#include <stdlib.h>
#include <string.h>

int lumi_compile_source(const char *source, lumi_compile_result *out_result) {
    lumi_compile_options options = {0};
    return lumi_compile_source_with_options(source, &options, out_result);
}

int lumi_compile_source_with_options(const char *source, const lumi_compile_options *options, lumi_compile_result *out_result) {
    lumi_program program;
    lumi_parse_error parse_error;
    lumi_emit_error emit_error;
    int optimization_level = 0;

    if (source == NULL || out_result == NULL) {
        return 0;
    }
    if (options != NULL) {
        optimization_level = options->optimization_level;
        if (optimization_level < 0) {
            optimization_level = 0;
        }
        if (optimization_level > 3) {
            optimization_level = 3;
        }
    }

    memset(out_result, 0, sizeof(*out_result));
    memset(&program, 0, sizeof(program));
    memset(&parse_error, 0, sizeof(parse_error));
    memset(&emit_error, 0, sizeof(emit_error));

    if (!lumi_parse(source, &program, &parse_error)) {
        out_result->error_message = parse_error.message;
        out_result->error_line = parse_error.line;
        out_result->error_column = parse_error.column;
        return 0;
    }

    if (!lumi_emit_bytecode(&program, optimization_level, &out_result->bytecode, &emit_error)) {
        out_result->error_message = emit_error.message;
        out_result->error_line = emit_error.line;
        out_result->error_column = emit_error.column;
        lumi_program_free(&program);
        return 0;
    }

    lumi_program_free(&program);
    return 1;
}

void lumi_compile_result_free(lumi_compile_result *result) {
    if (result == NULL) {
        return;
    }

    lumi_bytecode_free(&result->bytecode);
    free(result->error_message);
    memset(result, 0, sizeof(*result));
}
