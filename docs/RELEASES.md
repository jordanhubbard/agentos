# agentOS Release Protocol

An agentOS release is an evidence-bound transition of one exact repository
revision. It is not a version bump followed by an optimistic tag.

The repository currently has two overlapping mutating implementations:
`scripts/release.sh` and `xtask release`. Neither is the long-term authority.
Until `task_30b9bcb838654949b26fd30595c26c3e` is complete, release engineers must
review every step manually and must not use either path to bypass protected
branch review.

## Authority and states

agentOS uses Semantic Versioning and annotated `vMAJOR.MINOR.PATCH` tags.
`CHANGELOG.md` is the release-note authority. `docs/ROADMAP.md` records planned
release outcomes; MAC records work state and dependencies.

A release moves through five deliberately separate states:

1. **Plan** — read-only. Bind the proposed version, branch, source revision,
   required gates, milestone, changelog authority, remote, and expected tag.
2. **Prepare** — update only declared version and release-note paths. Do not
   commit, tag, push, or publish.
3. **Check** — from a clean prepared commit, revalidate the plan identity and
   run its exact gates. Write an ignored receipt beneath
   `build/release/<version>/`.
4. **Publish** — after explicit authorization, create an annotated tag and
   perform ordinary non-forced pushes. Publish provider artifacts only after
   the tag exists remotely.
5. **Verify published** — read-only. Confirm the remote tag, source revision,
   checksums, release page, and attached evidence agree with the receipt.

The final implementation belongs in Rust under `xtask`; top-level Make targets
are thin entry points and shell is compatibility glue only.

## Branch policy

- Product work lands on `main` through reviewed pull requests.
- A release preparation uses `release/<major>.<minor>.x`.
- The release line contains release metadata and trunk-first backports, not
  unreviewed feature integration.
- The release PR must merge before the final tag is created.
- No release tool may force-push, move a published tag, or push directly to
  protected `main`.
- A partial publication is recovered by inspection and idempotent replay, not
  by deleting remote state.

Patch releases contain bounded correctness, security, and reliability changes
that already landed on `main`. New protocols, contract wire-format changes,
guest architectures, and significant features wait for a minor or major
release.

## Release evidence matrix

Every plan selects gates from the claims made by that release:

| Claim | Required evidence |
| --- | --- |
| Host-only library or tooling change | `make test-host` |
| agentOS root task or service behavior | `make gate` |
| Linux or FreeBSD guest behavior | focused target test plus `make demo-test` |
| Authenticated dual-guest networking | `make demo-test` with retained SSH transcripts |
| Network desktop proof | `make demo-desktop-test` with protocol and non-empty-frame evidence |
| Framebuffer or guest graphics | target framebuffer test plus guest DRM/input/frame capture |
| x86 guest support | x86 guest userspace, device, lifecycle, and isolation gates |
| Major/minor public narrative | claim-ledger verification and rendered deck QA |

Passing a narrower row does not prove a broader claim. A skipped gate must be
recorded as a limitation and may block publication depending on release scope.

## Required plan identity

The future Rust planner records at least:

- proposed and previous version;
- exact source commit and branch;
- clean/dirty status;
- canonical remote;
- release milestone and open MAC work scoped to it;
- declared changelog and version paths;
- exact Make gate commands;
- expected annotated tag;
- artifact names and checksums;
- release engineer and explicit publication authorization;
- presentation/narrative edition for major and minor releases.

Any change to the source revision, policy, gates, release metadata, or artifact
set invalidates the prepared receipt and requires a fresh check.

## Release notes and presentations

Release notes are reviewed claims, not mechanically categorized commit
subjects. Each claim states:

- what users can do;
- the mechanism that makes it true;
- the gate and retained evidence;
- known limitations or unsupported configurations;
- migration, rollback, and cleanup boundaries.

Major and minor releases refresh the agentOS systems/security narrative after
implementation and gate evidence are final. Patch releases keep the previous
edition unless the patch corrects a claim in that artifact. A published tag is
never moved to carry a later documentation refresh.

## Recovery

Publication must report Git and provider state separately. If a branch push,
tag push, or provider release fails, the operator runs read-only verification,
retains the failure record, and resumes only when local and remote identities
still select the checked commit. A conflicting remote tag is a hard stop.
