# Trust and delegation baseline — design

Status: draft, pending review.
Audit date: 2026-10-03. Roadmap entry: [`ROADMAP.md`](../../ROADMAP.md),
"Trust and delegation baseline".

## Intent

agentOS isolates well and delegates not at all. The device tier is factored as
intended — one owner per frame and IRQ, a separate virtualizer per class, and
guests confined to emulated virtio — and that structure is qualified on target.
What does not exist is any mechanism by which authority is established,
observed, or passed at runtime.

Three gaps motivate this work:

1. The control plane does not authenticate. `cc_pd` records a caller-supplied
   badge and acts on it. Possession of the socket is total authority over
   guest lifecycle, console input, and fault injection.
2. Nothing is verified before it runs. The root task spawns protection domains
   without hashing or checking any image. Trust in the domain set is trust in
   the boot medium.
3. The authority graph is fixed at build time and invisible at run time. There
   is no way to create a domain in response to work, no way to pass authority
   temporarily, and no way to ask what any domain currently holds.

Gap 3 is the one that matters for the platform's stated purpose. A system that
hosts agents must create them in response to tasks, and those agents will be
code the platform does not trust. The compile-time descriptor cannot express
that. This is a real limitation of the current design, not a conservative
virtue, and the audit accepts the premise that motivated the original proposal.

Success for this track means: a domain can be created at run time holding
strictly less authority than its creator; authority can be lent for the
duration of an operation and verifiably withdrawn afterward; the resulting
graph can be inspected without granting the inspector any power to change it;
and the control plane through which all of this is driven is bound to an
authority envelope.

## Threat model

Established 2026-10-03. This model governs every component below, and several
of them change shape because of it.

**The vendor is trusted. The local operator is not.** The build produces a set
of cryptographically signed units. The signing key is the vendor's and does
not exist on the board. Local operators run the hardware, may read anything
the hardware holds, and are not assumed to act in the platform's interest.

Three consequences follow, and the second is the one that constrains the
design most.

**A secret that ships in the image is not a secret from the operator.** Signing
a unit protects it from modification, not from inspection. Any symmetric
credential embedded in the build is extractable by the adversary this model
names. Therefore no component may rely on the confidentiality of build-embedded
material against an operator.

**Without a hardware root of trust, an operator with physical access can
replace the entire bundle.** Verification code the operator can substitute is
verification the operator controls, and a public key shipped in the image can
be replaced alongside the image it validates. Software signing alone cannot
prevent this; it can only make unmodified operation verifiable to a party that
already has an independent anchor. Scope for T3 and T8 therefore depends on
what the target board supplies — see open question 5.

**Authority must come from the build, not from the credential.** The usable
construction is delegation rather than authentication: the build defines an
*operator authority envelope*, and the operator's credential selects which
envelope is in force rather than attesting that its holder is trustworthy. An
operator who extracts the credential obtains exactly the envelope, which they
were already entitled to. Operations outside it are not protected by a
stronger secret; they hold no capability, and require authorization signed by
a key that is never present on the device.

This is the same hierarchical model adopted below, applied to the outermost
boundary: the build is the root of the authority tree, and the operator is a
child holding a strict subset.

## Rejected alternative: centralized trust broker

The original proposal was a central broker that mints and cryptographically
signs capabilities on request, maintains a check-in registry of entities, and
arbitrates discovery with cryptographic provenance. It is rejected. The
reasoning is recorded so it is not relitigated without new argument.

**A central minting authority is ambient authority.** A component able to
produce any capability for any requester holds, in effect, all authority in
the system. Its compromise is not partial. The proposal's own requirement that
the broker be "highly resilient to attack itself" identifies the problem
correctly; hardening raises the cost of the single event that ends the
system's guarantees without changing that such an event exists. The capability
literature is direct on this point — a central authority consulted per access
is an access-control-list system in capability vocabulary.

