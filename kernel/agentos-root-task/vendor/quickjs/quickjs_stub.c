#include "../../include/agentos.h"
#include "quickjs_stub.h"

JSRuntime *JS_NewRuntime(void) {
    console_log(16, 16, "[js_runtime] QUICKJS_STUB: JS_NewRuntime (no-op)\n");
    return (JSRuntime *)1;
}

void JS_FreeRuntime(JSRuntime *rt) {
    (void)rt;
    console_log(16, 16, "[js_runtime] QUICKJS_STUB: JS_FreeRuntime (no-op)\n");
}

JSContext *JS_NewContext(JSRuntime *rt) {
    (void)rt;
    console_log(16, 16, "[js_runtime] QUICKJS_STUB: JS_NewContext (no-op)\n");
    return (JSContext *)2;
}

void JS_FreeContext(JSContext *ctx) {
    (void)ctx;
    console_log(16, 16, "[js_runtime] QUICKJS_STUB: JS_FreeContext (no-op)\n");
}

JSValue JS_Eval(JSContext *ctx, const char *input, size_t input_len,
                const char *filename, int eval_flags) {
    (void)ctx;
    (void)input;
    (void)input_len;
    (void)filename;
    (void)eval_flags;
    console_log(16, 16, "[js_runtime] QUICKJS_STUB: JS_Eval (no-op)\n");
    return JS_UNDEFINED;
}

int JS_IsException(JSValue val) {
    (void)val;
    return 0;
}

const char *JS_ToCString(JSContext *ctx, JSValue val) {
    (void)ctx;
    (void)val;
    return "[stub result]";
}

void JS_FreeCString(JSContext *ctx, const char *ptr) {
    (void)ctx;
    (void)ptr;
}

void JS_FreeValue(JSContext *ctx, JSValue val) {
    (void)ctx;
    (void)val;
}

JSValue JS_GetException(JSContext *ctx) {
    (void)ctx;
    console_log(16, 16, "[js_runtime] QUICKJS_STUB: JS_GetException (no-op)\n");
    return JS_UNDEFINED;
}

JSValue JS_ToString(JSContext *ctx, JSValue val) {
    (void)ctx;
    (void)val;
    return JS_NewString(ctx, "[stub result]");
}

JSValue JS_GetGlobalObject(JSContext *ctx) {
    (void)ctx;
    console_log(16, 16, "[js_runtime] QUICKJS_STUB: JS_GetGlobalObject (no-op)\n");
    return JS_UNDEFINED;
}

JSValue JS_GetPropertyStr(JSContext *ctx, JSValue obj, const char *prop) {
    (void)ctx;
    (void)obj;
    (void)prop;
    console_log(16, 16, "[js_runtime] QUICKJS_STUB: JS_GetPropertyStr (no-op)\n");
    return JS_UNDEFINED;
}

JSValue JS_Call(JSContext *ctx, JSValue func_obj, JSValue this_obj,
                int argc, JSValue *argv) {
    (void)ctx;
    (void)func_obj;
    (void)this_obj;
    (void)argc;
    (void)argv;
    console_log(16, 16, "[js_runtime] QUICKJS_STUB: JS_Call (no-op)\n");
    return JS_UNDEFINED;
}

JSValue JS_NewString(JSContext *ctx, const char *str) {
    (void)ctx;
    (void)str;
    return JS_UNDEFINED;
}

int JS_SetPropertyStr(JSContext *ctx, JSValue obj, const char *prop, JSValue val) {
    (void)ctx;
    (void)obj;
    (void)prop;
    (void)val;
    console_log(16, 16, "[js_runtime] QUICKJS_STUB: JS_SetPropertyStr (no-op)\n");
    return 0;
}
