
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>

#define MAX_LINE 4096
#define MAX_NAME 64
#define MAX_SYMBOL 16
#define MAX_VARS 256
#define MAX_RATES 256
#define MAX_ERROR 160

typedef struct {
    double value;
    char symbol[MAX_SYMBOL]; /* Empty means NONE */
} Value;

typedef struct {
    char name[MAX_NAME];
    Value value;
    int used;
} Variable;

typedef struct {
    char from[MAX_SYMBOL];
    char to[MAX_SYMBOL];
    double rate;
    int used;
} Rate;

static Variable vars[MAX_VARS];
static Rate rates[MAX_RATES];

static char error_message[MAX_ERROR];

/* All protocol responses go through this function. */
static void respond(const char *message)
{
    puts(message);
    fflush(stdout);
}

static void set_error(const char *message)
{
    snprintf(error_message, sizeof(error_message), "%s", message);
}

static int valid_number(double x)
{
    return isfinite(x);
}

static int valid_name(const char *s)
{
    size_t i, n = strlen(s);

    if (n == 0 || n >= MAX_NAME)
        return 0;

    if (!(isalpha((unsigned char)s[0]) || s[0] == '_'))
        return 0;

    for (i = 1; i < n; i++) {
        if (!(isalnum((unsigned char)s[i]) || s[i] == '_'))
            return 0;
    }

    return 1;
}

/* Only these ISO-style currency codes are accepted by the language. */
static int valid_symbol(const char *s)
{
    static const char *const currency_codes[] = {
        "USD", "EUR", "GBP", "CNY", "JPY", "RUB", "INR",
        "KRW", "SAR", "AED", "BRL", "ZAR", "CAD", "AUD",
        "CHF", "SGD", "MXN", "TRY", "NZD"
    };
    size_t i;

    for (i = 0; i < sizeof(currency_codes) / sizeof(currency_codes[0]); i++) {
        if (strcmp(s, currency_codes[i]) == 0)
            return 1;
    }

    return 0;
}

static int same_symbol(const Value *a, const Value *b)
{
    return strcmp(a->symbol, b->symbol) == 0;
}

static const char *symbol_name(const Value *v)
{
    return v->symbol[0] ? v->symbol : "NONE";
}

static int find_var(const char *name)
{
    int i;

    for (i = 0; i < MAX_VARS; i++) {
        if (vars[i].used && strcmp(vars[i].name, name) == 0)
            return i;
    }

    return -1;
}

static int get_or_create_var(const char *name)
{
    int i = find_var(name);

    if (i >= 0)
        return i;

    for (i = 0; i < MAX_VARS; i++) {
        if (!vars[i].used) {
            snprintf(vars[i].name, sizeof(vars[i].name), "%s", name);
            vars[i].value.value = 0.0;
            vars[i].value.symbol[0] = '\0';
            vars[i].used = 1;
            return i;
        }
    }

    set_error("symbol table full");
    return -1;
}

static int find_rate(const char *from, const char *to)
{
    int i;

    for (i = 0; i < MAX_RATES; i++) {
        if (rates[i].used &&
            strcmp(rates[i].from, from) == 0 &&
            strcmp(rates[i].to, to) == 0)
            return i;
    }

    return -1;
}

static int store_rate(const char *from, const char *to, double rate)
{
    int i = find_rate(from, to);

    if (i < 0) {
        for (i = 0; i < MAX_RATES; i++) {
            if (!rates[i].used)
                break;
        }

        if (i == MAX_RATES) {
            set_error("rate table full");
            return 0;
        }
    }

    snprintf(rates[i].from, sizeof(rates[i].from), "%s", from);
    snprintf(rates[i].to, sizeof(rates[i].to), "%s", to);
    rates[i].rate = rate;
    rates[i].used = 1;

    return 1;
}

/* ---------------- Expression parser ---------------- */

typedef struct {
    const char *p;
    char token[MAX_NAME];
    int kind;
} Parser;

enum {
    TOK_END,
    TOK_NUMBER,
    TOK_NAME,
    TOK_PLUS,
    TOK_MINUS,
    TOK_MUL,
    TOK_DIV,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_BAD
};

