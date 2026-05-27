/*
 * test_nanojs.cpp — Headless smoke tests for the NanoJS runtime.
 *
 * Tests the JS engine, error handling, promises, and the nanogui module
 * registration entirely without opening a GL window.  Run as:
 *
 *   ./test_nanojs
 *
 * Exit code 0 = all tests passed, non-zero = failures.
 */

#include "nanojs.h"

#include <cstdio>
#include <cstring>
#include <string>

// ---------------------------------------------------------------------------
// Minimal test harness
// ---------------------------------------------------------------------------

static int g_pass = 0;
static int g_fail = 0;

static void check(bool cond, const char* label) {
    if (cond) {
        printf("  PASS  %s\n", label);
        ++g_pass;
    } else {
        printf("  FAIL  %s\n", label);
        ++g_fail;
    }
}

// ---------------------------------------------------------------------------
// Helpers: run a snippet and capture stdout-like result via a JS global
// ---------------------------------------------------------------------------

// Evaluates `expr` and stores the result string in `out`.
// We inject a tiny __result__ global so we can read back values without
// needing a full console.log implementation.
static bool eval_expr(nanogui::NanoJS& js, const std::string& expr,
                      std::string& out) {
    std::string src = "__result__ = String(" + expr + ");";
    if (!js.eval_string(src, "<test>"))
        return false;

    // Read __result__ back via a second eval that sets another marker
    // Actually simpler: use JS_GetGlobalObject + JS_GetPropertyStr.
    // But NanoJS doesn't expose that directly - so we round-trip through
    // another assignment to a known sentinel and compare.
    // Easiest: just eval to a boolean comparison.
    out = "?"; // placeholder - used only where we don't need the value
    return true;
}

// Evaluates `expr` (expected to be a JS expression) and returns whether
// the JS engine reports it equal to `expected` string.
static bool eval_eq(nanogui::NanoJS& js, const std::string& expr,
                    const std::string& expected) {
    // Build:  String(<expr>) === "<expected>"
    std::string cmp = "String(" + expr + ") === \"" + expected + "\"";
    // We can't read back a bool easily without a raw ctx, so we abuse
    // the exception path: throw if false, succeed if true.
    std::string src = "if (!(" + cmp + ")) throw new Error('mismatch');";
    return js.eval_string(src, "<test>");
}

// ---------------------------------------------------------------------------
// Test suites
// ---------------------------------------------------------------------------

static void test_basic_eval(nanogui::NanoJS& js) {
    printf("\n[basic eval]\n");

    check(js.eval_string("1 + 1", "<test>"),           "1+1 evaluates");
    check(eval_eq(js, "2 + 2", "4"),                   "arithmetic: 2+2==4");
    check(eval_eq(js, "\"hello\" + \" world\"", "hello world"),
                                                        "string concat");
    check(eval_eq(js, "typeof 42", "number"),           "typeof number");
    check(eval_eq(js, "typeof 'hi'", "string"),         "typeof string");
    check(eval_eq(js, "[1,2,3].length", "3"),           "array length");
    check(eval_eq(js, "Math.max(3,7)", "7"),            "Math.max");
    check(eval_eq(js, "(x => x*x)(5)", "25"),           "arrow function IIFE");
}

static void test_multiline_and_state(nanogui::NanoJS& js) {
    printf("\n[state persistence across evals]\n");

    // Variable set in one eval should be visible in the next
    check(js.eval_string("var counter = 0;", "<test>"),   "declare counter");
    check(js.eval_string("counter += 10;",   "<test>"),   "increment counter");
    check(eval_eq(js, "counter", "10"),                   "counter == 10");

    // Function defined in one eval callable in next
    check(js.eval_string("function greet(n){ return 'hi ' + n; }", "<test>"),
          "define function");
    check(eval_eq(js, "greet('world')", "hi world"),      "call function");
}

static void test_error_handling(nanogui::NanoJS& js) {
    printf("\n[error handling]\n");

    // Syntax error
    bool ok = js.eval_string("this is not valid JS !!!!", "<test>");
    check(!ok, "syntax error returns false");

    // Runtime error (reference to undefined variable)
    ok = js.eval_string("undefinedVariable.foo()", "<test>");
    check(!ok, "runtime error returns false");

    // Runtime continues after errors
    check(eval_eq(js, "1 + 1", "2"), "runtime alive after errors");

    // Explicit throw
    ok = js.eval_string("throw new Error('deliberate');", "<test>");
    check(!ok, "explicit throw returns false");

    // try/catch inside JS should still return true
    check(js.eval_string("try { throw 42; } catch(e) {}", "<test>"),
          "caught throw returns true");
}

