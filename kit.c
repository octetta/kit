#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define MAX_STACK 64
#define MAX_SYMBOLS 512
#define MAX_BLOCK_LINES 2048

typedef struct {
    int parent_emit;
    int branch_taken;
    int this_emit;
} Frame;

typedef struct {
    char name[64];
    int val;
    int is_const; // Set by CLI, cannot be overwritten by @define
} Symbol;

Frame stack[MAX_STACK];
int sp = 0;
Symbol symbols[MAX_SYMBOLS];
int sym_count = 0;
int minify = 0;

/* --- Utilities --- */
void trim_trailing(char *s) {
    if (!s) return;
    char *e = s + strlen(s) - 1;
    while(e >= s && (*e == '\n' || *e == '\r' || isspace((unsigned char)*e))) *e-- = '\0';
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

/* --- Core Logic --- */
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

void handle_for(char *line, FILE *in) {
    char var[64], range_start_s[64], range_end_s[64];
    // Supports @for i=0..N or @for i=0..5
    char *eq = strchr(line, '=');
    char *dot = strstr(line, "..");
    if (!eq || !dot) return;

    // Extract variable name
    char *v_start = ltrim(line + 4); 
    int v_len = eq - v_start;
    strncpy(var, v_start, v_len); var[v_len] = 0;
    char *trimmed_var = strtok(var, " \t");

    // Extract start and end
    *dot = 0;
    int start = get_symbol(ltrim(eq + 1));
    int end = get_symbol(ltrim(dot + 2));

    // Buffer the loop body
    char *body[MAX_BLOCK_LINES];
    int count = 0;
    char buf[4096];
    int depth = 1;
    while (fgets(buf, sizeof(buf), in)) {
        if (strstr(buf, "@for")) depth++;
        if (strstr(buf, "@endfor")) {
            depth--;
            if (depth == 0) break;
        }
        if (count < MAX_BLOCK_LINES) body[count++] = strdup(buf);
    }

    if (current_emit()) {
        for (int i = start; i <= end; i++) {
            set_symbol(trimmed_var, i, 0); // Loop vars are not const
            for (int j = 0; j < count; j++) {
                char temp[4096];
                strcpy(temp, body[j]);
                process_line(temp, in);
            }
        }
    }

    for (int j = 0; j < count; j++) free(body[j]);
}

void process_line(char *line, FILE *in) {
    char original[4096];
    strcpy(original, line);
    trim_trailing(line);
    char *s = ltrim(line);

    if (!*s) { 
        if (current_emit() && !minify) putchar('\n'); 
        return; 
    }

    if (!strncmp(s, "@define", 7)) {
        char n[64], v[64];
        if (sscanf(s, "@define %s %s", n, v) == 2) set_symbol(n, get_symbol(v), 0);
        return;
    }
    if (!strncmp(s, "@if", 3)) {
        char *open = strchr(s, '('), *close = strrchr(s, ')');
        if (open && close) {
            *close = 0; expr_p = open + 1;
            int cond = parse_expr();
            int can_emit = current_emit();
            stack[sp++] = (Frame){can_emit, cond, can_emit && cond};
        }
        return;
    }
    if (!strncmp(s, "@elif", 5)) {
        if (sp > 0) {
            Frame *f = &stack[sp-1];
            char *open = strchr(s, '('), *close = strrchr(s, ')');
            if (open && close) {
                *close = 0; expr_p = open + 1;
                int cond = parse_expr();
                if (f->branch_taken) f->this_emit = 0;
                else { f->this_emit = f->parent_emit && cond; if (cond) f->branch_taken = 1; }
            }
        }
        return;
    }
    if (!strncmp(s, "@else", 5)) {
        if (sp > 0) {
            Frame *f = &stack[sp-1];
            f->this_emit = (!f->branch_taken && f->parent_emit);
            f->branch_taken = 1;
        }
        return;
    }
    if (!strncmp(s, "@endif", 6)) { if (sp > 0) sp--; return; }
    if (!strncmp(s, "@for", 4)) { handle_for(line, in); return; }

    if (current_emit()) substitute_and_print(line);
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (eq) {
            *eq = 0;
            set_symbol(argv[i], atoi(eq+1), 1);
        }
    }
    char line[4096];
    while (fgets(line, sizeof(line), stdin)) process_line(line, stdin);
    return 0;
}