static void next_token(Parser *p)
{
    const char *start;
    size_t n;

    while (isspace((unsigned char)*p->p))
        p->p++;

    p->token[0] = '\0';

    if (*p->p == '\0') {
        p->kind = TOK_END;
        return;
    }

    switch (*p->p) {
        case '+': p->kind = TOK_PLUS;   p->p++; return;
        case '-': p->kind = TOK_MINUS;  p->p++; return;
        case '*': p->kind = TOK_MUL;    p->p++; return;
        case '/': p->kind = TOK_DIV;    p->p++; return;
        case '(': p->kind = TOK_LPAREN; p->p++; return;
        case ')': p->kind = TOK_RPAREN; p->p++; return;
        default: break;
    }

    if (isdigit((unsigned char)*p->p) || *p->p == '.') {
        char *end;
        double number;

        errno = 0;
        number = strtod(p->p, &end);

        if (end == p->p || errno == ERANGE || !isfinite(number)) {
            p->kind = TOK_BAD;
            return;
        }

        n = (size_t)(end - p->p);
        if (n >= sizeof(p->token)) {
            p->kind = TOK_BAD;
            return;
        }

        memcpy(p->token, p->p, n);
        p->token[n] = '\0';
        p->p = end;
        p->kind = TOK_NUMBER;
        return;
    }

    if (isalpha((unsigned char)*p->p) || *p->p == '_') {
        start = p->p++;

        while (isalnum((unsigned char)*p->p) || *p->p == '_')
            p->p++;

        n = (size_t)(p->p - start);

        if (n >= sizeof(p->token)) {
            p->kind = TOK_BAD;
            return;
        }

        memcpy(p->token, start, n);
        p->token[n] = '\0';
        p->kind = TOK_NAME;
        return;
    }

    p->kind = TOK_BAD;
}

static Value parse_expression(Parser *p);

static Value parse_primary(Parser *p)
{
    Value result = {0.0, ""};

    if (p->kind == TOK_NUMBER) {
        result.value = strtod(p->token, NULL);
        next_token(p);
        return result;
    }

    if (p->kind == TOK_NAME) {
        int i = find_var(p->token);

        if (i < 0) {
            set_error("undefined variable");
            return result;
        }

        result = vars[i].value;
        next_token(p);
        return result;
    }

    if (p->kind == TOK_LPAREN) {
        next_token(p);
        result = parse_expression(p);

        if (error_message[0])
            return result;

        if (p->kind != TOK_RPAREN) {
            set_error("missing closing parenthesis");
            return result;
        }

        next_token(p);
        return result;
    }

    set_error("expected number, variable, or parenthesis");
    return result;
}

static Value parse_unary(Parser *p)
{
    Value result;

    if (p->kind == TOK_PLUS) {
        next_token(p);
        return parse_unary(p);
    }

    if (p->kind == TOK_MINUS) {
        next_token(p);
        result = parse_unary(p);
        result.value = -result.value;
        return result;
    }

    return parse_primary(p);
}

static Value parse_term(Parser *p)
{
    Value left = parse_unary(p);

    while (!error_message[0] &&
           (p->kind == TOK_MUL || p->kind == TOK_DIV)) {
        int op = p->kind;
        Value right;

        next_token(p);
        right = parse_unary(p);

        if (error_message[0])
            return left;

        if (op == TOK_MUL) {
            if (left.symbol[0] && right.symbol[0]) {
                set_error("cannot multiply two currencies");
                return left;
            }

            left.value *= right.value;

            if (!left.symbol[0] && right.symbol[0])
                snprintf(left.symbol, sizeof(left.symbol),
                         "%s", right.symbol);
        } else {
            if (right.value == 0.0) {
                set_error("division by zero");
                return left;
            }

            if (left.symbol[0] && right.symbol[0]) {
                if (!same_symbol(&left, &right)) {
                    set_error("currency mismatch in division");
                    return left;
                }

                left.value /= right.value;
                left.symbol[0] = '\0';
            } else {
                left.value /= right.value;
            }
        }

        if (!valid_number(left.value)) {
            set_error("numeric overflow");
            return left;
        }
    }

    return left;
}

