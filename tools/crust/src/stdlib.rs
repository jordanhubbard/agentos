/// Built-in standard library functions.
/// In hack mode these are implemented directly in the interpreter's method dispatch.
/// This module documents what's available and provides helpers for REPL completions.

pub const BUILTIN_FUNCTIONS: &[&str] = &[
    "println!", "print!", "eprintln!", "eprint!",
    "format!", "vec!", "assert!", "assert_eq!", "panic!", "todo!",
];

pub const BUILTIN_TYPES: &[&str] = &[
    "String", "Vec", "bool", "i64", "f64", "char", "usize",
];

pub const STRING_METHODS: &[&str] = &[
    "len", "is_empty", "contains", "starts_with", "ends_with",
    "to_uppercase", "to_lowercase", "trim", "trim_start", "trim_end",
    "replace", "split", "chars", "lines", "push_str", "push",
    "to_string", "clone", "parse", "repeat",
];

pub const VEC_METHODS: &[&str] = &[
    "push", "pop", "len", "is_empty", "get", "contains",
    "sort", "iter", "clone", "first", "last", "reverse", "join",
];
