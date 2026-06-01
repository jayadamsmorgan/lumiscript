#ifndef LUMI_COMPILER_H
#define LUMI_COMPILER_H

#include <stddef.h>

#include "lumi/bytecode.h"

typedef struct lumi_compile_result {
    lumi_bytecode bytecode;
    char *error_message;
    size_t error_line;
    size_t error_column;
} lumi_compile_result;

typedef struct lumi_compile_options {
    int optimization_level;
} lumi_compile_options;

int lumi_compile_source(const char *source, lumi_compile_result *out_result);
int lumi_compile_source_with_options(const char *source, const lumi_compile_options *options, lumi_compile_result *out_result);
void lumi_compile_result_free(lumi_compile_result *result);

#endif
