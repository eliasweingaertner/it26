<!--
Sync Impact Report
==================
Version change: template (unversioned) → 1.0.0
Rationale: Initial ratification — first concrete constitution for ittrack.

Principles (all newly defined from the template placeholders):
  - I.   Engine Fidelity is 1:1 with the Original ASM (NON-NEGOTIABLE)
  - II.  Authentic Data, Never Eyeballed
  - III. Determinism & the Regression Safety Net
  - IV.  One Real Engine
  - V.   Backend Abstraction & Portability

Added sections:
  - Toolchain & Build Constraints
  - Development Workflow
  - Governance (filled)

Removed sections: none

Templates requiring updates:
  - ✅ .specify/templates/plan-template.md (Constitution Check gates filled)
  - ✅ .specify/templates/spec-template.md (no constitution references — no change needed)
  - ✅ .specify/templates/tasks-template.md (no constitution references — no change needed)
  - ✅ .specify/templates/commands/*.md (directory does not exist — nothing to scan)

Follow-up TODOs: none. Ratification date set to first-version date (2026-06-16);
no earlier adoption date exists.
-->

# ittrack Constitution

## Core Principles

### I. Engine Fidelity is 1:1 with the Original ASM (NON-NEGOTIABLE)

The playback-engine code (`src/it_music.c`, `src/it_effects.c`,
`src/it_tables.c`, `src/it_driver.c`, `src/it_load.c`, `src/it_structs.h`) MUST
remain an instruction-for-instruction transliteration of the released Impulse
Tracker 2.17 x86 assembly. Original label/function names, the goto-mirrored
control flow, and data tables MUST be preserved byte-exact — *including the
original tables' typos* (they are part of the sound). Struct layouts MUST stay
pinned with `_Static_assert` against `InternalDocumentation/CHANNEL.TXT`.
Mechanical translations (segmented addressing → pointers/indices, FPU rounding
emulation) are permitted only where they are behaviour-preserving and recorded
in the README "Fidelity notes".

**Rationale:** This port's entire value is being auditable side-by-side against
the assembly. An "equivalent" rewrite is a different project.

### II. Authentic Data, Never Eyeballed

UI layout, colors, box styles, glyphs, palette, screen object coordinates and
behaviour MUST come from the original ASM data tables (`IT_S.ASM`, `IT_F.ASM`,
`IT_PE.ASM`, `IT_OBJ1.ASM`, `IT_DISPL.ASM`, and peers), not from guesswork.
Generated artifacts (`src/it_vgadata.c`) MUST be regenerated via their tool
(`tools/gen_vgadata.py`) and MUST NOT be hand-edited. Any approximation (e.g.
the F4 right pane currently eyeballed from screenshots) MUST be explicitly
flagged as a known deviation in the handoff docs.

**Rationale:** Fidelity extends to the look and feel; "looks about right"
silently diverges from the original.

### III. Determinism & the Regression Safety Net

Offline WAV renders MUST stay bit-deterministic. After ANY change to engine or
pattern-format code, the `tests/test_pattern.c` harness MUST pass for all four
testdata modules with byte-identical audio (all `IDENTICAL`) and matching the
known-good FNV-1a hashes. Reproducibility hooks (`Music_ResetRNG`, the
`WAV_InitSound` output-FIFO reset) MUST be preserved.

**Rationale:** Bit-exact renders are the only objective proof the port still
matches the original.

### IV. One Real Engine

The editor MUST build on the ported engine's real data structures and entry
points (the host/slave channel model, `Music_PlayNote`/freeplay PlayMode 0, and
the real pattern/sample/instrument memory layouts). No substitute or simplified
playback engine may be introduced. Editor pattern edits MUST round-trip through
the exact inverse of the player's decoder (`src/it_pattern.c`) and MUST be
serialised against the audio thread via `Engine_Lock`/`Engine_Unlock`.

**Rationale:** A faithful UI over a fake engine is not a port of Impulse
Tracker.

### V. Backend Abstraction & Portability

All code MUST compile as C11 across Windows, Linux and macOS. Platform- and
presentation-specific code MUST sit behind the `screen_backend_t` vtable in
`src/it_screen.h`; the cell buffer, control-code renderer, box drawing and the
rasterizer stay backend-independent and shared. New presentation backends go
behind that vtable. Vendored dependencies (`external/miniaudio.h`) MUST NOT be
edited.

**Rationale:** Cross-platform reach is a core goal and is only sustainable if
the shared core never accretes platform `#ifdef`s.

## Toolchain & Build Constraints

- C11 is required (notably for `_Static_assert`). Builds proceed via CMake
  (targets `itplay`, `ited`) or directly with MSVC/gcc/clang per
  `docs/HANDOFF.md` §3. MSVC requires `/std:c11 /D_CRT_SECURE_NO_WARNINGS`; the
  editor links `user32.lib gdi32.lib` for the pixel backend.
- The build configuration mirrors the 2.17 release: `SWITCH.INC` with
  `USEFPUCODE=1`, and the `WAVSWITC.INC` driver settings (`VOLUMERAMP=1`,
  `CUBICINTERPOLATION=1`, `DITHEROUTPUT=1`, `RAMPSPEED=8`, `RAMPCOMPENSATE=255`).
- Every intentional deviation from the DOS binary MUST be documented in the
  README "Fidelity notes" and kept current.

## Development Workflow

- Engine changes are not "done" until a green regression run (`docs/HANDOFF.md`
  §4) confirms all four testdata modules remain `IDENTICAL`.
- Editor changes reference the ASM for layout and behaviour but may be idiomatic
  C. Layout and colors come from the ASM data tables, not from eyeballing.
- Visual changes are iterated with `ITED_SHOT` (pixel-exact BMP) compared
  against `../screenshots/`; `ITED_DUMP` and `ITED_SELFTEST` provide
  non-interactive checks.
- Deviations and approximations are recorded in `docs/HANDOFF.md` §6 and the
  README fidelity notes.

## Governance

This constitution supersedes other practices. Amendments require a documented
rationale and a version bump following semantic versioning:

- **MAJOR** — backward-incompatible governance change: removal or redefinition
  of a principle.
- **MINOR** — a new principle or section, or materially expanded guidance.
- **PATCH** — clarifications, wording, and non-semantic refinements.

Principle I (Engine Fidelity) and Principle III (Determinism) are the
non-negotiable gates: a change that breaks the regression harness or diverges
the engine from the assembly MUST NOT be merged without explicit owner sign-off
and a recorded justification. All reviews MUST verify compliance with these
principles. `docs/HANDOFF.md` is the runtime development guidance file.

**Version**: 1.0.0 | **Ratified**: 2026-06-16 | **Last Amended**: 2026-06-16
