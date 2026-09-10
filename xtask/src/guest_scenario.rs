//! Resolve bounded multi-profile host scenarios used by QEMU QA.

use crate::cmd_guest_profile::{self, HostProfilePlan};
use anyhow::{ensure, Context, Result};
use clap::Args;
use serde::Deserialize;
use std::collections::BTreeSet;
use std::fs;
use std::path::{Component, Path, PathBuf};

const SCHEMA: u16 = 1;
const MAX_GUESTS: usize = 8;

#[derive(Args)]
pub struct GuestScenarioArgs {
    /// Directory containing scenario TOML files.
    #[arg(long, default_value = "guest-scenarios")]
    pub root: PathBuf,
    /// Directory containing guest profile TOML files.
    #[arg(long, default_value = "guest-profiles")]
    pub profile_root: PathBuf,
    /// Resolve this scenario alias.
    #[arg(long)]
    pub alias: String,
    /// Print the profile assigned to this target control type.
    #[arg(long)]
    pub control_type: u32,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct Scenario {
    schema: u16,
    id: String,
    aliases: Vec<String>,
    board: String,
    machine: String,
    memory: String,
    guests: Vec<ScenarioGuest>,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct ScenarioGuest {
    profile: PathBuf,
    ram_mb: u32,
    ssh_host_port: u16,
    ssh_guest_address: String,
}

#[derive(Clone, Debug)]
pub(crate) struct HostScenarioPlan {
    pub(crate) id: String,
    pub(crate) board: String,
    pub(crate) machine: String,
    pub(crate) memory: String,
    pub(crate) guests: Vec<ScenarioGuestPlan>,
}

#[derive(Clone, Debug)]
pub(crate) struct ScenarioGuestPlan {
    pub(crate) profile: HostProfilePlan,
    pub(crate) ram_mb: u32,
    pub(crate) ssh_host_port: u16,
    pub(crate) ssh_guest_address: String,
}

pub(crate) fn resolve_alias(
    scenario_root: &Path,
    profile_root: &Path,
    alias: &str,
) -> Result<HostScenarioPlan> {
    ensure!(valid_alias(alias), "invalid guest scenario alias {alias:?}");
    let mut matched = None;
    for entry in fs::read_dir(scenario_root)
        .with_context(|| format!("reading {}", scenario_root.display()))?
    {
        let path = entry?.path();
        if path.extension().and_then(|value| value.to_str()) != Some("toml") {
            continue;
        }
        let text = fs::read_to_string(&path)
            .with_context(|| format!("reading scenario {}", path.display()))?;
        let scenario: Scenario = toml::from_str(&text)
            .with_context(|| format!("parsing scenario {}", path.display()))?;
        validate(&scenario, profile_root)
            .with_context(|| format!("invalid scenario {}", path.display()))?;
        if scenario.aliases.iter().any(|candidate| candidate == alias) {
            ensure!(
                matched.is_none(),
                "guest scenario alias {alias:?} is ambiguous"
            );
            matched = Some(to_plan(scenario, profile_root)?);
        }
    }
    matched.with_context(|| format!("unknown guest scenario alias {alias:?}"))
}

pub fn run(args: &GuestScenarioArgs) -> Result<()> {
    let plan = resolve_alias(&args.root, &args.profile_root, &args.alias)?;
    let profile = plan
        .guests
        .iter()
        .find(|guest| guest.profile.control_type == args.control_type)
        .with_context(|| {
            format!(
                "scenario {} has no profile for control type {}",
                plan.id, args.control_type
            )
        })?;
    println!(
        "{}",
        profile
            .profile
            .path
            .strip_prefix(&args.profile_root)
            .unwrap_or(&profile.profile.path)
            .display()
    );
    Ok(())
}

fn validate(scenario: &Scenario, profile_root: &Path) -> Result<()> {
    ensure!(
        scenario.schema == SCHEMA,
        "scenario schema must be {SCHEMA}"
    );
    ensure!(valid_name(&scenario.id), "invalid scenario id");
    ensure!(
        !scenario.aliases.is_empty() && scenario.aliases.iter().all(|value| valid_alias(value)),
        "scenario aliases are invalid"
    );
    ensure!(valid_name(&scenario.board), "invalid scenario board");
    ensure!(
        !scenario.machine.is_empty()
            && scenario.machine.len() <= 127
            && scenario
                .machine
                .bytes()
                .all(|byte| byte.is_ascii_alphanumeric() || b",=._-".contains(&byte)),
        "invalid scenario machine"
    );
    ensure!(valid_memory(&scenario.memory), "invalid scenario memory");
    ensure!(
        (2..=MAX_GUESTS).contains(&scenario.guests.len()),
        "scenario must contain 2..={MAX_GUESTS} guests"
    );
    let mut profiles = BTreeSet::new();
    let mut controls = BTreeSet::new();
    let mut ports = BTreeSet::new();
    let mut addresses = BTreeSet::new();
    for guest in &scenario.guests {
        safe_relative(&guest.profile)?;
        ensure!(
            profiles.insert(&guest.profile),
            "duplicate scenario profile"
        );
        let profile = cmd_guest_profile::host_profile_plan(profile_root, &guest.profile)?;
        ensure!(
            profile
                .qemu
                .as_ref()
                .is_some_and(|qemu| qemu.board == scenario.board),
            "scenario profile {} uses a different board",
            profile.id
        );
        ensure!(
            controls.insert(profile.control_type),
            "duplicate scenario control_type"
        );
        ensure!(guest.ram_mb > 0, "scenario guest ram_mb must be nonzero");
        ensure!(
            guest.ssh_host_port != 0 && ports.insert(guest.ssh_host_port),
            "scenario SSH host ports must be nonzero and unique"
        );
        ensure!(
            guest
                .ssh_guest_address
                .parse::<std::net::Ipv4Addr>()
                .is_ok()
                && addresses.insert(&guest.ssh_guest_address),
            "scenario SSH guest addresses must be valid and unique"
        );
    }
    Ok(())
}

fn to_plan(scenario: Scenario, profile_root: &Path) -> Result<HostScenarioPlan> {
    let guests = scenario
        .guests
        .into_iter()
        .map(|guest| {
            Ok(ScenarioGuestPlan {
                profile: cmd_guest_profile::host_profile_plan(profile_root, &guest.profile)?,
                ram_mb: guest.ram_mb,
                ssh_host_port: guest.ssh_host_port,
                ssh_guest_address: guest.ssh_guest_address,
            })
        })
        .collect::<Result<Vec<_>>>()?;
    Ok(HostScenarioPlan {
        id: scenario.id,
        board: scenario.board,
        machine: scenario.machine,
        memory: scenario.memory,
        guests,
    })
}

fn safe_relative(path: &Path) -> Result<()> {
    ensure!(
        !path.is_absolute()
            && path.extension().and_then(|value| value.to_str()) == Some("toml")
            && path
                .components()
                .all(|part| matches!(part, Component::Normal(_))),
        "scenario profile paths must be confined relative TOML paths"
    );
    Ok(())
}

fn valid_alias(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= 63
        && value
            .bytes()
            .all(|byte| byte.is_ascii_lowercase() || byte.is_ascii_digit() || byte == b'-')
}

fn valid_name(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= 63
        && value
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || byte == b'_' || byte == b'-')
}

fn valid_memory(value: &str) -> bool {
    value.len() >= 2
        && value.len() <= 8
        && matches!(value.as_bytes().last(), Some(b'M' | b'G'))
        && value[..value.len() - 1]
            .parse::<u32>()
            .is_ok_and(|number| number > 0)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn dual_release_scenario_resolves_profiles_from_data() {
        let repo = Path::new(env!("CARGO_MANIFEST_DIR")).parent().unwrap();
        let plan = resolve_alias(
            &repo.join("guest-scenarios"),
            &repo.join("guest-profiles"),
            "both",
        )
        .unwrap();
        assert_eq!(plan.id, "dual-release-aarch64");
        assert_eq!(plan.guests.len(), 2);
        assert_eq!(plan.guests[0].profile.control_type, 2);
        assert_eq!(plan.guests[1].profile.control_type, 1);
    }
}
