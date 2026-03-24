#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define MAX_STACK 64
#define MAX_SYMBOLS 512
#define MAX_MACROS 64
#define MAX_BLOCK_LINES 1024

typedef struct { int parent_emit, branch_taken, this_emit; } Frame;
typedef struct { char name[64]; int val, is_const; } Symbol;
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
int minify = 0;

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

void set_symbol(const char *name, int val, int is_const) {
    if (*name == '@') name++;
    for(int i=0; i<sym_count; i++) {
        if(!strcmp(symbols[i].name, name)) {
            if (!symbols[i].is_const || is_const) {
                symbols[i].val = val;
                if (is_const) symbols[i].is_const = 1;
            }
            return;
        }
    }
    if (sym_count < MAX_SYMBOLS) {
        strncpy(symbols[sym_count].name, name, 63);
        symbols[sym_count].val = val;
        symbols[sym_count].is_const = is_const;
        sym_count++;
    }
}

int get_symbol(const char *name) {
    if (*name == '@') name++;
    if (isdigit(*name) || (*name == '-' && isdigit(name[1]))) return atoi(name);
    for(int i=0; i<sym_count; i++) {
        if(!strcmp(symbols[i].name, name)) return symbols[i].val;
    }
    return 0;
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
    return (i == 0) ? 0 : get_symbol(buf);
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
            if (i > 0) printf("%d", get_symbol(name));
            else putchar('@');
        } else putchar(*q++);
    }
    putchar('\n');
}

void process_line(char *line, FILE *in);

/* --- Directive Handlers --- */
void handle_macro_def(char *line, FILE *in) {
    if (macro_count >= MAX_MACROS) return;
    Macro *m = &macros[macro_count++];
    char *open = strchr(line, '('), *close = strchr(line, ')');
    if (!open || !close) return;

    // Name
    int name_len = open - (line + 7);
    strncpy(m->name, ltrim(line + 7), name_len);
    m->name[name_len] = 0; trim(m->name);

    // Params
    char pbuf[128]; strncpy(pbuf, open + 1, close - open - 1); pbuf[close - open - 1] = 0;
    char *tok = strtok(pbuf, ", ");
    while (tok && m->param_count < 8) {
        strcpy(m->params[m->param_count++], tok);
        tok = strtok(NULL, ", ");
    }

    // Body
    char buf[4096];
    while (fgets(buf, sizeof(buf), in)) {
        if (strstr(buf, "@endmacro")) break;
        m->body[m->line_count++] = strdup(buf);
    }
}

void handle_macro_call(Macro *m, char *line) {
    char *open = strchr(line, '('), *close = strrchr(line, ')');
    if (!open || !close) return;
    char args[8][128]; int arg_count = 0;
    char abuf[512]; strncpy(abuf, open + 1, close - open - 1); abuf[close - open - 1] = 0;
    
    char *tok = strtok(abuf, ",");
    while (tok && arg_count < 8) {
        strcpy(args[arg_count++], ltrim(tok));
        trim(args[arg_count-1]);
        if (args[arg_count-1][0] == '"') { // Strip quotes if present
            int len = strlen(args[arg_count-1]);
            if (args[arg_count-1][len-1] == '"') {
                args[arg_count-1][len-1] = 0;
                memmove(args[arg_count-1], args[arg_count-1]+1, len);
            }
        }
        tok = strtok(NULL, ",");
    }

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
        process_line(expanded, NULL);
    }
}

void handle_for(char *line, FILE *in) {
    char var[64];
    char *eq = strchr(line, '=');
    char *dot = strstr(line, "..");
    if (!eq || !dot) return;
    int vlen = eq - (ltrim(line) + 4);
    strncpy(var, ltrim(line) + 4, vlen); var[vlen] = 0; trim(var);
    int start = get_symbol(ltrim(eq + 1)), end = get_symbol(ltrim(dot + 2));

    char *body[MAX_BLOCK_LINES]; int count = 0, depth = 1;
    char buf[4096];
    while (fgets(buf, sizeof(buf), in)) {
        if (strstr(buf, "@for")) depth++;
        if (strstr(buf, "@endfor") && --depth == 0) break;
        body[count++] = strdup(buf);
    }
    if (current_emit()) {
        for (int i = start; i <= end; i++) {
            set_symbol(var, i, 0);
            for (int j = 0; j < count; j++) {
                char tmp[4096]; strcpy(tmp, body[j]);
                process_line(tmp, in);
            }
        }
    }
    for (int j = 0; j < count; j++) free(body[j]);
}

void process_line(char *line, FILE *in) {
    char *s = ltrim(line);
    if (!*s) { if (current_emit() && !minify) putchar('\n'); return; }
    if (!strncmp(s, "@define", 7)) {
        char n[64], v[64];
        if (sscanf(s, "@define %s %s", n, v) == 2) set_symbol(n, get_symbol(v), 0);
        return;
    }
    if (!strncmp(s, "@if", 3)) {
        char *o = strchr(s, '('), *c = strrchr(s, ')');
        if (o && c) { *c = 0; expr_p = o + 1; int cond = parse_expr(); int em = current_emit();
            stack[sp++] = (Frame){em, cond, em && cond}; }
        return;
    }
    if (!strncmp(s, "@elif", 5)) {
        if (sp > 0) { Frame *f = &stack[sp-1]; char *o = strchr(s, '('), *c = strrchr(s, ')');
            if (o && c) { *c = 0; expr_p = o + 1; int cond = parse_expr();
                if (f->branch_taken) f->this_emit = 0;
                else { f->this_emit = f->parent_emit && cond; if (cond) f->branch_taken = 1; } } }
        return;
    }
    if (!strncmp(s, "@else", 5)) { if (sp > 0) { Frame *f = &stack[sp-1]; f->this_emit = (!f->branch_taken && f->parent_emit); f->branch_taken = 1; } return; }
    if (!strncmp(s, "@endif", 6)) { if (sp > 0) sp--; return; }
    if (!strncmp(s, "@macro", 6)) { handle_macro_def(s, in); return; }
    if (!strncmp(s, "@for", 4)) { handle_for(line, in); return; }

    for (int i = 0; i < macro_count; i++) {
        if (!strncmp(s, macros[i].name, strlen(macros[i].name)) && current_emit()) {
            handle_macro_call(&macros[i], s); return;
        }
    }
    if (current_emit()) substitute_and_print(line);
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (eq) { *eq = 0; set_symbol(argv[i], atoi(eq+1), 1); }
    }
    char line[4096];
    while (fgets(line, sizeof(line), stdin)) process_line(line, stdin);
    return 0;
}
