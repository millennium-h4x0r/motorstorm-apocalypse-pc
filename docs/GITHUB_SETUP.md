# GitHub Setup

This document describes how to set up the repository for GitHub if you're contributing.

## Initial Setup

After cloning the repository:

```bash
cd motorstorm_apoca

# Install Git hooks
scripts/install-hooks.sh

# Set up the toolkit
scripts/setup.sh
```

## Directory Structure

The repository uses a Git allowlist (`.gitignore` denies everything by default, then allows specific files/directories):

- ✅ **Allowed**: Documentation, scripts, port source code, patches
- ❌ **Forbidden**: Game files, lifted code, build artifacts, save data

## Before Committing

The pre-commit hook will run `scripts/check_clean.sh` automatically to verify:

- No game files (`.elf`, `.self`, `.bin`, etc.)
- No credentials or API keys
- No binary files (except JPEGs in `docs/screenshots/`)
- No code lifted from the game (in `work/src/recomp/`)
- File sizes are reasonable

To check manually:

```bash
scripts/check_clean.sh            # Check staged changes
scripts/check_clean.sh --tracked  # Check all tracked files
```

## Making Changes

When working on the port:

1. **Port Code**: Changes to `work/port/` (C++ implementation)
2. **Tools**: Changes to `work/tools/` (Python/shell utilities)
3. **Documentation**: Changes to `docs/` and root files

All changes must pass the clean check before committing.

## Pushing to Remote

The pre-push hook will verify all tracked files pass the check:

```bash
scripts/check_clean.sh --tracked
```

If anything fails, the push is blocked until issues are resolved.

## Patches

When modifying ps3recomp, regenerate the patch:

```bash
scripts/make_patch.sh
```

This creates `work/patches/ps3recomp.patch` with all changes against the upstream base commit.

## Continuous Integration

The repository can be set up with CI checks that run:

1. `scripts/check_clean.sh --tracked` - Verify no forbidden files
2. Code linting and formatting checks
3. Build verification

## Collaboration

When collaborating:

1. Create feature branches from `main`
2. Keep commits focused and well-described
3. Ensure all checks pass before pushing
4. Provide context in commit messages about game-specific changes

## Legal and Attribution

Remember:

- No game files or code from lifted binaries in the repository
- All patches to ps3recomp require proper attribution
- Use appropriate licenses (MIT for most code)
- Game content remains the intellectual property of the original creators
