/*
 * main.c - Spinel AOT compiler entry point
 *
 * Usage: spinel --source=app.rb --output=app_aot.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <prism.h>
#include "codegen.h"

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s --source=FILE --output=FILE [--lib=DIR...]\n", prog);
    fprintf(stderr, "\nOptions:\n");
    fprintf(stderr, "  --source=FILE   Ruby source file to compile\n");
    fprintf(stderr, "  --output=FILE   Output C file (default: stdout)\n");
    fprintf(stderr, "  --lib=DIR       Library search path for require (repeatable)\n");
}

static char *read_file(const char *path, size_t *length) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Error: cannot open '%s'\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = malloc(len + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    fread(buf, 1, len, f);
    buf[len] = '\0';
    fclose(f);

    if (length) *length = (size_t)len;
    return buf;
}

int main(int argc, char **argv) {
    const char *source_path = NULL;
    const char *output_path = NULL;
    const char *lib_paths[16];
    int lib_path_count = 0;

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--source=", 9) == 0) {
            source_path = argv[i] + 9;
        } else if (strncmp(argv[i], "--output=", 9) == 0) {
            output_path = argv[i] + 9;
        } else if (strncmp(argv[i], "--lib=", 6) == 0) {
            if (lib_path_count < 16)
                lib_paths[lib_path_count++] = argv[i] + 6;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    if (!source_path) {
        fprintf(stderr, "Error: --source is required\n");
        usage(argv[0]);
        return 1;
    }

    /* Default lib path: <exe_dir>/lib/ */
    if (lib_path_count == 0) {
        static char default_lib[4096];
        char exe_dir[4096];
        /* Try /proc/self/exe for reliable exe path (Linux) */
        ssize_t len = readlink("/proc/self/exe", exe_dir, sizeof(exe_dir) - 1);
        if (len > 0) {
            exe_dir[len] = '\0';
        } else {
            snprintf(exe_dir, sizeof(exe_dir), "%s", argv[0]);
        }
        char *slash = strrchr(exe_dir, '/');
        if (slash) *slash = '\0'; else snprintf(exe_dir, sizeof(exe_dir), ".");
        snprintf(default_lib, sizeof(default_lib), "%s/lib", exe_dir);
        lib_paths[lib_path_count++] = default_lib;
    }

    /* Read source file */
    size_t source_len;
    char *source = read_file(source_path, &source_len);
    if (!source) return 1;

    /* Parse with Prism */
    pm_parser_t parser;
    pm_parser_init(&parser, (const uint8_t *)source, source_len, NULL);
    pm_node_t *root = pm_parse(&parser);

    /* Check for parse errors */
    if (parser.error_list.size > 0) {
        fprintf(stderr, "Parse errors in '%s':\n", source_path);
        pm_diagnostic_t *diag;
        for (diag = (pm_diagnostic_t *)parser.error_list.head;
             diag != NULL;
             diag = (pm_diagnostic_t *)diag->node.next) {
            ptrdiff_t offset = diag->location.start - parser.start;
            fprintf(stderr, "  offset %td: %s\n", offset, diag->message);
        }
        pm_node_destroy(&parser, root);
        pm_parser_free(&parser);
        free(source);
        return 1;
    }

    /* Open output file */
    FILE *out = stdout;
    if (output_path) {
        out = fopen(output_path, "w");
        if (!out) {
            fprintf(stderr, "Error: cannot open '%s' for writing\n", output_path);
            pm_node_destroy(&parser, root);
            pm_parser_free(&parser);
            free(source);
            return 1;
        }
    }

    /* Generate C code */
    codegen_ctx_t *ctx = (codegen_ctx_t *)calloc(1, sizeof(codegen_ctx_t));
    codegen_init(ctx, &parser, out, source_path);
    for (int i = 0; i < lib_path_count; i++)
        ctx->lib_paths[ctx->lib_path_count++] = lib_paths[i];
    codegen_program(ctx, root);

    /* Cleanup required files */
    for (int i = 0; i < ctx->required_file_count; i++) {
        pm_node_destroy(&ctx->required_files[i].parser, ctx->required_files[i].root);
        pm_parser_free(&ctx->required_files[i].parser);
        free(ctx->required_files[i].source);
        free(ctx->required_files[i].path);
    }
    /* Free dynamically allocated method arrays */
    for (int i = 0; i < ctx->class_count; i++)
        free(ctx->classes[i].methods);
    for (int i = 0; i < ctx->module_count; i++)
        free(ctx->modules[i].methods);
    free(ctx);

    /* Cleanup */
    if (out != stdout) fclose(out);
    pm_node_destroy(&parser, root);
    pm_parser_free(&parser);
    free(source);

    if (output_path)
        fprintf(stderr, "Wrote %s\n", output_path);

    return 0;
}
