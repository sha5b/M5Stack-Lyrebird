---
name: cleanup
description: Remove AI slop from a repository — documentation that has drifted from the code, append-only session-log docs, contradictions between files, dead scaffolding, and comments that narrate instead of explain. Verifies every claim against the code before touching it. Use when asked to clean up, de-slop, audit the docs, or check that the docs still match reality.
---

# Cleanup

AI slop in a repo is not bad prose. It is **prose that no longer matches the code**,
and prose that was appended instead of edited. An agent that writes a section per
session leaves a document whose later half corrects its earlier half, and a reader
cannot tell which half is true. That is the thing to remove.

Do not rewrite for style. This repo's comments are dense on purpose — a comment that
explains *why* a value is what it is earns its lines. Deleting those is not cleanup,
it is damage.

## What counts as slop

1. **Drift** — a doc, comment or UI string states a fact the code contradicts.
   Counts, sizes, file names, pin numbers, version numbers, "N species", "not yet
   verified".
2. **Append-only history** — "second session", "third session", "update:", a section
   that corrects an earlier section rather than replacing it, `~~struck-through~~`
   claims. A handoff doc is a statement of current state, not a changelog. Git is
   the changelog.
3. **Internal contradiction** — two files, or two parts of one file, that cannot both
   be true. Fix by finding out which is true.
4. **Orphans** — references to files, flags, functions or paths that do not exist;
   empty directories; assets inherited from the project this one was copied from.
5. **Narration** — a comment that restates the line below it (`// increment i`), a
   doc section that describes what the reader can see, a summary of a summary.
6. **Unearned confidence** — "verified", "robust", "production-ready", "should work"
   where nothing was measured. Either name the measurement or drop the adjective.

## What is not slop

- Long comments that carry a reason, a measurement, or a trap. Especially
  "do not 'improve' this again" comments with data behind them — those are the most
  expensive knowledge in a repo.
- Repetition between a README and a UI string. Two audiences, two texts.
- A doc that records something the code cannot: an owner decision, a rejected
  alternative, a hardware measurement.

## Process

Work in this order. Do not edit until step 3.

### 1. Inventory the claims

Grep the docs and comments for anything checkable, and build a list. Numbers and
names are where drift hides:

```bash
grep -rniE "[0-9]+ (species|syllab|voices|KB|MB|Hz|kHz|px|ms)|\b(twelve|thirteen|dozen)\b" \
  README.md *.md src include web/src --include='*'
grep -rniE "verified|not yet|placeholder|TODO|FIXME|for now|currently" README.md *.md src include
```

### 2. Verify each claim against the code

Every claim gets checked against the thing it describes, not against another doc.
A doc that agrees with a doc proves nothing — they drift together.

- counts and sizes: read the generated file, `du -h`, `ls -la`, `grep -c`
- file names and paths: `ls`
- behaviour: read the function
- "verified on hardware": look for the measurement. No measurement, no claim.

Write down which side is wrong. Usually the code is right and the doc is stale — but
not always: a stale *comment* next to correct code can also mean the code changed by
accident. When a doc and the code disagree about intent rather than fact, that is an
owner decision. Flag it, do not resolve it silently.

### 3. Fix, in this order

1. **Correct the facts.** Smallest edit that makes the sentence true.
2. **Collapse the session log.** Rewrite an append-only doc as one statement of
   current state, organised by subject. Keep every finding that is still true;
   delete the narrative of how it was found, unless the narrative *is* the finding
   (a measurement table, a rejected approach and why).
3. **Delete orphans.** Dead references, empty directories, inherited assets.
4. **Delete narration.** Only where the comment adds nothing the code does not say.
5. **Strip unearned confidence.**

### 4. Report

State what was changed and why, and separately list:

- **Owner decisions** — the disagreements you did not resolve.
- **Not fixed** — anything found and deliberately left, with the reason.

Never report a claim as verified unless you ran the check. If a build or test was
not run, say so.

## Rules

- **Verify before you edit.** An edit that makes a doc agree with your assumption is
  worse than the drift it replaced.
- **Never invent a fact to fill a gap.** If a number cannot be established, delete
  the sentence or mark it unknown.
- **Do not touch behaviour.** This is a documentation and dead-code pass. If a real
  bug turns up, report it; fix it only if asked.
- **Prose style is a separate job.** For rewriting, use the `ste-writing` skill —
  cleanup decides *what is true*, that skill decides *how it reads*.
- **Preserve voice.** Match the surrounding text. Do not neutralise a repo that has
  a house style into generic documentation prose.
- **Generated files:** fix the generator, then regenerate. Never hand-edit a file
  whose header says it is generated. If the committed output was generated from an
  input the repo does not have, that is a finding — say so.
