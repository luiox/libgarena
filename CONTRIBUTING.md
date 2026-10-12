# Contributing

Thanks for considering a contribution.

## Scope of this library

This is a **C++17 classfile reading / writing / transformation library** that is
API-compatible with ObjectWeb ASM and behaviourally validated against it. It is
an independent implementation, not a port.

If your change would require copying code from ASM or any other third-party
library, it almost certainly does not belong here.

## Before you start

- Open an issue first for anything non-trivial (API shape, new module, new
  analysis pass). It is much cheaper to agree on a shape than to rework a
  finished implementation.
- Small bug fixes and documentation fixes can go straight to a PR.

## Build and test

Register the package repository once:

```sh
xmake repo --add luiox-repo https://github.com/luiox/luiox-repo.git
```

Then:

```sh
xmake f --with_tests=y
xmake build
xmake run libjbytecode_asm_core_unittest
```

The full test matrix (all suites, coverage and format checks) runs in CI on both
Windows and Linux.

## Ground rules

- **Untrusted input is the normal case.** This library parses hostile
  `.class` files. Any new code path must handle malformed input without
  crashing, leaking, or looping forever. `doc/safety.md` documents the
  intended analysis boundary — keep it accurate when you change behaviour.
- **Document new behaviour at the header.** API detail belongs in Doxygen
  comments on the declaration; `doc/` holds frozen-level summaries and
  cross-interface contracts, not API listings.
- **Add tests that fail without your change.** A test that passes before and
  after proves nothing.
- **Do not weaken an assertion to make a test pass.** If the assertion is
  wrong, fix the assertion deliberately and say why in the commit message.

## Style

- Follow the surrounding file. The project uses `.clang-format`; CI enforces it.
- Comments explain *why*, not *what*. The code already says what.
- Keep translation units free of business logic; the headers carry the contract.

## Commit messages

One logical change per commit. Describe the observable behaviour change and any
analysis-boundary implication; the diff already shows the mechanics.

## License

By contributing you agree that your contribution is licensed under the MIT
License that covers this repository.