# Security Policy

## Supported Versions

Only the latest tagged release receives security fixes.

## Reporting a Vulnerability

Please report security issues privately rather than opening a public issue.

Use GitHub's private vulnerability reporting for this repository
(**Security → Report a vulnerability**), or contact the maintainer directly.

Please include:

- affected version (tag or commit)
- platform and toolchain (e.g. Windows/MSVC 2022, xmake version)
- a minimal reproducer if possible
- the impact you observed (crash, memory corruption, unbounded allocation, infinite loop, out-of-bounds read/write)

## Scope

This library parses untrusted binary input (`.class` files, archives). Reports
about malformed-input handling — out-of-bounds access, unbounded memory growth,
or unbounded loops triggered by hostile input — are in scope.

Reports limited to the documented analysis boundaries (see `doc/safety.md`)
are not vulnerabilities; those boundaries are stated precisely and are
intentional design decisions.