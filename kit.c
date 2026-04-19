#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define MAX_STACK 64
#define MAX_SYMBOLS 512
#define MAX_MACROS 64
#define MAX_BLOCK_LINES 1024
#define MAX_INC_PATHS 16

typedef struct { int parent_emit, branch_taken, this_emit; } Frame;

typedef struct {
    char name[64];
    int is_string;
    int int_val;
    char str_val[256];
    int is_const;
} Symbol;

typedef struct {
    char name[64];
    char params[8][32];
    int param_count;
    char *body[MAX_BLOCK_LINES];
    int line_count;
} Macro;

Frame stack[MAX_STACK];
int sp = 0;
Symbol symbols[MAX_SYMBOLS];
int sym_count = 0;
Macro macros[MAX_MACROS];
int macro_count = 0;
char *inc_paths[MAX_INC_PATHS];
int inc_path_count = 0;
int minify = 0;
int trace = 0;

static FILE *kit_in = NULL;
static const char *kit_in_name = "stdin";

/* --- Error Reporting --- */
void kit_error(const char *file, int line, const char *msg) {
    fprintf(stderr, "%s:%d: error: %s\n", file, line, msg);
    exit(1);
}

/* --- Utilities --- */
void trim(char *s) {
    if (!s) return;
    char *e = s + strlen(s) - 1;
    while(e >= s && isspace((unsigned char)*e)) *e-- = '\0';
}

char* ltrim(char *s) { 
    while(*s && isspace((unsigned char)*s)) s++; 
    return s; 
}

int current_emit() { return sp == 0 ? 1 : stack[sp-1].this_emit; }

int is_numeric(const char *s) {
    if (*s == '-') s++;
    if (!*s) return 0;
    while (*s) { if (!isdigit((unsigned char)*s)) return 0; s++; }
    return 1;
}

Symbol* get_symbol_struct(const char *name) {
    if (*name == '@') name++;
    for(int i=0; i<sym_count; i++) {
        if(!strcmp(symbols[i].name, name)) return &symbols[i];
    }
    return NULL;
}

void set_symbol_int(const char *name, int val, int is_const) {
    if (*name == '@') name++;
    Symbol *s = get_symbol_struct(name);
    if (s) {
        if (!s->is_const || is_const) {
            s->is_string = 0;
            s->int_val = val;
            if (is_const) s->is_const = 1;
        }
        return;
    }
    if (sym_count < MAX_SYMBOLS) {
        strncpy(symbols[sym_count].name, name, 63);
        symbols[sym_count].is_string = 0;
        symbols[sym_count].int_val = val;
        symbols[sym_count].is_const = is_const;
        sym_count++;
    }
}

void set_symbol_str(const char *name, const char *val, int is_const) {
    if (*name == '@') name++;
    Symbol *s = get_symbol_struct(name);
    if (s) {
        if (!s->is_const || is_const) {
            s->is_string = 1;
            strncpy(s->str_val, val, 255);
            if (is_const) s->is_const = 1;
        }
        return;
    }
    if (sym_count < MAX_SYMBOLS) {
        strncpy(symbols[sym_count].name, name, 63);
        symbols[sym_count].is_string = 1;
        strncpy(symbols[sym_count].str_val, val, 255);
        symbols[sym_count].is_const = is_const;
        sym_count++;
    }
}

int get_symbol_int(const char *name) {
    if (*name == '@') name++;
    if (is_numeric(name)) return atoi(name);
    Symbol *s = get_symbol_struct(name);
    return s ? s->int_val : 0;
}

/* --- Expression Parser --- */
const char *expr_p;
void skip_ws() { while(*expr_p && isspace((unsigned char)*expr_p)) expr_p++; }
int parse_expr();

int parse_primary() {
    skip_ws();
    if (*expr_p == '(') {
        expr_p++; int v = parse_expr();
        skip_ws(); if (*expr_p == ')') expr_p++;
        return v;
    }
    if (*expr_p == '!') { expr_p++; return !parse_primary(); }
    char buf[64]; int i = 0;
    if (*expr_p == '@') expr_p++; 
    while (isalnum((unsigned char)*expr_p) || *expr_p == '_') buf[i++] = *expr_p++;
    buf[i] = 0;
    return (i == 0) ? 0 : get_symbol_int(buf);
}

int parse_eq() {
    int v = parse_primary();
    skip_ws();
    if (!strncmp(expr_p, "==", 2)) { expr_p += 2; v = (v == parse_primary()); }
    else if (!strncmp(expr_p, "!=", 2)) { expr_p += 2; v = (v != parse_primary()); }
    return v;
}

int parse_and() {
    int v = parse_eq();
    while (1) {
        skip_ws();
        if (!strncmp(expr_p, "&&", 2)) { expr_p += 2; v = v && parse_eq(); }
        else break;
    }
    return v;
}

int parse_expr() {
    int v = parse_and();
    while (1) {
        skip_ws();
        if (!strncmp(expr_p, "||", 2)) { expr_p += 2; v = v || parse_and(); }
        else break;
    }
    return v;
}

