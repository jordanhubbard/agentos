mod ast;
mod environment;
mod error;
mod interpreter;
mod lexer;
mod parser;
mod stdlib;
mod types;

use std::fs;
use interpreter::Interpreter;
use lexer::Lexer;
use parser::Parser;

fn run_source(source: &str, filename: &str) -> Result<(), String> {
    let mut lexer = Lexer::new(source);
    let tokens = lexer.tokenize().map_err(|e| format!("{}: {}", filename, e))?;
    let mut parser = Parser::new(tokens);
    let program = parser.parse_program().map_err(|e| format!("{}: {}", filename, e))?;
    let mut interp = Interpreter::new();
    interp.run_program(&program).map_err(|e| format!("{}: {}", filename, e))?;
    Ok(())
}

fn run_repl() {
    println!("crust v0.1 REPL — hack mode. Type 'exit' or Ctrl-D to quit.");
    println!("Hint: wrap statements in fn main() {{ ... }} or type expressions directly.");

    let mut rl = match rustyline::DefaultEditor::new() {
        Ok(r) => r,
        Err(e) => {
            eprintln!("Failed to init REPL: {}", e);
            return;
        }
    };

    let mut buffer = String::new();
    let mut brace_depth: i32 = 0;

    loop {
        let prompt = if brace_depth > 0 { "...  " } else { ">>> " };
        match rl.readline(prompt) {
            Ok(line) => {
                if line.trim() == "exit" || line.trim() == "quit" {
                    println!("Goodbye!");
                    return;
                }
                let _ = rl.add_history_entry(&line);
                for c in line.chars() {
                    if c == '{' { brace_depth += 1; }
                    if c == '}' { brace_depth -= 1; }
                }
                buffer.push_str(&line);
                buffer.push('\n');

                if brace_depth <= 0 {
                    brace_depth = 0;
                    let src = buffer.trim().to_string();
                    buffer.clear();

                    if src.is_empty() { continue; }

                    // If input looks like a full fn declaration, run it directly
                    // Otherwise wrap in fn main() {}
                    let source = if src.starts_with("fn ") || src.starts_with("pub fn ") {
                        src.clone()
                    } else {
                        format!("fn main() {{\n{}\n}}", src)
                    };

                    match run_source(&source, "<repl>") {
                        Ok(()) => {}
                        Err(e) => eprintln!("error: {}", e),
                    }
                }
            }
            Err(rustyline::error::ReadlineError::Interrupted) => {
                buffer.clear();
                brace_depth = 0;
                println!("^C");
            }
            Err(rustyline::error::ReadlineError::Eof) => {
                println!("Goodbye!");
                return;
            }
            Err(e) => {
                eprintln!("REPL error: {}", e);
                return;
            }
        }
    }
}

fn print_usage() {
    eprintln!("crust v0.1 — a gradual-strictness Rust interpreter");
    eprintln!();
    eprintln!("USAGE:");
    eprintln!("    crust run <file.crs>        Run a .crs program");
    eprintln!("    crust repl                  Start interactive REPL");
    eprintln!();
    eprintln!("OPTIONS:");
    eprintln!("    --pedantic=N     Set strictness level (1-4, default: 0 = hack mode)");
    eprintln!("    --help, -h       Show this help");
}

fn main() {
    let args: Vec<String> = std::env::args().collect();

    if args.len() < 2 {
        print_usage();
        std::process::exit(1);
    }

    match args[1].as_str() {
        "run" => {
            if args.len() < 3 {
                eprintln!("error: 'crust run' requires a file argument");
                std::process::exit(1);
            }
            let file = &args[2];
            let source = match fs::read_to_string(file) {
                Ok(s) => s,
                Err(e) => {
                    eprintln!("error: cannot read '{}': {}", file, e);
                    std::process::exit(1);
                }
            };
            match run_source(&source, file) {
                Ok(()) => {}
                Err(e) => {
                    eprintln!("{}", e);
                    std::process::exit(1);
                }
            }
        }
        "repl" => {
            run_repl();
        }
        "--help" | "-h" | "help" => {
            print_usage();
        }
        cmd => {
            eprintln!("error: unknown command '{}'", cmd);
            print_usage();
            std::process::exit(1);
        }
    }
}
