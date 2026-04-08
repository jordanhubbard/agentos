use crate::SetupArgs;

pub fn run(args: &SetupArgs) -> anyhow::Result<()> {
    println!("agentOS Development Environment Check");
    println!("======================================");
    println!();

    if args.sdk_only {
        println!("SDK-only mode: checking SDK-related tools\n");
        check_sdk_tools();
    } else {
        println!("Checking required build tools:\n");
        check_required_tools();
        println!();
        println!("Checking SDK/WASM tools:\n");
        check_sdk_tools();
    }

    println!();
    println!("To install missing tools:");
    println!("  macOS:  brew install qemu aarch64-elf-gcc");
    println!("          rustup target add wasm32-unknown-unknown");
    println!("          cargo install trunk wasm-pack");
    println!("  Linux:  sudo apt install qemu-system-aarch64 gcc-aarch64-linux-gnu");
    println!("          rustup target add wasm32-unknown-unknown");
    println!("          cargo install trunk wasm-pack");

    Ok(())
}

fn check_required_tools() {
    let required = [
        ("make", "GNU Make — build system"),
        ("qemu-system-aarch64", "QEMU AArch64 — hardware emulation"),
        ("aarch64-linux-gnu-gcc", "AArch64 cross-compiler (Linux)"),
        ("cargo", "Rust package manager"),
    ];

    for (tool, description) in &required {
        let found = check_tool(tool);
        let status = if found { "[OK]    " } else { "[MISSING]" };
        println!("  {} {} — {}", status, tool, description);
        if !found {
            print_install_hint(tool);
        }
    }
}

fn check_sdk_tools() {
    let sdk_tools = [
        ("wasm-pack", "wasm-pack — WebAssembly build tool"),
        ("trunk", "trunk — WASM dashboard bundler"),
    ];

    for (tool, description) in &sdk_tools {
        let found = check_tool(tool);
        let status = if found { "[OK]    " } else { "[MISSING]" };
        println!("  {} {} — {}", status, tool, description);
        if !found {
            print_install_hint(tool);
        }
    }
}

fn print_install_hint(tool: &str) {
    match tool {
        "make" => println!("         Install: brew install make  OR  apt install build-essential"),
        "qemu-system-aarch64" => {
            println!("         Install: brew install qemu  OR  apt install qemu-system-aarch64")
        }
        "aarch64-linux-gnu-gcc" => {
            println!("         Install: brew install aarch64-elf-gcc  OR  apt install gcc-aarch64-linux-gnu")
        }
        "cargo" => println!("         Install: https://rustup.rs/"),
        "wasm-pack" => println!("         Install: cargo install wasm-pack"),
        "trunk" => println!("         Install: cargo install trunk"),
        _ => {}
    }
}

fn check_tool(name: &str) -> bool {
    std::process::Command::new("which")
        .arg(name)
        .output()
        .map(|o| o.status.success())
        .unwrap_or(false)
}
