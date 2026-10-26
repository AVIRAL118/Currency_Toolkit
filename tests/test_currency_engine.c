#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

/* Rename main from currency_engine.c so there is no symbol conflict */
#define main currency_engine_main
#include "currency_engine.c"
#undef main

/* --- Custom Test Macros --- */
#define ASSERT_TRUE(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "\n❌ FAILED at %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        exit(1); \
    } \
} while (0)

#define ASSERT_STR_EQ(a, b) ASSERT_TRUE(strcmp((a), (b)) == 0)
#define ASSERT_DOUBLE_EQ(a, b) ASSERT_TRUE(fabs((a) - (b)) < 1e-9)

static void run_tests(void)
{
    printf("Running currency engine unit tests...\n");

    /* 0. Reset global state if the engine tracks counts */
    /* Uncomment the lines below if your engine has these variables: */
    /* var_count = 0; */
    /* rate_count = 0; */

    /* 1. parse_currency_token(): "USD10.50" should split into symbol="USD" and value=10.50 */
    {
        char symbol[MAX_SYMBOL];
        double value = 0.0;
        int res = parse_currency_token("USD10.50", symbol, sizeof(symbol), &value);
        ASSERT_TRUE(res == 1);
        ASSERT_STR_EQ(symbol, "USD");
        ASSERT_DOUBLE_EQ(value, 10.50);
    }

    /* 2. parse_currency_token(): unsupported code like "XYZ10" should return 0 (rejected) */
    {
        char symbol[MAX_SYMBOL];
        double value = 0.0;
        int res = parse_currency_token("XYZ10", symbol, sizeof(symbol), &value);
        ASSERT_TRUE(res == 0);
    }

    /* 3. valid_symbol(): confirm "INR" returns 1 and "RUPEE" returns 0 */
    {
        ASSERT_TRUE(valid_symbol("INR") == 1);
        ASSERT_TRUE(valid_symbol("RUPEE") == 0);
    }

    /* 4. valid_name(): confirm "_price1" is valid and "1price" is invalid */
    {
        ASSERT_TRUE(valid_name("_price1") == 1);
        ASSERT_TRUE(valid_name("1price") == 0);
    }

    /* 5. Tokenizer (next_token): feeding "12.5 + x" yields TOK_NUMBER, TOK_PLUS, TOK_NAME */
    {
        Parser p;
        p.p = "12.5 + x";
        p.token[0] = '\0';

        next_token(&p);
        ASSERT_TRUE(p.kind == TOK_NUMBER);
        ASSERT_STR_EQ(p.token, "12.5");

        next_token(&p);
        ASSERT_TRUE(p.kind == TOK_PLUS);

        next_token(&p);
        ASSERT_TRUE(p.kind == TOK_NAME);
        ASSERT_STR_EQ(p.token, "x");
    }

    /* 6. evaluate(): "2 + 3 * 4" should evaluate to 14 (operator precedence) */
    {
        Value out;
        int res = evaluate("2 + 3 * 4", &out);
        ASSERT_TRUE(res == 1);
        ASSERT_DOUBLE_EQ(out.value, 14.0);
        ASSERT_TRUE(out.symbol[0] == '\0');
    }

    /* 7. evaluate(): "(2 + 3) * 4" should evaluate to 20 (parentheses override precedence) */
    {
        Value out;
        int res = evaluate("(2 + 3) * 4", &out);
        ASSERT_TRUE(res == 1);
        ASSERT_DOUBLE_EQ(out.value, 20.0);
        ASSERT_TRUE(out.symbol[0] == '\0');
    }

    /* 8. evaluate() with currency: after storing x=USD10, "x + 5" yields value 15 with symbol "USD" */
    {
        int idx = get_or_create_var("x");
        vars[idx].value.value = 10.0;
        snprintf(vars[idx].value.symbol, sizeof(vars[idx].value.symbol), "USD");

        Value out;
        int res = evaluate("x + 5", &out);
        ASSERT_TRUE(res == 1);
        ASSERT_DOUBLE_EQ(out.value, 15.0);
        ASSERT_STR_EQ(out.symbol, "USD");
    }

    /* 9. evaluate(): adding two different currency symbols sets an ERROR in error_message */
    {
        int idx1 = get_or_create_var("currA");
        vars[idx1].value.value = 10.0;
        snprintf(vars[idx1].value.symbol, sizeof(vars[idx1].value.symbol), "USD");

        int idx2 = get_or_create_var("currB");
        vars[idx2].value.value = 5.0;
        snprintf(vars[idx2].value.symbol, sizeof(vars[idx2].value.symbol), "EUR");

        error_message[0] = '\0'; // Clear before test
        Value out;
        int res = evaluate("currA + currB", &out);
        ASSERT_TRUE(res == 0);
        ASSERT_TRUE(error_message[0] != '\0');
    }

    /* 10. evaluate(): multiplying two currency values sets an ERROR in error_message */
    {
        int idx1 = get_or_create_var("m1");
        vars[idx1].value.value = 10.0;
        snprintf(vars[idx1].value.symbol, sizeof(vars[idx1].value.symbol), "USD");

        int idx2 = get_or_create_var("m2");
        vars[idx2].value.value = 2.0;
        snprintf(vars[idx2].value.symbol, sizeof(vars[idx2].value.symbol), "USD");

        error_message[0] = '\0'; // Clear before test
        Value out;
        int res = evaluate("m1 * m2", &out);
        ASSERT_TRUE(res == 0);
        ASSERT_TRUE(error_message[0] != '\0');
    }

    /* 11. evaluate(): division by zero sets an ERROR in error_message, avoiding crash/inf */
    {
        error_message[0] = '\0'; // Clear before test
        Value out;
        int res = evaluate("100 / 0", &out);
        ASSERT_TRUE(res == 0);
        ASSERT_TRUE(error_message[0] != '\0');
    }

    /* 12. store_rate()/find_rate(): store USD->INR at 83, find it; unregistered pair returns -1 */
    {
        int stored = store_rate("USD", "INR", 83.0);
        ASSERT_TRUE(stored == 1);

        int idx = find_rate("USD", "INR");
        ASSERT_TRUE(idx >= 0);
        ASSERT_DOUBLE_EQ(rates[idx].rate, 83.0);

        int missing = find_rate("EUR", "JPY");
        ASSERT_TRUE(missing == -1);
    }

    printf("✅ All unit tests passed successfully!\n");
}

int main(void)
{
    run_tests();
    return 0;
}
/*Make sure you compile this with -lm (link math library) so that fabs() from <math.h> works:
gcc -std=c11 -Wall -Wextra -pedantic engine_tests.c -o engine_tests -lm
./engine_tests
*/