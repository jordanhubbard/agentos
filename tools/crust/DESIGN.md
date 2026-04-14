# crust — C-like Rust: A Gradual-Strictness Interpreter

## Mission

**crust** is an interpreted subset of Rust that lets you prototype fast with training wheels on — then tighten the screws when you're ready. It bridges the gap between "I want to hack on an idea" and "I want a real Rust program."

The name: **C**-like **Rust**. Start loose like C, end strict like Rust. The journey is the point.

## Why This Exists

Rust's compiler is brilliant but unforgiving. When you're sketching an algorithm or prototyping an agent behavior, fighting the borrow checker is wasted motion. But you *want* Rust's type system and ecosystem eventually.

crust solves this by making strictness a dial, not a wall:

- **Hack mode** (default): RC-based memory, no borrow checker, implicit clones. It Just Works™.
- **Pedantic mode** (`--pedantic`): Progressive warnings → errors as you tighten. Borrow checking, lifetime annotations, move semantics.
- **Eject**: When you're done, `crust eject` emits real `.rs` files that `cargo build` can compile.

## Core Principles

1. **Valid Rust is valid crust** — any crust program should be (or be trivially convertible to) real Rust
2. **The interpreter is a teaching tool** — error messages explain *why* Rust cares, not just *what* broke
3. **No magic syntax** — crust doesn't invent new keywords or sugar. It just relaxes enforcement
4. **Gradual strictness is the feature** — pedantic levels let you adopt Rust discipline incrementally

## Architecture

```
┌─────────────────────────────────────────────────┐
│                   crust CLI                      │
│  crust run foo.crs [--pedantic=N] [--eject]     │
└──────────┬──────────────────────────┬───────────┘
           │                          │
     ┌─────▼─────┐            ┌──────▼──────┐
     │   Lexer   │            │  Ejector    │
     │ (tokens)  │            │ (.crs → .rs)│
     └─────┬─────┘            └─────────────┘
           │
     ┌─────▼─────┐
     │  Parser   │
     │  (AST)    │
     └─────┬─────┘
           │
     ┌─────▼──────────┐
     │  Type Checker   │
     │  (gradual)      │
     └─────┬───────────┘
           │
     ┌─────▼──────────┐
     │  Interpreter    │
     │  (tree-walk)    │
     └────────────────┘
```

### Component Details

#### Lexer (`lexer.rs`)
Standard Rust token stream: keywords, identifiers, literals, operators, delimiters.
Recognizes all Rust keywords but the interpreter may only support a subset initially.

#### Parser (`parser.rs`)
Recursive descent parser producing an AST. Supports:
- `fn` declarations (with return types, generics later)
- `let` / `let mut` bindings
- `struct` and `enum` definitions
- `impl` blocks (methods)
- `if`/`else`, `while`, `loop`, `for..in`
- `match` expressions
- Pattern matching (destructuring)
- Expressions: arithmetic, comparison, logical, field access, method calls
- `String`, `Vec`, `HashMap` as built-in types
- `println!`, `format!`, `vec!` macros (built-in, not user-definable)
- Closures (`|args| expr`)
- `Option<T>` and `Result<T, E>` with `.unwrap()`, `?` operator

#### Type Checker (`types.rs`)
Gradual type inference — in hack mode, many things are inferred or defaulted:
- Untyped `let x = 5;` infers `i64`
- Strings default to `String` (not `&str` — no lifetimes in hack mode)
- Collections infer element types from first insertion
- Functions with no return type annotation infer from body

In pedantic mode, progressively requires:
- Level 1: Explicit function signatures
- Level 2: Explicit variable types for non-trivial expressions
- Level 3: Borrow-check warnings (suggest where `&` and `&mut` should go)
- Level 4: Full borrow checking — moves, borrows, lifetimes

#### Interpreter (`interpreter.rs`)
Tree-walking interpreter. Key design decisions:

**Hack mode memory model:**
- All values are `Rc<RefCell<Value>>` under the hood
- Assignment clones the Rc (cheap reference copy)
- No moves, no borrows, no lifetimes — values live until last reference dies
- This is semantically closer to Python/Java than Rust — and that's the point
- Mutable aliasing is allowed (RefCell handles it at runtime)

**Pedantic mode memory model:**
- Values track ownership (which binding "owns" them)
- Assignment is a move by default (old binding becomes invalid)
- `&x` creates an immutable borrow, `&mut x` creates a mutable borrow
- Borrow rules enforced: one `&mut` OR many `&`, not both
- Use-after-move is a hard error