**It discards what seL4 is for.** seL4's integrity argument depends on the
authority graph being analyzable. Microkit and CAmkES compute that graph
offline; `system_desc_aarch64.c` is a compile-time table for this reason. A
runtime minting authority makes the graph runtime-mutable, so the platform
would pay seL4's full cost — hand-distributed untyped memory, a fixed domain
set, no dynamic allocation — and forfeit the return.

**Signing capabilities inside a node is redundant and weaker.** seL4
capabilities are unforgeable by kernel construction. A capability cannot be
fabricated, and cannot be copied out of another CSpace without the kernel
performing the copy. A signed bearer token is strictly weaker: it is bytes,
and bytes can be stolen and replayed. Cryptography is retained only where
capabilities genuinely cannot reach — across a reboot, across a machine
boundary, and before a component runs.

**Discovery with provenance dissolves under introduction.** Locating a party
one holds no capability to is the operation capability systems exist to
prevent. The present `nameserver` returns routing information to any caller
passing a badge gate, which is a leak in spirit even though the data returned
is not itself authority. The correct pattern is introduction: a domain holding
capabilities to two parties passes one to the other. No registry, no
cryptography, no provenance question — a capability cannot be held unless it
was deliberately given, so its existence is its provenance.

## Adopted model: hierarchical delegation

Every protection domain is created by a parent. A parent may endow a child
only from authority the parent itself holds, and only as a subset. Authority
flows down a tree; no edge adds authority. The invariant replaces the static
table as the analyzable property:

> No domain ever holds authority its parent did not hold.

This is the model Genode demonstrates on seL4. The model is adopted; the
codebase is not. Genode's seL4 backend is less mature than its others, and
adopting it would mean discarding the driver and virtualizer tier this project
has already qualified.

Delegation is performed by the holder, never by a third party. When domain A
grants to domain B, A mints the derived capability, because A is the only
domain that can — it holds the original. No component needs grant authority
over capabilities it does not own, so no component accumulates general
minting power.

The global view the broker would have provided is retained, read-only. An
observer that enumerates authority but mints none is not an ambient authority,
and it is what makes the subsetting invariant checkable rather than merely
asserted.

### What seL4 already supplies

| Requirement | Mechanism | Status |
| --- | --- | --- |
| Unforgeable authority | Kernel-enforced capabilities | Present |
| Rights reduction on delegation | `seL4_CNode_Mint` with reduced rights | Present; used by the root task for virtualizer badges |
| Cascading revocation | `seL4_CNode_Revoke` over the derivation tree | Present, unused after boot |
| Caller identification | Endpoint badges | Present; used for virtualizer attach authority |
| Expiry or time bound on a capability | — | **Absent. The platform must supply it.** |
| Enumerating live derivations | — | **Absent. T4 supplies it.** |

The design therefore adds two things to the kernel's model and no more: a
bound on a delegation's lifetime, and visibility into the resulting graph.

## Components

### T1 — Control-plane authority envelope

`handle_connect` in `services/command-console/cc_pd.c` assigns
`g_sessions[s].client_badge = req->mr[0]` from the request and performs no
check. Every operation reachable through CC — create, destroy, suspend,
resume, console input, snapshot, restore, fault injection — is therefore
available to any party that can open the socket.

Under the threat model this is **not** an authentication problem. A credential
supplied by the build is readable by an untrusted operator, so a session check
that merely compares a shipped token defends against a stray local process and
not against the named adversary.

The construction is an **operator authority envelope**. The build defines the
set of CC operations a local operator may perform and binds it to a credential
supplied at image construction. The credential selects which envelope is in
force; it does not establish that its holder is trustworthy. `cc_pd` refuses
any operation outside the envelope regardless of credential, so extracting the
token yields exactly the authority the operator already held.

Operations outside the envelope are not gated by a stronger secret. They
require authorization signed by the vendor key, which never exists on the
board, and they are rejected when no such authorization accompanies them. The
split between the two sets is a build input, not a runtime policy table, so it
remains analyzable offline.

The envelope is enforced structurally wherever possible, so that a defect in
CC's parsing cannot yield authority CC was never given. Reading
`cc_dispatch` shows this needs three layers, because the operations do not
separate cleanly by endpoint:

