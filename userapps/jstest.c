/*
 * jstest: runs a set of JavaScript snippets in the QuickJS engine and
 * prints each result next to what it should be. Run it from the shell
 * ("run jstest"); it checks the engine and the C library under it
 * (memory allocator, maths, number formatting, clock).
 */

#include "naqos.h"

#include <stdlib.h>
#include "quickjs.h"

#define HEAP_BYTES (24u * 1024u * 1024u)

static u8 g_heap[HEAP_BYTES];

typedef struct test {
    const char* code;
    const char* expect;   /* NULL: just show the result */
} test_t;

static const test_t g_tests[] = {
    { "1 + 2 * 3", "7" },
    { "0.1 + 0.2", "0.30000000000000004" },
    { "(1e21).toString() + ' ' + (123.456).toFixed(2) + ' ' + (255).toString(16)", "1e+21 123.46 ff" },
    { "Math.sqrt(2)", "1.4142135623730951" },
    { "Math.pow(2, 10) + Math.pow(10, 3) + ' ' + (2 ** 0.5 === Math.SQRT2) + ' ' + 2 ** -2", "2024 true 0.25" },
    { "Math.sin(Math.PI / 6).toFixed(12) + ' ' + Math.cos(0) + ' ' + Math.atan2(1, 1)", "0.500000000000 1 0.7853981633974483" },
    { "Math.exp(1) + ' ' + Math.log(Math.E) + ' ' + Math.log10(1000) + ' ' + Math.cbrt(27)", "2.718281828459045 1 3 3" },
    { "[Math.floor(-1.5), Math.ceil(-1.5), Math.round(2.5), Math.trunc(-7.9), 7 % 3, -7.5 % 2].join()", "-2,-1,3,-7,1,-1.5" },
    { "Math.hypot(3, 4) + Math.tanh(0) + Math.sinh(0) + Math.cosh(0)", "6" },
    { "parseFloat('3.25e2') + parseInt('ff', 16)", "580" },
    { "'Hello'.split('').reverse().join('') + ' ' + 'abc'.toUpperCase().padStart(6, '*')", "olleH ***ABC" },
    { "`${[1, 2, 3].map(x => x * x).filter(x => x > 1).reduce((a, b) => a + b)}`", "13" },
    { "[5, 1, 40, 3, 22].sort((a, b) => a - b).join(' ')", "1 3 5 22 40" },
    { "JSON.stringify({ a: [1, 2, { b: null }], c: 'x', d: 1.5 })", "{\"a\":[1,2,{\"b\":null}],\"c\":\"x\",\"d\":1.5}" },
    { "JSON.parse('{\"n\": [10, 20, 30]}').n[2]", "30" },
    { "'2026-11-17 naq os'.replace(/(\\d+)-(\\d+)-(\\d+)/, '$3/$2/$1').match(/\\w+ os$/i)[0]", "naq os" },
    { "/^[\\p{L}]+$/u.test('h\\u00e9llo') + ' ' + 'a1b22c333'.replace(/\\d+/g, m => m.length)", "true a1b2c3" },
    { "class A { constructor(n) { this.n = n } get twice() { return this.n * 2 } }"
      " class B extends A { twice2() { return super.twice + 1 } } new B(20).twice2()", "41" },
    { "function* g() { let i = 0; while (i < 4) yield i++ } [...g()].join('')", "0123" },
    { "let m = new Map([[1, 'a'], [2, 'b']]); let s = new Set([1, 1, 2, 3]); m.get(2) + s.size", "b3" },
    { "(() => { try { null.x } catch (e) { return e instanceof TypeError } })()", "true" },
    { "(function f(n) { return n < 2 ? n : f(n - 1) + f(n - 2) })(24)", "46368" },
    { "(2n ** 100n).toString()", "1267650600228229401496703205376" },
    { "let out = ''; Promise.resolve(5).then(v => { out = 'promise ' + v }); out", "" },
    { "globalThis.later", "promise ran" },
    { "new Date(Date.UTC(2026, 10, 17, 12, 30)).toISOString()", "2026-11-17T12:30:00.000Z" },
    { "new Date(2026, 0, 1).getDay() + ' ' + (Date.now() > 1.7e12)", "4 true" },
    { "let a = []; for (let i = 0; i < 50000; i++) a.push({ i, s: 'item' + i });"
      " a = a.filter(o => o.i % 1000 == 0); a.length + ' ' + a[49].s", "50 item49000" },
    { "(function deep(n) { return n ? 1 + deep(n - 1) : 0 })(2000)", "2000" },
    { "try { (function forever() { forever() })() } catch (e) { e.constructor.name }", "InternalError" },
    { "new Date().toString()", 0 },
    { "Math.random() < 1", "true" },
};

static void print_text(const char* s) {
    write(1, s, strlen(s));
}

static int same(const char* a, const char* b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }

    return *a == *b;
}

int main(void) {
    naq_heap_init(g_heap, sizeof(g_heap));

    JSRuntime* rt = JS_NewRuntime();

    if (!rt) {
        print_text("jstest: could not create the engine\n");
        return 1;
    }

    JSContext* ctx = JS_NewContext(rt);

    if (!ctx) {
        print_text("jstest: could not create a context\n");
        return 1;
    }

    /* A job queued by a promise runs between scripts, as in a browser. */
    const char* setup = "Promise.resolve().then(() => { globalThis.later = 'promise ran' })";
    JSValue v = JS_Eval(ctx, setup, strlen(setup), "<setup>", JS_EVAL_TYPE_GLOBAL);

    JS_FreeValue(ctx, v);

    u32 passed = 0;
    u32 checked = 0;
    u64 start = get_ticks();

    for (u32 i = 0; i < sizeof(g_tests) / sizeof(g_tests[0]); i++) {
        const test_t* t = &g_tests[i];
        JSContext* job_ctx;

        v = JS_Eval(ctx, t->code, strlen(t->code), "<test>", JS_EVAL_TYPE_GLOBAL);

        if (JS_IsException(v)) {
            JS_FreeValue(ctx, v);
            v = JS_GetException(ctx);
        }

        const char* text = JS_ToCString(ctx, v);
        const char* shown = text ? text : "(no text)";

        if (!t->expect) {
            print_text("      ");
        } else {
            checked++;

            if (same(shown, t->expect)) {
                passed++;
                print_text("ok    ");
            } else {
                print_text("FAIL  ");
            }
        }

        print_text(shown);

        if (t->expect && !same(shown, t->expect)) {
            print_text("   (wanted ");
            print_text(t->expect);
            print_text(")");
        }

        print_text("\n");

        if (text) {
            JS_FreeCString(ctx, text);
        }

        JS_FreeValue(ctx, v);

        while (JS_ExecutePendingJob(rt, &job_ctx) > 0) {
        }
    }

    char line[96];

    snprintf(line, sizeof(line), "%u of %u passed, %u ms, heap in use %u KB\n",
             passed, checked, (u32)(get_ticks() - start),
             (u32)(naq_heap_used() / 1024));
    print_text(line);

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);

    snprintf(line, sizeof(line), "after shutdown: heap in use %u bytes\n",
             (u32)naq_heap_used());
    print_text(line);

    return 0;
}