**Standard library (built-in):**
- `String`: `new`, `push_str`, `len`, `contains`, `split`, `trim`, `chars`, `to_uppercase`, `to_lowercase`, `format!`
- `Vec<T>`: `new`, `push`, `pop`, `len`, `iter`, `map`, `filter`, `collect`, `get`, `sort`
- `HashMap<K,V>`: `new`, `insert`, `get`, `remove`, `contains_key`, `keys`, `values`, `iter`
- `Option<T>`: `Some`, `None`, `unwrap`, `unwrap_or`, `is_some`, `is_none`, `map`
- `Result<T,E>`: `Ok`, `Err`, `unwrap`, `is_ok`, `is_err`, `map`, `?` operator
- `println!`, `eprintln!`, `format!`, `vec!`
- Basic I/O: `std::fs::read_to_string`, `std::fs::write` (sandboxed)
- Math: `i64`, `f64` with standard ops

#### Ejector (`eject.rs`)
Transforms a `.crs` AST into valid `.rs` source:
- Adds explicit type annotations where inferred
- Converts RC-model code to proper ownership (best-effort)
- Adds `use` statements for standard library items
- Generates a `Cargo.toml` alongside
- Leaves `// TODO: review ownership` comments where it can't auto-convert

## File Extension

`.crs` — **c**-like **r**u**s**t. Short, distinctive, not taken.

## Usage

```bash
# Hack mode — just run it
crust run sketch.crs

# See what pedantic level 1 would complain about
crust run sketch.crs --pedantic=1

# Full Rust discipline
crust run sketch.crs --pedantic=4

# REPL for quick experiments
crust repl

# Eject to real Rust when ready
crust eject sketch.crs --output ./my_project/
```

## Example

```rust
// sketch.crs — valid crust (hack mode)
fn main() {
    let names = vec!["Alice", "Bob", "Charlie"];
    let mut greeting_map = HashMap::new();

    for name in names {
        // In real Rust, `name` would move here and `names` would be consumed.
        // In crust hack mode, it just works — names are RC'd.
        let greeting = format!("Hello, {}!", name);
        greeting_map.insert(name, greeting);
    }

    // In real Rust, accessing `names` here would be a compile error (moved).
    // In crust hack mode, it's fine — RC keeps it alive.
    println!("Greeted {} people", names.len());

    for (name, greeting) in greeting_map {
        println!("{}: {}", name, greeting);
    }
}
```

Running with `--pedantic=3`:
```
warning[crust::borrow]: `names` is consumed by `for` loop at line 4,
  but accessed again at line 11.
  --> sketch.crs:11:38
   |
4  |     for name in names {
   |                 ----- value moved here (for loop takes ownership)
   ...
11 |     println!("Greeted {} people", names.len());
   |                                   ^^^^^ use after move
   |
   = help: consider `for name in &names` to borrow instead of move
   = note: this works in hack mode (RC memory), but real Rust would reject it
```

## Implementation Language

crust itself is written in **Rust** — it's an interpreted Rust subset, written in Rust. This means:
- We eat our own dogfood
- The parser/interpreter can eventually self-host (interpret itself)
- It lives naturally in the agentOS repo alongside the kernel and services

## Project Structure

```
tools/crust/
├── DESIGN.md              # This file
├── Cargo.toml
├── src/
│   ├── main.rs            # CLI entry point
│   ├── lexer.rs           # Tokenizer
│   ├── parser.rs          # Recursive descent → AST
│   ├── ast.rs             # AST node definitions
│   ├── types.rs           # Type checker (gradual)
│   ├── interpreter.rs     # Tree-walk interpreter
│   ├── environment.rs     # Variable scopes, RC-based value store
│   ├── stdlib.rs          # Built-in types and functions
│   ├── pedantic.rs        # Borrow checking / strictness analysis
│   ├── eject.rs           # .crs → .rs converter
│   └── error.rs           # Error types and pretty-printing
├── examples/
│   ├── hello.crs
│   ├── fibonacci.crs
│   ├── linked_list.crs
│   └── ownership_demo.crs
└── tests/
    ├── lexer_tests.rs
    ├── parser_tests.rs
    ├── interpreter_tests.rs
    └── pedantic_tests.rs
```

## Scope for v0.1

Keep it small and working:
- [x] Lexer: full Rust token set
- [x] Parser: functions, let bindings, if/else, while, for, basic expressions
- [x] Interpreter: hack mode with RC memory
- [x] Types: i64, f64, bool, String, Vec, basic inference
- [x] Built-in macros: println!, vec!
- [x] CLI: `crust run` and `crust repl`
- [ ] Structs and impl blocks (v0.2)
- [ ] Enums and match (v0.2)
- [ ] Pedantic mode (v0.2)
- [ ] Ejector (v0.3)
- [ ] Closures (v0.3)
- [ ] HashMap, Option, Result (v0.2)

## agentOS Integration (Future)

crust is designed to become the scripting layer for agentOS agents:
- Agents write `.crs` files for tool implementations and behaviors
- The interpreter runs inside the agentOS sandbox (capability-constrained)
- As agent code stabilizes, `crust eject` produces compiled Rust for performance
- The gradual strictness model matches how agents learn — start messy, refine iteratively