static Value parse_expression(Parser *p)
{
    Value left = parse_term(p);

    while (!error_message[0] &&
           (p->kind == TOK_PLUS || p->kind == TOK_MINUS)) {
        int op = p->kind;
        Value right;

        next_token(p);
        right = parse_term(p);

        if (error_message[0])
            return left;

        if (left.symbol[0] && right.symbol[0] &&
            !same_symbol(&left, &right)) {
            set_error("currency mismatch in addition/subtraction");
            return left;
        }

        if (op == TOK_PLUS)
            left.value += right.value;
        else
            left.value -= right.value;

        if (!left.symbol[0] && right.symbol[0])
            snprintf(left.symbol, sizeof(left.symbol),
                     "%s", right.symbol);

        if (!valid_number(left.value)) {
            set_error("numeric overflow");
            return left;
        }
    }

    return left;
}

static int evaluate(const char *expression, Value *out)
{
    Parser p;

    error_message[0] = '\0';
    p.p = expression;
    p.token[0] = '\0';

    next_token(&p);
    *out = parse_expression(&p);

    if (error_message[0])
        return 0;

    if (p.kind != TOK_END) {
        set_error("unexpected token in expression");
        return 0;
    }

    return 1;
}

/* ---------------- Command handling ---------------- */

static int parse_plain_number(const char *s, double *out)
{
    char *end;

    errno = 0;
    *out = strtod(s, &end);

    if (s == end || *end != '\0' || errno == ERANGE ||
        !isfinite(*out))
        return 0;

    return 1;
}

/*
 * Split a token such as INR12.50 or USD-12.50
 * into its ISO currency code and numeric value.
 * Currency symbols and unknown codes are rejected.
 */
static int parse_currency_token(const char *token,
                                char *symbol, size_t symbol_size,
                                double *value)
{
    const char *p;
    char *end;
    size_t symbol_len;

    p = token;

    while (*p && !(isdigit((unsigned char)*p) || *p == '.' ||
          ((*p == '+' || *p == '-') &&
           (isdigit((unsigned char)p[1]) || p[1] == '.'))))
        p++;

    if (p == token || *p == '\0')
        return 0;

    symbol_len = (size_t)(p - token);

    if (symbol_len >= symbol_size)
        return 0;

    memcpy(symbol, token, symbol_len);
    symbol[symbol_len] = '\0';

    errno = 0;
    *value = strtod(p, &end);

    if (end == p || *end != '\0' || errno == ERANGE ||
        !isfinite(*value))
        return 0;

    return valid_symbol(symbol);
}

static void command_set(char *args, int currency)
{
    char name[MAX_NAME];
    char token[MAX_LINE];
    char symbol[MAX_SYMBOL] = "";
    double value;
    int index;
    Value v;

    error_message[0] = '\0';

    if (sscanf(args, "%63s %4095s", name, token) != 2 ||
        !valid_name(name)) {
        respond("ERR invalid SET syntax");
        return;
    }

    if (currency) {
        if (!parse_currency_token(token, symbol, sizeof(symbol), &value)) {
            respond("ERR invalid currency value");
            return;
        }
    } else {
        if (!parse_plain_number(token, &value)) {
            respond("ERR invalid numeric value");
            return;
        }
    }

    index = get_or_create_var(name);

    if (index < 0) {
        char response[MAX_ERROR + 8];
        snprintf(response, sizeof(response), "ERR %s", error_message);
        respond(response);
        return;
    }

    v.value = value;
    snprintf(v.symbol, sizeof(v.symbol), "%s", symbol);
    vars[index].value = v;

    {
        char response[MAX_LINE];
        snprintf(response, sizeof(response), "OK %.15g %s",
                 value, symbol[0] ? symbol : "NONE");
        respond(response);
    }
}

static void command_eval(const char *expression)
{
    Value result;
    char response[MAX_LINE];

    if (!evaluate(expression, &result)) {
        snprintf(response, sizeof(response), "ERR %s", error_message);
        respond(response);
        return;
    }

    snprintf(response, sizeof(response), "OK %.15g %s",
             result.value, symbol_name(&result));
    respond(response);
}

