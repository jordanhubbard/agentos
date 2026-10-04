# `dev_signing_key.seed`

This is a raw 32-byte Ed25519 seed, generated with `openssl rand -out
dev_signing_key.seed 32` and committed to this repository on purpose.

**It is deliberately not a secret.** It exists so that a developer building
agentOS from a fresh checkout, with no key material of their own, still gets
a boot manifest (see `kernel/agentos-root-task/include/boot_manifest.h` and
`xtask/src/boot_manifest.rs`) that the root task's compiled-in public key
will accept — rather than the build either failing outright or silently
skipping signature verification. The matching public key is derived from
this seed and compiled into the root task at build time; the generated boot
manifest header also carries `AOS_BOOT_MANIFEST_DEV_SIGNED 1` whenever this
key was used, and the root task prints an unmissable
`[rt] WARNING: DEVELOPMENT-signed image` line at boot when it is.

**It must never be used to sign anything that will be presented as, or
mistaken for, a production image.** Anyone with a copy of this repository
can sign a manifest this key's matching root task will accept; it
establishes nothing about provenance or integrity beyond "built from source
that still had the default key."

## Using your own key instead

Set `AGENTOS_BUNDLE_SIGNING_KEY` to the path of your own 32-byte raw Ed25519
seed before building (e.g. `openssl rand -out my_key.seed 32`). When set,
`xtask gen-pd-bundle` reads that file instead of this one, signs the
manifest with it, and compiles the *matching* public key into the root
task — the dev key and its warnings are not used at all when this variable
is set. See `xtask/src/boot_manifest.rs::load_signing_key` for the exact
logic.
