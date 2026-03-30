# Contributing to urt-proxy

Thanks for your interest in contributing! This is a small, focused project — contributions that keep it simple and reliable are welcome.

## Building

```bash
make          # produces build/urt-proxy
make clean    # removes build/
```

Requires GCC and GNU Make on a Linux system. No external libraries — only POSIX and standard C11.

## Code Style

- **C standard:** C11 (`-std=c11`)
- **Comments:** C-style block comments (`/* ... */`). No `//` single-line comments.
- **Warnings:** Code must compile cleanly with `-Wall -Wextra -pedantic`.
- **Naming:** `snake_case` for functions and variables. Prefix module names (e.g. `session_map_init`, `q3_is_connectionless`).
- **Headers:** Each `.c` file has a corresponding `.h` in `include/`. Keep headers self-contained (include their own dependencies).

## Submitting Changes

1. Fork the repo and create a feature branch from `main`.
2. Make your changes, keeping commits focused and descriptive.
3. Make sure `make clean && make` succeeds with no warnings.
4. Open a pull request with a clear description of what changed and why.

## Reporting Issues

Open a GitHub issue with:

- What you expected vs. what happened
- Steps to reproduce
- OS, GCC version, and any relevant configuration

## Scope

This project intentionally stays minimal — it's a UDP relay, not a full game server framework. Features that add significant complexity or external dependencies are unlikely to be merged.
