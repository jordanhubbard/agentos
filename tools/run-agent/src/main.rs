//! run-agent — load and execute a signed agentOS WASM agent on the host.
//!
//! Usage examples:
//!
//!   run-agent agent.wasm
//!   run-agent agent.wasm --init-only
//!   run-agent agent.wasm --ppc 0xA0 1 2 3 4
//!   run-agent agent.wasm --notify 1 --notify 3
//!   run-agent agent.wasm --verbose

use std::path::PathBuf;
use anyhow::{Context, Result};
use clap::{Parser, ArgAction};
use agentos_sim::{SimEngine, VerifyMode};

#[derive(Parser, Debug)]
#[command(
    name = "run-agent",
    about = "Run a signed agentOS WASM agent in the host simulator",
    version,
)]
struct Cli {
    /// Path to the .wasm agent file
    wasm: PathBuf,

    /// Only call init(); skip ppc / notifications / health-check
    #[arg(long)]
    init_only: bool,

    /// Inject an IPC call: --ppc <opcode> [mr1 mr2 mr3 mr4]
    /// The opcode becomes MR0/label; remaining args fill MR1-MR4.
    #[arg(long = "ppc", num_args = 1..=5, value_names = ["OPCODE", "MR1", "MR2", "MR3", "MR4"])]
    ppc: Option<Vec<String>>,

    /// Inject a notification on a channel (can be repeated)
    #[arg(long = "notify", action = ArgAction::Append, value_name = "CHANNEL")]
    notify: Vec<u32>,

    /// Skip capability setup (run without any default grants)
    #[arg(long)]
    no_caps: bool,

    /// Agent name used in log output
    #[arg(long, default_value = "agent")]
    name: String,

    /// Verbose tracing output
    #[arg(short, long)]
    verbose: bool,

    /// Require a valid agentos.signature section; fail if absent or mismatched
    #[arg(long)]
    strict_verify: bool,
}

fn main() -> Result<()> {
    let cli = Cli::parse();

    // Initialise tracing
    let filter = if cli.verbose { "debug" } else { "info" };
    tracing_subscriber::fmt()
        .with_env_filter(
            tracing_subscriber::EnvFilter::try_from_default_env()
                .unwrap_or_else(|_| tracing_subscriber::EnvFilter::new(filter)),
        )
        .with_target(false)
        .init();

    // Load WASM
    let wasm_bytes = std::fs::read(&cli.wasm)
        .with_context(|| format!("cannot read {}", cli.wasm.display()))?;

    println!("[run-agent] loaded {} ({} bytes)", cli.wasm.display(), wasm_bytes.len());

    // Set up sim engine
    let engine = SimEngine::new();

    if !cli.no_caps {
        engine.caps_mut().grant_defaults(&cli.name);
    }

    // Subscribe to all EventBus events so we can log them
    engine.eventbus_mut().subscribe("*", |ev| {
        println!("[eventbus] topic={} payload={} bytes src={:?}",
            ev.topic, ev.payload.len(), ev.source);
    });

    // Spawn agent
    let verify_mode = if cli.strict_verify {
        VerifyMode::Strict
    } else {
        VerifyMode::WarnOnly
    };
    let mut runner = engine.spawn_agent_verified(&cli.name, &wasm_bytes, verify_mode)
        .context("failed to instantiate WASM agent")?;

    println!("[run-agent] calling init()");
    runner.init().context("init() failed")?;

    if cli.init_only {
        println!("[run-agent] --init-only: stopping after init()");
        print_summary(&runner);
        return Ok(());
    }

    // Inject notifications
    for ch in &cli.notify {
        runner.state_mut().shim.inject_notification(*ch);
    }
    if !cli.notify.is_empty() {
        println!("[run-agent] draining {} notification(s)", cli.notify.len());
        runner.drain_notifications().context("notified() failed")?;
    }

    // Inject PPC call
    if let Some(args) = &cli.ppc {
        let parse_u64 = |s: &str| -> Result<i64> {
            let v = if s.starts_with("0x") || s.starts_with("0X") {
                i64::from_str_radix(&s[2..], 16)?
            } else {
                s.parse::<i64>()?
            };
            Ok(v)
        };
        let mr0 = parse_u64(&args[0]).context("bad opcode")?;
        let mr1 = args.get(1).map(|s| parse_u64(s)).transpose()?.unwrap_or(0);
        let mr2 = args.get(2).map(|s| parse_u64(s)).transpose()?.unwrap_or(0);
        let mr3 = args.get(3).map(|s| parse_u64(s)).transpose()?.unwrap_or(0);
        let mr4 = args.get(4).map(|s| parse_u64(s)).transpose()?.unwrap_or(0);

        println!("[run-agent] calling handle_ppc(0x{:x}, {}, {}, {}, {})", mr0, mr1, mr2, mr3, mr4);
        runner.handle_ppc(mr0, mr1, mr2, mr3, mr4).context("handle_ppc() failed")?;
    }

    // Health check
    let health = runner.health_check().context("health_check() failed")?;
    println!("[run-agent] health_check() = {} ({})", health,
        if health == 0 { "healthy" } else { "DEGRADED" });

    print_summary(&runner);
    Ok(())
}

fn print_summary(runner: &agentos_sim::AgentRunner) {
    let state = runner.state();
    println!("[run-agent] --- summary ---");
    println!("  log lines     : {}", state.log_lines.len());
    println!("  ppcall count  : {}", state.shim.call_log.len());
    println!("  notify count  : {}", state.shim.notify_log.len());

    for (i, line) in state.log_lines.iter().enumerate() {
        println!("  log[{}]: {}", i, line.trim_end());
    }
    for call in &state.shim.call_log {
        println!("  ppcall ch={} opcode=0x{:x} mr_count={}",
            call.channel, call.info.label, call.info.mr_count);
    }
    for notif in &state.shim.notify_log {
        println!("  notify ch={}", notif.channel);
    }
}
