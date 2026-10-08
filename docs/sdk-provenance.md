# Vendored source provenance

This document describes how agentOS obtains the external kernel/SDK sources it
builds from, what that arrangement proves, and — equally important — what it
does not prove.

## The problem this replaces

Until now the SDK pipeline fetched from upstream by commit SHA. The claim was:

> This is upstream seL4 at `e60776ac…`, plus a patch you can read.

A reviewer verified it by resolving that SHA in the canonical repository.

That instruction is no longer durable. The RISC-V H-extension spike
(`docs/superpowers/specs/2026-10-08-riscv-he-spike-findings.md`) found that
`au-ts/libvmm@riscv` references an sddf commit that upstream no longer serves:

```
fatal: remote error: upload-pack: not our ref e145d311…
```

Upstreams can and do delete refs. "Fetch by SHA from the canonical repo" is a
build strategy with an external single point of failure, and it is also the
*only* thing that made the provenance claim checkable. Both halves had to be
fixed at once, because the obvious fix for the first half — copy the sources
into the tree — destroys the second half.

## The mechanism

Each external dependency is a **git submodule under `vendor/`, tracking a
full-history mirror of upstream that this project controls.**

| Dependency | Path | Upstream | Mirror |
|---|---|---|---|
| seL4 | `vendor/sel4` | `github.com/seL4/seL4` | `github.com/jordanhubbard/agentos-vendor-sel4` |
| Microkit | `vendor/microkit` | `github.com/seL4/microkit` | `github.com/jordanhubbard/agentos-vendor-microkit` |

The mirrors are pushed with `--mirror`: every branch and tag, complete
history, no agentOS commits. They are byte-for-byte the upstream object graph
at the time of mirroring, which is why `git diff <upstream-sha>..<anything>`
still works inside them and why a reviewer can fetch upstream and this mirror
into one repository and confirm the two agree.

`tools/sdk/vendor.manifest` is the single declaration of what is vendored —
name, upstream URL, mirror URL, submodule path, pinned commit, and the agentOS
delta. Adding a dependency is: append a record, add the submodule, push the
mirror. No recipe changes.

The agentOS delta stays where it was: a readable patch file under
`tools/sdk/patches/`, applied by `tools/sdk/candidate.mk` to a fresh detached
checkout of the pinned commit. It is 77 lines and four files, and
`make sdk-provenance` prints its diff stat and re-checks that it still applies.

### Why not a subtree or a flat copy

Three reasons, in descending order of how decisive they are.

1. **The project constitution forbids it.** `CLAUDE.md` bans Python and says
   in terms: "vendored code is not exempt." `cargo xtask policy-check`
   enforces this over `git ls-files`, and has a regression test named
   `vendor_files_are_not_exempt`. The pinned seL4 tree contains 39 files with
   forbidden extensions and Microkit contains two (`build_sdk.py`,
   `dev_build.py`) — the very script `candidate.mk` invokes. A subtree makes
   all 41 tracked files of this repository, and `make policy-check` fails
   immediately. Making it pass would mean amending the constitution to create
   a vendoring exemption. That is not a mechanical consequence of "vendor the
   sources" and was not the decision taken. A submodule records a single
   gitlink, which `git ls-files` reports as one extension-less path; the
   forbidden files are never files of this repository. Verified: policy-check
   passes with both submodules present and checked out.

2. **The delta stays reviewable.** The feasibility study warned that folding
   upstream into the tree grows the reviewable patch to ~2,340 lines, at which
   point "a reviewer can no longer read it." With a submodule the delta is
   still the 77-line patch, because the base it applies to is a commit, not a
   copy.

3. **Repository size.** A subtree of seL4 would add its full history to every
   `git clone` of agentOS. As a submodule the superproject grows by two
   gitlinks and a `.gitmodules` stanza.

### Why not shallow or filtered submodule clones

A shallow clone cannot answer `git diff <upstream-sha>..<local>` for an
arbitrary upstream SHA, which is the property being preserved. The full
history costs 19 MB (seL4) + 2.6 MB (Microkit) on disk, measured, and is only
fetched by developers who run `make submodules`. Shallow is not worth it here.
If a future dependency has a genuinely large history, `--filter=blob:none` is
the right first lever: it keeps every commit reachable (so diffs still work)
and fetches blobs on demand. Depth truncation is not.

## What a reviewer runs

```
make submodules        # fetch the vendored mirrors
make sdk-provenance
```

For each dependency this prints:

- `UPSTREAM` — the canonical repository the sources came from.
- `MIRROR` — the agentOS-controlled mirror actually used.
- `COMMIT` — the upstream commit agentOS builds from.
- `GITLINK` — the commit this repository's **committed tree** records at that
  path, and whether it equals `COMMIT`. A mismatch is a hard failure. This is
  the load-bearing line: the pin is not prose in a Makefile, it is a gitlink,
  so git's own object integrity binds agentOS's history to that exact upstream
  commit. You cannot change which sources are built without changing this
  repository's tree hash.