1. **Structural, where an operation has its own endpoint.** Fault injection
   reaches `fault_handler` over a dedicated channel. The default build simply
   does not grant `cc_pd` that capability, so the operation cannot be
   performed regardless of what CC's dispatcher believes. This is the
   strongest form and should be used wherever an operation maps to a distinct
   grant.
2. **Badge-enforced at the server, where one endpoint serves both sets.**
   Snapshot, restore, and guest lifecycle all reach `vm_manager` over the same
   channel, so CC cannot be denied one without losing the others. `vm_manager`
   therefore checks the caller's badge against the operation, in the same
   shape as the existing `virtualizer_authority.h` pattern — which already
   establishes that a request payload cannot select its own authority.
3. **A gate in `cc_dispatch`, as defense in depth only.** Useful for clear
   diagnostics and for operations not yet separable by either mechanism above.
   It must not be the sole control for anything, because it is exactly the
   layer an input-parsing defect bypasses.

The single `cc_dispatch` switch at `services/command-console/cc_pd.c:1901` is
the natural site for layer 3 and the inventory point for deciding which
operations layers 1 and 2 must cover.

Which operations fall inside the default envelope is open question 6.

T1 does not depend on T2: envelope selection requires comparison, not entropy.
Once T2 lands, credential presentation becomes a challenge–response exchange
so that a recorded session cannot be replayed — which matters against a
network or relay adversary even though it does not constrain the operator.

`reap_oldest_session` is a second, separate defect. When the table is full it
evicts the least-recently-active *live* session, so an unauthenticated peer
can displace an established one by connecting `CC_MAX_SESSIONS` times.
Eviction must be restricted to sessions that have expired rather than merely
aged. The two fixes ship together because either alone leaves the control
plane trivially deniable.

This component does not make the CC socket a capability boundary. It makes
possession of the socket insufficient. The socket remains a privileged
transport and should continue to be described as one.

### T2 — Entropy service

`services/entropy-service/entropy_svc.c` is a `for(;;)` stub, as are the timer
and USB services. There is no source of randomness on target, so no nonce,
key, or challenge can be generated.

The service becomes a driver domain in the existing shape: it owns its entropy
source and nothing else, and exposes a bounded request contract. It is a
driver, not a virtualizer, because there is no multiplexing decision to make —
every client receives independent output.

The proof obligation is deliberately modest. The target test asserts that the
service draws from the hardware source it claims and that clients receive
independent values. It does not assert statistical randomness quality, which
is not establishable by a boot test and should not be claimed by one.

### T3 — Image verification before spawn

The root task parses the bundle, loads each ELF, and spawns it. Nothing is
measured. A modified image boots silently.

Each bundled image carries a detached signature over its contents. The root
task verifies each one against a public key fixed at build time, before
allocating the domain's TCB, and refuses to start a domain whose signature
does not verify. Verification requires only the public key and the Ed25519
implementation already present at `libs/pd-support/ed25519_verify.c`, so this
component is independent of T2.

Failure is a boot refusal naming the rejected image, not a warning. The
existing `verify.c` precedent of a permissive development mode is deliberately
not carried forward; a verification path that can be configured off is
reported as present and behaves as absent.

**Scope limit under the threat model.** A public key shipped in the image can
be replaced along with the image, so verification anchored only in the build
does not constrain an operator with physical access to the boot medium. What
it does constrain is every adversary who can modify an image but not replace
the boot chain: remote compromise, supply paths after signing, and partial
tampering. That is a real and worthwhile boundary, and it is the honest
extent of the claim absent a hardware anchor.

Making this effective against the operator requires the verification root to
live somewhere the operator cannot rewrite — OTP-fused key hashes and signed
boot on the RPi5, UEFI Secure Boot or a firmware TPM on Intel hardware. Those
mechanisms are board properties and are not yet confirmed for the target
units; open question 5 resolves whether this component can claim
operator-resistance or must state its absence. The component is built the same
way either way. Only the claim changes.

It is not a measured-boot chain — that is T8.

### T4 — Read-only authority observer

