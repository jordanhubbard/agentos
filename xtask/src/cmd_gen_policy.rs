use crate::GenPolicyArgs;
use anyhow::{Context, Result};
use std::fs;

const CAP_POLICY_MAGIC: u32 = 0x4341_5050;
const CAP_POLICY_VERSION: u32 = 1;

fn parse_number(value: &str) -> Result<u32> {
    if let Some(hex) = value.strip_prefix("0x") {
        Ok(u32::from_str_radix(hex, 16)?)
    } else {
        Ok(value.parse()?)
    }
}

fn compile_policy(input: &str) -> Result<Vec<u8>> {
    let mut grants = Vec::new();
    for (line_number, line) in input.lines().enumerate() {
        let line = line.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let mut agent = None;
        let mut class = 0u32;
        let mut rights = 7u32;
        let mut flags = 0u32;
        let mut resource = 0u32;
        for field in line.split_whitespace() {
            let (name, value) = field
                .split_once('=')
                .with_context(|| format!("line {}: expected name=value", line_number + 1))?;
            match name {
                "agent" => agent = Some(parse_number(value)?),
                "class" => class = parse_number(value)?,
                "rights" => rights = parse_number(value)?,
                "flags" => flags = parse_number(value)?,
                "resource" => resource = parse_number(value)?,
                _ => anyhow::bail!("line {}: unknown field {name}", line_number + 1),
            }
        }
        let agent = agent.with_context(|| format!("line {}: missing agent", line_number + 1))?;
        anyhow::ensure!(
            agent <= u8::MAX as u32
                && class <= u8::MAX as u32
                && rights <= u8::MAX as u32
                && flags <= u8::MAX as u32,
            "line {}: byte field is out of range",
            line_number + 1
        );
        grants.push((
            agent as u8,
            class as u8,
            rights as u8,
            flags as u8,
            resource,
        ));
    }

    let mut out = Vec::with_capacity(16 + grants.len() * 8);
    out.extend_from_slice(&CAP_POLICY_MAGIC.to_le_bytes());
    out.extend_from_slice(&CAP_POLICY_VERSION.to_le_bytes());
    out.extend_from_slice(&(grants.len() as u32).to_le_bytes());
    out.extend_from_slice(&0u32.to_le_bytes());
    for (agent, class, rights, flags, resource) in grants {
        out.extend_from_slice(&[agent, class, rights, flags]);
        out.extend_from_slice(&resource.to_le_bytes());
    }
    Ok(out)
}

pub fn run(args: &GenPolicyArgs) -> Result<()> {
    let input = fs::read_to_string(&args.input)
        .with_context(|| format!("failed to read {}", args.input.display()))?;
    let output = compile_policy(&input)?;
    if let Some(parent) = args.output.parent() {
        fs::create_dir_all(parent)?;
    }
    fs::write(&args.output, &output)
        .with_context(|| format!("failed to write {}", args.output.display()))?;
    println!(
        "[gen-policy] wrote {} ({} bytes)",
        args.output.display(),
        output.len()
    );
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn emits_legacy_packed_wire_format() {
        let out =
            compile_policy("# comment\nagent=1 class=0x03 rights=0x07 flags=0x01 resource=42\n")
                .unwrap();
        assert_eq!(&out[0..4], &CAP_POLICY_MAGIC.to_le_bytes());
        assert_eq!(&out[8..12], &1u32.to_le_bytes());
        assert_eq!(&out[16..20], &[1, 3, 7, 1]);
        assert_eq!(&out[20..24], &42u32.to_le_bytes());
    }

    #[test]
    fn rejects_unknown_and_out_of_range_fields() {
        assert!(compile_policy("agent=1 surprise=2").is_err());
        assert!(compile_policy("agent=256").is_err());
    }
}
