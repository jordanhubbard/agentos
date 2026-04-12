/*
 * quickjs_stub.h — API-compatible stub for QuickJS 2.3.x
 *
 * In production builds, replace this with the real quickjs.h from
 * https://bellard.org/quickjs/ and add quickjs.c + libregexp.c +
 * libunicode.c + cutils.c to the js_runtime build.
 *
 * The QUICKJS_PRODUCTION_BUILD macro gates the real implementation.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct JSRuntime JSRuntime;
typedef struct JSContext JSContext;
typedef uint64_t JSValue;
#define JS_TAG_EXCEPTION   6
#define JS_TAG_STRING      9
#define JS_TAG_UNDEFINED   4
#define JS_EXCEPTION ((JSValue)((uint64_t)JS_TAG_EXCEPTION << 32))
#define JS_UNDEFINED ((JSValue)((uint64_t)JS_TAG_UNDEFINED << 32))

/* Runtime/context lifecycle */
JSRuntime *JS_NewRuntime(void);
void JS_FreeRuntime(JSRuntime *rt);
JSContext *JS_NewContext(JSRuntime *rt);
void JS_FreeContext(JSContext *ctx);

/* Evaluation */
JSValue JS_Eval(JSContext *ctx, const char *input, size_t input_len,
                const char *filename, int eval_flags);
#define JS_EVAL_TYPE_GLOBAL   0
#define JS_EVAL_TYPE_MODULE   1

/* Value accessors */
int JS_IsException(JSValue val);
const char *JS_ToCString(JSContext *ctx, JSValue val);
void JS_FreeCString(JSContext *ctx, const char *ptr);
void JS_FreeValue(JSContext *ctx, JSValue val);
JSValue JS_GetException(JSContext *ctx);
JSValue JS_ToString(JSContext *ctx, JSValue val);

/* Global object */
JSValue JS_GetGlobalObject(JSContext *ctx);
JSValue JS_GetPropertyStr(JSContext *ctx, JSValue obj, const char *prop);
JSValue JS_Call(JSContext *ctx, JSValue func_obj, JSValue this_obj,
                int argc, JSValue *argv);
JSValue JS_NewString(JSContext *ctx, const char *str);
int JS_SetPropertyStr(JSContext *ctx, JSValue obj, const char *prop, JSValue val);