Nothing can currently answer "what does this domain hold?" at run time. The
existing `cap_audit.c` logs per-domain capability counts at boot and stops
there, and `agentos_gui` renders a hardcoded diagram annotated with its own
message traffic, which reflects the GUI's API surface rather than the system.

The observer enumerates live authority — which domain holds which frames,
endpoints, notifications, and IRQ handlers, and the derivation relationships
among them — and publishes it through a read-only mapping, following the
pattern already established for boot inspection and the operator session's
read-only snapshot.

It is sequenced before T5 and T6 deliberately. The subsetting invariant is the
whole safety argument for hierarchical delegation, and an invariant that
cannot be observed cannot be tested. Building the inspector after the
mechanism would mean validating each against the other.

Proof obligations are two: the observed graph matches the descriptor for the
default image, and a fault probe confirms the observer's clients cannot write
the mapping or mint from it.

### T5 — Capability lending

A library, not a service. The holder of a capability mints a badged,
rights-reduced derivative, passes it to the borrower, and revokes it when the
operation completes. `seL4_CNode_Revoke` removes every descendant, so a
borrower that sub-delegated cannot outlive the revocation.

The bound is operation completion rather than wall-clock expiry. This is a
deliberate narrowing of the original "temporary, for the duration of the
operation" requirement to the part that is both well-defined and achievable
now: there is no timer service, and a time-based lease would require one plus
a policy for what happens to work in flight when a lease lapses mid-operation.
Operation-scoped lending has neither problem, and matches the stated use.

Lending works within the current static domain set, so it ships independently
of T6 and de-risks the primitive T6 depends on.

The honest limit: revocation withdraws future use. It does not undo what the
borrower did while holding the capability, and it does not recover data the
borrower copied. Lending bounds authority in time; it is not confinement.

### T6 — Hierarchical delegation and dynamic domain creation

The agentic-workload requirement. A parent creates a child domain and endows
it from the parent's own authority, enforcing the subsetting invariant, with
the result visible through T4.

This is the largest component and the one that most changes the platform's
character, because it is the point at which the authority graph stops being a
compile-time artifact. The invariant is what keeps it analyzable, which is why
it must be enforced at the creation path rather than checked afterward.

Open question for review: whether dynamic creation is reachable from the
control plane in this track, or whether T6 initially supports only
parent-driven creation from within the domain tree. The former is what the
agent use case ultimately needs; the latter is a smaller change with a
narrower blast radius, and the two can be sequenced.

### T7 — GUI

Two separable problems. The hardening items are concrete: `csp` is `null` in
`tauri.conf.json`, `capabilities/default.json` grants unscoped `core:default`
over a 29-command surface that includes `cc_connect` with a frontend-supplied
socket path and `cc_fault_inject`, and `@tauri-apps/plugin-shell` is a
declared dependency that is neither registered nor granted — inert today, one
line from being an execution surface in a window that speaks to the control
socket.

The functional item is that the GUI cannot show what it exists to show. Once
T4 publishes observed authority, the topology view is sourced from it rather
than from a hardcoded layout, and the GUI becomes a view of the system instead
of a view of itself.

The GUI cannot exceed the control plane's guarantees, so T7's security value
is contingent on T1.

### T8 and T9

T8 supersedes `boot_integrity.c`, whose Ed25519 signing key is derived from
the data being signed — so any holder of a quote can reproduce the key and
forge it — and whose signature is discarded rather than retained. It requires
T2 and T3 and is scoped after them.

**T8 is gated on hardware, not on effort.** Remote attestation requires a
signing key the operator cannot extract. Under this threat model, with no
hardware key store, an operator can produce a correct attestation for any
system state, which makes the attestation worthless to the remote party it
exists to convince. Attestation must not ship as a claim until open question 5
resolves affirmatively; shipping it without a hardware anchor would assert a
property the platform does not have. If no anchor is available, the achievable
substitute is vendor-side *detection* — a unit that cannot attest is visible
as such — which is a weaker and differently-shaped control that needs its own
scoping.