/* --- Substitution Engine --- */
void substitute_and_print(const char *line) {
    const char *q = line;
    while (*q) {
        if (*q == '@') {
            q++; char name[64]; int i = 0;
            while (isalnum((unsigned char)*q) || *q == '_') name[i++] = *q++;
            name[i] = 0;
            if (i > 0) {
                Symbol *s = get_symbol_struct(name);
                if (s) {
                    if (s->is_string) printf("%s", s->str_val);
                    else printf("%d", s->int_val);
                } else {
                    printf("@%s", name);
                }
            } else { putchar('@'); }
        } else putchar(*q++);
    }
    putchar('\n');
}

void process_line(char *line, FILE *in, const char *fname, int *lnum);

/* --- Directive Handlers --- */
void handle_include(char *line, const char *curr_file, int curr_line) {
    char inc_file[256];
    if (sscanf(line, "@include \"%255[^\"]\"", inc_file) != 1) return;
    
    FILE *f = fopen(inc_file, "r");
    for (int i = 0; !f && i < inc_path_count; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", inc_paths[i], inc_file);
        f = fopen(path, "r");
    }
    
    if (f) {
        char buf[4096]; int l = 0;
        while (fgets(buf, sizeof(buf), f)) {
            l++;
            process_line(buf, f, inc_file, &l);
        }
        fclose(f);
    } else {
        kit_error(curr_file, curr_line, "Could not find include file");
    }
}

void handle_macro_def(char *line, FILE *in, const char *fname, int *lnum) {
    if (macro_count >= MAX_MACROS) kit_error(fname, *lnum, "Too many macros");
    Macro *m = &macros[macro_count++];
    char *open = strchr(line, '('), *close = strchr(line, ')');
    if (!open || !close) kit_error(fname, *lnum, "Malformed macro definition");

    int name_len = open - (line + 7);
    strncpy(m->name, ltrim(line + 7), name_len);
    m->name[name_len] = 0; trim(m->name);

    char pbuf[128]; strncpy(pbuf, open + 1, close - open - 1); pbuf[close - open - 1] = 0;
    char *tok = strtok(pbuf, ", ");
    while (tok && m->param_count < 8) {
        strcpy(m->params[m->param_count++], tok);
        tok = strtok(NULL, ", ");
    }

    char buf[4096];
    while (fgets(buf, sizeof(buf), in)) {
        (*lnum)++;
        if (strstr(buf, "@endmacro")) break;
        m->body[m->line_count++] = strdup(buf);
    }
}

void handle_macro_call(Macro *m, char *line, const char *fname, int lnum) {
    (void)fname;
    (void)lnum;
    char *open = strchr(line, '('), *close = strrchr(line, ')');
    if (!open || !close) return;
    char args[8][128]; int arg_count = 0;
    char abuf[512]; strncpy(abuf, open + 1, close - open - 1); abuf[close - open - 1] = 0;
    
    char *tok = strtok(abuf, ",");
    while (tok && arg_count < 8) {
        strcpy(args[arg_count++], ltrim(tok));
        trim(args[arg_count-1]);
        if (args[arg_count-1][0] == '"') {
            int len = strlen(args[arg_count-1]);
            if (args[arg_count-1][len-1] == '"') {
                args[arg_count-1][len-1] = 0;
                memmove(args[arg_count-1], args[arg_count-1]+1, len);
            }
        }
        tok = strtok(NULL, ",");
    }

    char m_name[128]; snprintf(m_name, 128, "macro:%s", m->name);
    for (int i = 0; i < m->line_count; i++) {
        char expanded[4096]; strcpy(expanded, m->body[i]);
        for (int a = 0; a < arg_count; a++) {
            char target[64], *pos;
            sprintf(target, "@%s", m->params[a]);
            while ((pos = strstr(expanded, target))) {
                char tmp[4096]; int offset = pos - expanded;
                strncpy(tmp, expanded, offset);
                sprintf(tmp + offset, "%s%s", args[a], pos + strlen(target));
                strcpy(expanded, tmp);
            }
        }
        int ml = i + 1;
        process_line(expanded, NULL, m_name, &ml);
    }
}

void handle_for(char *line, FILE *in, const char *fname, int *lnum) {
    char var[64], line_copy[4096]; strcpy(line_copy, line);
    char *eq = strchr(line_copy, '='), *dot = strstr(line_copy, "..");
    if (!eq || !dot) kit_error(fname, *lnum, "Malformed @for loop");
    
    *eq = 0; *dot = 0;
    char *v_start = ltrim(line_copy + 4);
    strncpy(var, v_start, 63); var[63] = 0; trim(var);
    int start = get_symbol_int(ltrim(eq + 1)), end = get_symbol_int(ltrim(dot + 2));

    char *body[MAX_BLOCK_LINES]; int count = 0, depth = 1;
    char buf[4096];
    while (fgets(buf, sizeof(buf), in)) {
        (*lnum)++;
        if (strstr(buf, "@for")) depth++;
        if (strstr(buf, "@endfor") && --depth == 0) break;
        body[count++] = strdup(buf);
    }
    if (current_emit()) {
        for (int i = start; i <= end; i++) {
            set_symbol_int(var, i, 0);
            for (int j = 0; j < count; j++) {
                char tmp[4096]; strcpy(tmp, body[j]);
                int fake_l = *lnum; // Inside unroll, line numbers are approximate
                process_line(tmp, in, fname, &fake_l);
            }
        }
    }
    for (int j = 0; j < count; j++) free(body[j]);
}