static void command_vars(void)
{
    char response[MAX_LINE] = "VARLIST";
    size_t used = strlen(response);
    int i;

    for (i = 0; i < MAX_VARS; i++) {
        char item[128];
        int n;

        if (!vars[i].used)
            continue;

        if (vars[i].value.symbol[0]) {
            n = snprintf(item, sizeof(item), " %s=%s%.15g",
                         vars[i].name,
                         vars[i].value.symbol,
                         vars[i].value.value);
        } else {
            n = snprintf(item, sizeof(item), " %s=%.15g",
                         vars[i].name,
                         vars[i].value.value);
        }

        if (n < 0 || (size_t)n >= sizeof(item) ||
            used + (size_t)n >= sizeof(response)) {
            break;
        }

        memcpy(response + used, item, (size_t)n + 1);
        used += (size_t)n;
    }

    respond(response);
}

static void command_rate(char *args)
{
    char from[MAX_SYMBOL], to[MAX_SYMBOL], number[128];
    double rate;

    error_message[0] = '\0';

    if (sscanf(args, "%15s %15s %127s", from, to, number) != 3 ||
        !valid_symbol(from) || !valid_symbol(to)) {
        respond("ERR invalid RATE syntax");
        return;
    }

    if (!parse_plain_number(number, &rate) || rate <= 0.0) {
        respond("ERR rate must be a positive finite number");
        return;
    }

    if (!store_rate(from, to, rate)) {
        char response[MAX_ERROR + 8];
        snprintf(response, sizeof(response), "ERR %s", error_message);
        respond(response);
        return;
    }

    respond("OK RATE");
}

static void command_convert(char *args)
{
    char name[MAX_NAME], target[MAX_SYMBOL];
    int index, rate_index;
    Value v;
    char response[MAX_LINE];

    if (sscanf(args, "%63s %15s", name, target) != 2 ||
        !valid_name(name) || !valid_symbol(target)) {
        respond("ERR invalid CONVERT syntax");
        return;
    }

    index = find_var(name);

    if (index < 0) {
        respond("ERR undefined variable");
        return;
    }

    v = vars[index].value;

    if (!v.symbol[0]) {
        respond("ERR cannot convert a plain number");
        return;
    }

    if (strcmp(v.symbol, target) == 0) {
        snprintf(response, sizeof(response), "OK %.15g %s",
                 v.value, target);
        respond(response);
        return;
    }

    rate_index = find_rate(v.symbol, target);

    if (rate_index < 0) {
        respond("ERR conversion rate not found");
        return;
    }

    v.value *= rates[rate_index].rate;

    if (!valid_number(v.value)) {
        respond("ERR numeric overflow");
        return;
    }

    snprintf(v.symbol, sizeof(v.symbol), "%s", target);

    /* Conversion returns the result; it does not modify the variable. */
    snprintf(response, sizeof(response), "OK %.15g %s",
             v.value, v.symbol);
    respond(response);
}

/* ---------------- Persistent stdin loop ---------------- */

int main(void)
{
    char line[MAX_LINE];

    while (fgets(line, sizeof(line), stdin) != NULL) {
        char *command;
        char *args;
        size_t len = strlen(line);

        /* Drain overlong lines so their remainder isn't a new command. */
        if (len > 0 && line[len - 1] != '\n' && !feof(stdin)) {
            int ch;
            while ((ch = getchar()) != '\n' && ch != EOF) {}
            respond("ERR command line too long");
            continue;
        }

        while (len > 0 &&
               (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        command = line;

        while (isspace((unsigned char)*command))
            command++;

        if (*command == '\0') {
            respond("ERR empty command");
            continue;
        }

        args = command;

        while (*args && !isspace((unsigned char)*args))
            args++;

        if (*args) {
            *args++ = '\0';
            while (isspace((unsigned char)*args))
                args++;
        }

        if (strcmp(command, "QUIT") == 0) {
            if (*args)
                respond("ERR QUIT takes no arguments");
            else
            {
                respond("OK BYE");
                break;
            }
        } else if (strcmp(command, "SET") == 0) {
            command_set(args, 1);
        } else if (strcmp(command, "SETN") == 0) {
            command_set(args, 0);
        } else if (strcmp(command, "EVAL") == 0) {
            if (*args)
                command_eval(args);
            else
                respond("ERR missing expression");
        } else if (strcmp(command, "VARS") == 0) {
            if (*args)
                respond("ERR VARS takes no arguments");
            else
                command_vars();
        } else if (strcmp(command, "RATE") == 0) {
            command_rate(args);
        } else if (strcmp(command, "CONVERT") == 0) {
            command_convert(args);
        } else {
            respond("ERR unknown command");
        }
    }

    return 0;
}
