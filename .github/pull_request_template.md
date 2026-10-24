Closes #

**What / why:**

**Definition of Done**
- [ ] `make` and `make test` pass
- [ ] `make SAN=thread test` passes (no data races)
- [ ] `make SAN=address test` passes (no memory errors)
- [ ] Has a test that would fail if the logic broke
- [ ] Interface or behavior changed → note added in `docs/design.md`
- [ ] If a shared header changed, all three members approved the PR
