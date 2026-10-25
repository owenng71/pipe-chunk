# CLAUDE.md: pipe-chunk

OS course project #11: a C11/pthreads parallel file copier with bounded queues and backpressure.
Team: Phat (integration, CI, tests, pipeline.c), Vy (I/O), Son (queue, pool, cancel, metrics).

Read first:
- `docs/01-PROJECT-OVERVIEW.md`: what and why (architecture, buffer ownership §5.2, interfaces §10, code rules §11)
- `docs/02-PLAN-AND-ROLES.md`: who, when and how (sprints, task IDs, GitHub workflow)
- `README.md`: setup, build commands, what each file is for

Rules when writing code:
- Ubuntu 24.04, gcc, C11, pthreads. Only external lib: OpenSSL libcrypto. Build and validate with `make`, `make test`, `make SAN=thread test`, and `make SAN=address test`.
- Every module has `tests/test_<module>.c` using a plain `main()` and `assert`; success returns 0.
- Interfaces in `src/*.h` are frozen; changing one needs a PR approved by all three and a note in `docs/design.md`.
- Frozen Overview section 10 return values take precedence over the generic 0/-1 rule for status-returning functions; all functions preserve `errno` according to their contracts. Other agreed return types follow their header contracts. Never `exit()` inside a module; raw read/write only in `io.c`.
- Respect buffer ownership: one stage owns a record at a time. A data race is a bug even if tests pass.
- Don't implement a teammate's module unless asked. Mark known shortcuts with `// limit:`. Do not edit the Makefile just to add a module; wildcard rules discover source and tests.
- Speckit specs, plans, and tasks stay local. Do not create GitHub issues or publish Speckit task lists.
- Workflow: one spec per task ID; for existing issue work use `feat/<task-id>-<short-name>` and one PR with `Closes #<issue>` → 1 approval + green CI → squash-merge.
