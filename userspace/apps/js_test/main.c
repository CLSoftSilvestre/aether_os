/*
 * js_test — Phase 7.4 integration test (Task 7.4.5)
 *
 * Exit codes for kernel-log diagnosis (pipe relay bug hides stdout):
 *   0   — all tests passed
 *   11  — JS_NewRuntime() returned NULL
 *   12  — JS_NewContext() returned NULL
 *   21  — test 1 failed (string eval)
 *   22  — test 2 failed (arithmetic)
 *   23  — test 3 failed (arrow function)
 *   24  — test 4 failed (closure + let)
 *   25  — test 5 failed (BigInt 2n**53n)
 *
 * Run in AetherOS:  js_test
 * Pass:             sys_exit(0) in kernel log
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quickjs.h"

/* Write directly to fd=1 (pipe) — bypasses stdio buffering */
static void say(const char *msg)
{
    size_t len = 0;
    while (msg[len]) len++;
    /* sys_write: x8=34, x0=fd, x1=buf, x2=len */
    register long x0 asm("x0") = 1;
    register long x1 asm("x1") = (long)msg;
    register long x2 asm("x2") = (long)len;
    register long x8 asm("x8") = 34;
    asm volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
}

int main(void)
{
    say("=== AetherOS Phase 7.4 QuickJS integration test ===\n\n");

    /* ── Runtime + context ───────────────────────────────────────────────── */
    JSRuntime *rt = JS_NewRuntime();
    if (!rt) {
        say("[qjs] FAIL: JS_NewRuntime returned NULL\n");
        return 11;
    }
    JS_SetMemoryLimit(rt, 32 * 1024 * 1024);  /* 32 MB heap cap */
    JS_SetMaxStackSize(rt, 256 * 1024);        /* 256 KB stack cap */

    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) {
        say("[qjs] FAIL: JS_NewContext returned NULL\n");
        JS_FreeRuntime(rt);
        return 12;
    }

    /* ── 1. String expression ────────────────────────────────────────────── */
    {
        const char *src = "\"Hello from QuickJS!\"";
        JSValue v = JS_Eval(ctx, src, strlen(src), "<t1>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(v)) {
            say("[qjs] FAIL test1: string eval exception\n");
            JS_FreeValue(ctx, v);
            JS_FreeContext(ctx);
            JS_FreeRuntime(rt);
            return 21;
        }
        const char *s = JS_ToCString(ctx, v);
        if (!s || strcmp(s, "Hello from QuickJS!") != 0) {
            say("[qjs] FAIL test1: string mismatch\n");
            JS_FreeCString(ctx, s);
            JS_FreeValue(ctx, v);
            JS_FreeContext(ctx);
            JS_FreeRuntime(rt);
            return 21;
        }
        say("[qjs] test1 string OK: \"Hello from QuickJS!\"\n");
        JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, v);
    }

    /* ── 2. Integer arithmetic ───────────────────────────────────────────── */
    {
        const char *src = "6 * 7";
        JSValue v = JS_Eval(ctx, src, strlen(src), "<t2>", JS_EVAL_TYPE_GLOBAL);
        int64_t n = 0;
        if (JS_IsException(v) || JS_ToInt64(ctx, &n, v) != 0 || n != 42) {
            say("[qjs] FAIL test2: arithmetic\n");
            JS_FreeValue(ctx, v);
            JS_FreeContext(ctx);
            JS_FreeRuntime(rt);
            return 22;
        }
        say("[qjs] test2 arithmetic OK: 6*7=42\n");
        JS_FreeValue(ctx, v);
    }

    /* ── 3. Arrow function ───────────────────────────────────────────────── */
    {
        const char *src = "(x => x * x)(9)";
        JSValue v = JS_Eval(ctx, src, strlen(src), "<t3>", JS_EVAL_TYPE_GLOBAL);
        int64_t n = 0;
        if (JS_IsException(v) || JS_ToInt64(ctx, &n, v) != 0 || n != 81) {
            say("[qjs] FAIL test3: arrow function\n");
            JS_FreeValue(ctx, v);
            JS_FreeContext(ctx);
            JS_FreeRuntime(rt);
            return 23;
        }
        say("[qjs] test3 arrow fn OK: (x=>x*x)(9)=81\n");
        JS_FreeValue(ctx, v);
    }

    /* ── 4. Closure + let/block scope ────────────────────────────────────── */
    {
        const char *src =
            "(function(){"
            "let s=0;for(let i=1;i<=10;i++)s+=i;return s;"
            "})()";
        JSValue v = JS_Eval(ctx, src, strlen(src), "<t4>", JS_EVAL_TYPE_GLOBAL);
        int64_t n = 0;
        if (JS_IsException(v) || JS_ToInt64(ctx, &n, v) != 0 || n != 55) {
            say("[qjs] FAIL test4: closure/let\n");
            JS_FreeValue(ctx, v);
            JS_FreeContext(ctx);
            JS_FreeRuntime(rt);
            return 24;
        }
        say("[qjs] test4 closure OK: sum(1..10)=55\n");
        JS_FreeValue(ctx, v);
    }

    /* ── 5. BigInt (ES2020) ──────────────────────────────────────────────── */
    {
        const char *src = "String(2n ** 53n)";
        JSValue v = JS_Eval(ctx, src, strlen(src), "<t5>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(v)) {
            say("[qjs] FAIL test5: BigInt exception\n");
            JS_FreeValue(ctx, v);
            JS_FreeContext(ctx);
            JS_FreeRuntime(rt);
            return 25;
        }
        const char *s = JS_ToCString(ctx, v);
        if (!s || strcmp(s, "9007199254740992") != 0) {
            say("[qjs] FAIL test5: BigInt wrong value\n");
            JS_FreeCString(ctx, s);
            JS_FreeValue(ctx, v);
            JS_FreeContext(ctx);
            JS_FreeRuntime(rt);
            return 25;
        }
        say("[qjs] test5 BigInt OK: 2n**53n=9007199254740992\n");
        JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, v);
    }

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);

    say("\n=== All tests PASSED ===\n");
    return 0;
}