T9 is deferred. Capabilities do not traverse a network, so a cross-node
boundary genuinely requires signed tokens, but no federation is scoped and
there is no proof obligation until one is.

## Dependency order

```text
T1 control-plane auth ──┐
T2 entropy ─────────────┼──> T8 measured boot / attestation
T3 image verification ──┘

T4 observer ──> T5 lending ──> T6 hierarchical delegation
     │
     └──> T7 GUI authority view   (hardening half depends on T1)

T9 federation signing — deferred, no dependency scoped
```

T1, T2, and T3 are mutually independent. T4 is independent of all three and
may begin in parallel; it is ranked after them only because they retire more
risk per unit of work.

## Testing

The repository's existing proof taxonomy applies unchanged, and this design
claims nothing above `host-tested` without a booted image asserting it.
Host tests stub seL4 IPC and are a pre-filter.

Each component's target obligation is stated in the roadmap table. Two
cross-cutting obligations apply to the track as a whole:

- Every negative claim is a fault probe, following the pattern already used
  for network, block, and serial isolation. "The borrower cannot use the
  capability after revocation" means a target test in which the access faults
  with the expected badge, address, and direction — not a host assertion that
  a table entry was cleared.
- The subsetting invariant is checked against the T4 observer rather than
  against the code that enforces it.

`make gate` remains the OS-claim gate. Components that change the default
image must pass it; components shipping as build variants state that scope
explicitly, as the framebuffer and input paths already do.

## Constraints

`docs/TCB.md` forbids extending `cap_broker`, CapStore, and the other
quarantined components: "Do not extend these. Do not add opcodes. Do not
'finish' them." No component here may be implemented by reviving them.
`contracts/cap-broker/README.md` is additionally unusable as a specification —
it claims the broker performs the CNode operations that move capabilities
between address spaces, and the implementation contains no CNode operations at
all.

Language policy is C, Rust, or Assembly, including tests and tooling.

Each component that lands adds to the trusted computing base and must be
reflected in `docs/TCB.md` in the same change, with its qualification boundary
stated.

## Open questions for review

1. Release assignment. The recommendation is that this track becomes 0.4 with
   the guest-graphics and x86 work shifting to 0.5. Not applied.
2. T6 scope: parent-driven creation only, or reachable from the control plane
   in this track.
3. ~~T1 credential delivery.~~ **Resolved 2026-10-03:** supplied by the build.
   A TPM-backed credential is the path once hardware provides one; no current
   board is assumed to. Because the operator is untrusted, the build-supplied
   value selects an authority envelope rather than authenticating a principal
   — see the threat model.
4. Whether T4's observer is a new domain or an extension of the existing
   boot-inspection path. The latter is less new TCB surface; the former is
   cleaner to grant separately.
5. **Hardware root of trust per board.** Whether the RPi5 units use signed
   boot with an OTP-fused key hash, and whether the Intel units have UEFI
   Secure Boot or a firmware TPM (Intel PTT) available. This does not change
   how T3 is built, but it determines whether T3 may claim resistance to an
   operator with physical access and whether T8 is achievable at all. Needs
   confirmation against the actual units, not vendor documentation.

   *Assumption taken 2026-10-03, pending confirmation:* assume no hardware
   anchor. T3 is built unchanged and claims only resistance to adversaries
   who cannot replace the boot chain. T8 does not ship. This assumption can
   only make the claims stronger when corrected, never weaker, which is why
   it is the safe default to proceed under.

6. Which CC operations fall inside the default operator envelope. The split is
   a product decision about what a local operator is for, and it defines T1's
   acceptance test.

   *Assumption taken 2026-10-03, pending confirmation:* the envelope admits
   console attach, console input, and guest lifecycle (create, start, stop,
   destroy). It excludes fault injection, snapshot, restore, and trace, which
   require vendor-signed authorization. Rationale: snapshot reads guest RAM
   in full and is therefore an exfiltration primitive under this threat model,
   and fault injection is a deliberate attack tool. Lifecycle is admitted
   because an operator who cannot restart a guest cannot run the box.
