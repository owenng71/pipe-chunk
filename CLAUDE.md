# CLAUDE.md: pipe-chunk

OS course project #11: a C11/pthreads parallel file copier with bounded queues and backpressure.
Team: Phat (integration, CI, tests, pipeline.c), Vy (I/O), Son (queue, pool, cancel, metrics).

Read first:
- `docs/01-PROJECT-OVERVIEW.md`: what and why (architecture, buffer ownership §5.2, interfaces §10, code rules §11)
- `docs/02-PLAN-AND-ROLES.md`: who, when and how (sprints, task IDs, GitHub workflow)
- `README.md`: setup, build commands, what each file is for

Rules when writing code:
- Ubuntu 24.04, gcc, C11, pthreads. Only external lib: OpenSSL libcrypto. Build: `make`, `make test`, `make SAN=thread test`.
- Interfaces in `src/*.h` are frozen; changing one needs a PR approved by all three.
- Functions return 0/-1 and keep errno; never `exit()` inside a module; raw read/write only in `io.c`.
- Respect buffer ownership: one stage owns a record at a time. A data race is a bug even if tests pass.
- Don't implement a teammate's module unless asked. Mark known shortcuts with `// limit:`.
- Workflow: never commit to `main`. Branch `feature/<task-id>-name` → PR with `Closes #N` → Phat approves + green CI → squash-merge.
