use crate::SetupArgs;

/// Keep Make as the authority for platform tools, SDK acquisition, and checks.
/// The former independent inventory required an unused GCC cross compiler and
/// wasm-pack, and disagreed with the LLVM-based target build.
pub fn run(args: &SetupArgs) -> anyhow::Result<()> {
    let target = if args.sdk_only && !args.install {
        "sdk-check"
    } else if args.sdk_only {
        "sdk"
    } else if args.install {
        "setup"
    } else {
        "demo-check"
    };
    println!("[setup] Running make {target}");
    if !args.install {
        println!("[setup] To install platform prerequisites and the SDK, run make setup.");
    }
    let status = std::process::Command::new("make").arg(target).status()?;
    anyhow::ensure!(status.success(), "make {target} failed with {status}");
    Ok(())
}