void process_line(char *line, FILE *in, const char *fname, int *lnum) {
    char *e = line + strlen(line) - 1;
    while(e >= line && (*e == '\n' || *e == '\r')) *e-- = '\0';
    char *s = ltrim(line);

    if (trace && *s == '@') fprintf(stderr, "trace: %s:%d: %s\n", fname, *lnum, s);

    if (!*s) { if (current_emit() && !minify) putchar('\n'); return; }

    if (!strncmp(s, "@define", 7)) {
        char n[64], str_v[256];
        if (sscanf(s, "@define %63s \"%255[^\"]\"", n, str_v) == 2) set_symbol_str(n, str_v, 0);
        else {
            char v[64];
            if (sscanf(s, "@define %63s %63s", n, v) == 2) set_symbol_int(n, get_symbol_int(v), 0);
        }
        return;
    }
    if (!strncmp(s, "@include", 8)) { handle_include(s, fname, *lnum); return; }
    if (!strncmp(s, "@if", 3)) {
        char *o = strchr(s, '('), *c = strrchr(s, ')');
        if (!o || !c) kit_error(fname, *lnum, "Malformed @if condition");
        *c = 0; expr_p = o + 1;
        int cond = parse_expr();
        int em = current_emit();
        if (sp >= MAX_STACK) kit_error(fname, *lnum, "Stack overflow");
        stack[sp++] = (Frame){em, cond, em && cond};
        return;
    }
    if (!strncmp(s, "@elif", 5)) {
        if (sp == 0) kit_error(fname, *lnum, "@elif without @if");
        Frame *f = &stack[sp-1]; char *o = strchr(s, '('), *c = strrchr(s, ')');
        if (!o || !c) kit_error(fname, *lnum, "Malformed @elif condition");
        *c = 0; expr_p = o + 1; int cond = parse_expr();
        if (f->branch_taken) f->this_emit = 0;
        else { f->this_emit = f->parent_emit && cond; if (cond) f->branch_taken = 1; }
        return;
    }
    if (!strncmp(s, "@else", 5)) {
        if (sp == 0) kit_error(fname, *lnum, "@else without @if");
        Frame *f = &stack[sp-1]; f->this_emit = (!f->branch_taken && f->parent_emit); f->branch_taken = 1;
        return;
    }
    if (!strncmp(s, "@endif", 6)) {
      if (sp > 0) sp--; else kit_error(fname, *lnum, "@endif without @if"); return;
    }
    if (!strncmp(s, "@macro", 6)) { handle_macro_def(s, in, fname, lnum); return; }
    if (!strncmp(s, "@for", 4)) { handle_for(line, in, fname, lnum); return; }

    for (int i = 0; i < macro_count; i++) {
        if (!strncmp(s, macros[i].name, strlen(macros[i].name)) && current_emit()) {
            handle_macro_call(&macros[i], s, fname, *lnum); return;
        }
    }
    if (current_emit()) {
      substitute_and_print(line);
    }
}

int main(int argc, char **argv) {
    const char *input_path = NULL;
    const char *output_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "-I", 2)) {
            if (inc_path_count < MAX_INC_PATHS) inc_paths[inc_path_count++] = argv[i] + 2;
        } else if (!strcmp(argv[i], "-m")) {
            minify = 1;
        } else if (!strcmp(argv[i], "--trace")) {
            trace = 1;
        } else if (!strcmp(argv[i], "--input")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "missing argument for --input\n");
                return 1;
            }
            input_path = argv[++i];
        } else if (!strcmp(argv[i], "--output")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "missing argument for --output\n");
                return 1;
            }
            output_path = argv[++i];
        } else {
            char *eq = strchr(argv[i], '=');
            if (eq) {
                *eq = 0; char *val = eq + 1;
                if (is_numeric(val)) set_symbol_int(argv[i], atoi(val), 1);
                else set_symbol_str(argv[i], val, 1);
            }
        }
    }

    if (input_path) {
        kit_in = fopen(input_path, "r");
        if (!kit_in) {
            perror(input_path);
            return 1;
        }
        kit_in_name = input_path;
    } else {
        kit_in = stdin;
        kit_in_name = "stdin";
    }

    if (output_path) {
        if (freopen(output_path, "w", stdout) == NULL) {
            perror(output_path);
            if (kit_in != stdin) fclose(kit_in);
            return 1;
        }
    }

    char line[4096]; int lnum = 0;
    while (fgets(line, sizeof(line), kit_in)) {
        lnum++;
        process_line(line, kit_in, kit_in_name, &lnum);
    }
    if (sp != 0) kit_error(kit_in_name, lnum, "Unterminated @if block (missing @endif)");
    if (kit_in != stdin) fclose(kit_in);
    return 0;
}