static void test_promises_and_pump(nanogui::NanoJS& js) {
    printf("\n[promises and pump]\n");

    // Set up a flag that a promise continuation will set
    check(js.eval_string("var promiseFired = false;", "<test>"),
          "init promiseFired");

    check(js.eval_string(
        "Promise.resolve().then(() => { promiseFired = true; });",
        "<test>"), "schedule promise");

    // Before pump: microtask hasn't run yet
    // (QuickJS runs microtasks lazily — pump drains them)
    js.pump();

    check(eval_eq(js, "promiseFired", "true"), "promise ran after pump");

    // Chained promises
    check(js.eval_string(
        "var chain = 0;"
        "Promise.resolve(1)"
        "  .then(v => v + 1)"
        "  .then(v => v + 1)"
        "  .then(v => { chain = v; });",
        "<test>"), "schedule chained promise");
    js.pump();
    check(eval_eq(js, "chain", "3"), "chained promise resolves to 3");

    // async/await
    check(js.eval_string(
        "var asyncResult = 0;"
        "(async function() {"
        "  const v = await Promise.resolve(99);"
        "  asyncResult = v;"
        "})();",
        "<test>"), "schedule async/await");
    js.pump();
    check(eval_eq(js, "asyncResult", "99"), "async/await result == 99");
}

static void test_nanogui_module_import(nanogui::NanoJS& js) {
    printf("\n[nanogui module import]\n");

    // The 'nanogui' module is registered as a C module.
    // ES module eval requires JS_EVAL_TYPE_MODULE.
    // We test that import resolves, the exports are constructor functions,
    // and that no GL/GLFW context is needed just to import them.

    bool ok = js.eval_string(
        "import { Widget, Screen, Window } from 'nanogui';\n"
        "if (typeof Widget  !== 'function') throw new Error('Widget not a function');\n"
        "if (typeof Screen  !== 'function') throw new Error('Screen not a function');\n"
        "if (typeof Window  !== 'function') throw new Error('Window not a function');\n",
        "<module-test>",
        true   // as_module = true
    );
    check(ok, "import { Widget, Screen, Window } from 'nanogui'");
    js.pump(); // drain module instantiation microtasks

    // Import again and verify constructor names
    ok = js.eval_string(
        "import { Widget, Screen, Window } from 'nanogui';\n"
        "if (Widget.name  !== 'Widget')  throw new Error('wrong Widget name');\n"
        "if (Screen.name  !== 'Screen')  throw new Error('wrong Screen name');\n"
        "if (Window.name  !== 'Window')  throw new Error('wrong Window name');\n",
        "<name-test>",
        true
    );
    check(ok, "constructor .name properties match");
    js.pump();
}

static void test_classes_and_inheritance(nanogui::NanoJS& js) {
    printf("\n[JS class / inheritance basics]\n");

    check(js.eval_string(
        "class Animal {"
        "  constructor(n) { this.name = n; }"
        "  speak() { return this.name + ' speaks'; }"
        "}"
        "class Dog extends Animal {"
        "  speak() { return this.name + ' barks'; }"
        "}",
        "<test>"), "define Animal + Dog");

    check(eval_eq(js, "new Dog('Rex').speak()", "Rex barks"), "Dog.speak");
    check(eval_eq(js, "new Animal('Cat').speak()", "Cat speaks"), "Animal.speak");
    check(js.eval_string("if (!(new Dog('x') instanceof Animal)) throw 0;",
                         "<test>"), "instanceof Animal");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
    printf("=== NanoJS headless smoke tests ===\n");

    // Construct once; all test suites share the same runtime so state
    // accumulates (intentional — tests state persistence).
    nanogui::NanoJS js;

    test_basic_eval(js);
    test_multiline_and_state(js);
    test_error_handling(js);
    test_promises_and_pump(js);
    test_nanogui_module_import(js);
    test_classes_and_inheritance(js);

    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