- `MIRROR-CLONE` — that the mirror you just fetched really contains that
  object.
- `DELTA` / `DELTA-APPLY` — the patch path, its diff stat, and that it still
  applies cleanly to the pinned source.
- `UPSTREAM-SHA` — whether upstream still serves the commit. **When it does
  not, that is reported plainly and is not an error.** It is now an expected
  state; surviving it is the reason the sources are vendored.

To check the mirror against upstream independently of this project:

```
git clone https://github.com/jordanhubbard/agentos-vendor-sel4 sel4-check
git -C sel4-check remote add upstream https://github.com/seL4/seL4.git
git -C sel4-check fetch upstream
git -C sel4-check diff upstream/master e60776acc31097ca063806c257f07a3ec05eacf8 --stat
```

Any commit the mirror and upstream share has the same SHA-1 in both, because a
git commit ID covers its full history. That is the whole argument: if upstream
still serves `e60776ac…`, it is the same tree; if upstream has deleted it, the
mirror is the surviving copy and the SHA is what everyone else's historical
references point at.

## What this establishes

- Which upstream repository and commit each dependency claims to be.
- That this repository's tree is bound to exactly that commit, by gitlink.
- That the sources the build uses exist independently of upstream's
  willingness to keep serving them.
- The complete extent of the agentOS delta, as a readable patch.
- That the built kernels are the qualified ones —
  `tools/sdk/cr2-kernels.sha256` pins all ten artifacts and
  `SDK_CANDIDATE_ARCHIVE_SHA256` pins the published archive. Neither changed.

## What this does not establish

Stated plainly, because overclaiming is this project's primary defect:

- **It does not make the upstream code trustworthy.** Nothing here reviews
  seL4 or Microkit. The gitlink says "this is what we built"; it says nothing
  about whether that was a good idea.
- **It does not independently prove the mirror matches upstream.** It proves
  the commit ID. A reviewer who does not trust that SHA-1 collision resistance
  is sufficient must fetch both remotes and compare, as shown above. agentOS
  cannot prove this on the reviewer's behalf — that is the point of the
  reviewer doing it.
- **It does not mean upstream still has these commits.** `make sdk-provenance`
  queries and reports; the answer may be "no", and the build proceeds.
- **It does not establish that the kernel is correct**, only that it is the
  kernel whose hashes are recorded.

## What agentOS is now answerable for

This is the cost of vendoring, and it is real:

- **Mirror availability and integrity.** If the mirrors are deleted,
  rewritten, or force-pushed, agentOS's builds and its provenance claim break
  together. They must be treated as release infrastructure, not scratch repos.
- **Security updates.** Upstream fixes no longer arrive by following a branch.
  Moving to a newer seL4 is now an explicit act: push new upstream objects to
  the mirror, move the gitlink, rebase the patch, rebuild, re-record
  `cr2-kernels.sha256`, republish the archive, update
  `SDK_CANDIDATE_ARCHIVE_SHA256`.
- **The delta.** The `write_cr2` change in
  `tools/sdk/patches/sel4-e60776ac-cr2.patch` is agentOS's modification to a
  formally verified kernel's x86 VCPU path, and it is not covered by seL4's
  proofs. It was agentOS's responsibility before this change too; vendoring
  does not increase it, but it does remove the implication that someone
  upstream is watching.

## Adding a dependency

1. Mirror upstream: `git clone --bare <upstream>` then `git push --mirror
   <new mirror>`.
2. `git submodule add -- <mirror> vendor/<name>` and
   `git -C vendor/<name> checkout --detach <sha>`.
3. Append a record to `tools/sdk/vendor.manifest`.
4. If there is a delta, add the patch under `tools/sdk/patches/` and name it
   in the record.
5. `make sdk-provenance`.

Steps 1, 2 and 5 are one command each; step 3 is six lines. Nothing in
`candidate.mk`, the Makefile or the xtask code needs to change. sddf and
libvmm are the expected next two and fit this shape unchanged.

### The RISC-V H-extension fork is explicitly out of scope

Vendoring the HE fork is **not** a consequence of this change and is not
authorised by it. The mechanism would accept it — it is a mirror, a submodule
and a manifest record like any other — but the decision is separate, the spike
puts the work at 16–30 person-weeks, and the fork is not upstream seL4 at a
named commit plus a readable patch. Everything in "what this does not
establish" would apply with far more force: the delta would be large enough
that the provenance report's diff stat stops being review and starts being
decoration. If that fork is ever adopted it needs its own qualification, not
a manifest line.
