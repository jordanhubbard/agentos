use crate::SetupArgs;

pub fn run(args: &SetupArgs) -> anyhow::Result<()> {
    println!("agentOS Development Environment Check");
    println!("======================================");
    println!();

    let tools_to_check: &[(&str, &str)] = if args.sdk_only {
        println!("SDK-only mode: checking SDK-related tools\n");
        &SDK_TOOLS
    } else {
        println!("Checking required build tools:\n");
        &REQUIRED_TOOLS
    };

    let mut missing: Vec<&str> = Vec::new();

    for (tool, description) in tools_to_check {
        let found = check_tool(tool);
        let status = if found { "[OK]    " } else { "[MISSING]" };
        println!("  {} {} — {}", status, tool, description);
        if !found {
            if !args.install {
                print_install_hint(tool);
            }
            missing.push(tool);
        }
    }

    if !args.sdk_only {
        println!();
        println!("Checking SDK/WASM tools:\n");
        for (tool, description) in &SDK_TOOLS {
            let found = check_tool(tool);
            let status = if found { "[OK]    " } else { "[MISSING]" };
            println!("  {} {} — {}", status, tool, description);
            if !found {
                if !args.install {
                    print_install_hint(tool);
                }
                missing.push(tool);
            }
        }
    }

    if args.install && !missing.is_empty() {
        println!();
        println!("[setup] Installing {} missing tool(s)...", missing.len());
        let os = detect_os();
        for tool in missing {
            install_tool(tool, &os)?;
        }
    } else if !args.install {
        println!();
        println!("To install missing tools:");
        println!("  macOS:  brew install qemu aarch64-elf-gcc");
        println!("          rustup target add wasm32-unknown-unknown");
        println!("          cargo install trunk wasm-pack");
        println!("  Linux:  sudo apt install qemu-system-aarch64 gcc-aarch64-linux-gnu");
        println!("          rustup target add wasm32-unknown-unknown");
        println!("          cargo install trunk wasm-pack");
        println!();
        println!("  Or rerun with --install to install automatically.");
    }

    Ok(())
}

#[derive(Debug, PartialEq)]
enum Os {
    MacOs,
    Linux,
    Other,
}

fn detect_os() -> Os {
    if cfg!(target_os = "macos") {
        Os::MacOs
    } else if cfg!(target_os = "linux") {
        Os::Linux
    } else {
        Os::Other
    }
}

fn install_tool(tool: &str, os: &Os) -> anyhow::Result<()> {
    println!("[setup] Installing {}...", tool);
    match tool {
        "cargo" => {
            println!("[setup] Please install Rust via https://rustup.rs/ — skipping automatic install.");
            return Ok(());
        }
        "wasm-pack" | "trunk" => {
            let status = std::process::Command::new("cargo")
                .args(["install", tool])
                .status()
                .map_err(|e| anyhow::anyhow!("failed to run cargo install {}: {}", tool, e))?;
            anyhow::ensure!(status.success(), "cargo install {} failed", tool);
            return Ok(());
        }
        _ => {}
    }

    match os {
        Os::MacOs => {
            let brew_pkg = match tool {
                "make" => "make",
                "qemu-system-aarch64" => "qemu",
                "aarch64-linux-gnu-gcc" => "aarch64-elf-gcc",
                other => other,
            };
            let status = std::process::Command::new("brew")
                .args(["install", brew_pkg])
                .status()
                .map_err(|e| anyhow::anyhow!("failed to run brew install {}: {}", brew_pkg, e))?;
            anyhow::ensure!(status.success(), "brew install {} failed", brew_pkg);
        }
        Os::Linux => {
            let apt_pkg = match tool {
                "make" => "build-essential",
                "qemu-system-aarch64" => "qemu-system-aarch64",
                "aarch64-linux-gnu-gcc" => "gcc-aarch64-linux-gnu",
                other => other,
            };
            let status = std::process::Command::new("sudo")
                .args(["apt-get", "install", "-y", apt_pkg])
                .status()
                .map_err(|e| anyhow::anyhow!("failed to run apt-get install {}: {}", apt_pkg, e))?;
            anyhow::ensure!(status.success(), "apt-get install {} failed", apt_pkg);
        }
        Os::Other => {
            println!(
                "[setup] Unsupported OS for automatic install of {}. Please install manually.",
                tool
            );
        }
    }

    Ok(())
}

const REQUIRED_TOOLS: [(&str, &str); 4] = [
    ("make", "GNU Make — build system"),
    ("qemu-system-aarch64", "QEMU AArch64 — hardware emulation"),
    ("aarch64-linux-gnu-gcc", "AArch64 cross-compiler (Linux)"),
    ("cargo", "Rust package manager"),
];

const SDK_TOOLS: [(&str, &str); 2] = [
    ("wasm-pack", "wasm-pack — WebAssembly build tool"),
    ("trunk", "trunk — WASM dashboard bundler"),
];

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
