# agentOS Systems and Security Presentation

This package owns the original agentOS narrative for operating-systems,
virtualization, and security audiences.

- `deck.md` is the editable slide sequence and speaker notes.
- `FACTS.md` is the claim ledger that must be refreshed from current source and
  runtime evidence before a release edition is rendered.

The portable baseline is Markdown. The repository-owned Rust renderer writes a
PDF and a checksum-bearing QA receipt beneath `build/`, preserves editable text
and speaker notes in the source, and excludes those notes from audience pages.
Render a release edition with:

```text
make presentation-render PRESENTATION_EDITION=0.2.0
```

The first render leaves `visual_review=required` in the adjacent QA receipt.
After reviewing the complete contact sheet and representative dense pages,
rerun with `PRESENTATION_VISUAL_REVIEW=1` to record that manual check.

No JavaScript, Python, HTML, browser runtime, private Literate AI
implementation, or mandatory cloud connector is involved.

## Edition policy

Major and minor releases refresh the deck after code and gate evidence are
final. Patch releases keep the latest major/minor edition unless they correct a
claim in the deck. Every rendered edition records:

- source commit and release version;
- factual-ledger revision;
- renderer and font versions;
- structural validation result;
- full contact-sheet review;
- full-size review of dense and image-led slides;
- unresolved limitations;
- local artifact checksum and optional publication URL.

## Content review

Before an edition is accepted, reviewers answer:

1. Can a skeptical systems practitioner reconstruct the architecture, or only
   hear claims about it?
2. Does each consequential outcome identify its mechanism and evidence?
3. Are current behavior, current limitations, near-term work, and long-term
   direction visibly distinct?
4. Is at least one real contract, capability path, failure transcript, and
   release receipt shown?
5. Does the deck answer containment, resource exhaustion, guest compromise,
   device ownership, and recovery questions?
6. Do speaker notes preserve caveats when the deck is forwarded without its
   author?
