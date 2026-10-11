# Daily Windows prereleases (inactive)

This lane builds unsigned Windows x64 portable and x64/ARM64 Burn installer
packages at 06:47 UTC. It reuses `build-win-mac.yml` with macOS disabled. All
existing Windows application builds, VRR tests, DualSense tests, diagnostic ZIP
checks, packaged replay smoke tests, exact replay fixtures, and packaging must
succeed before publishing. Daily gates additionally check both PE and MSI
versions, the Burn version, and the portable diagnostics/settings marker.
No SignPath action, signing secret, stable release, or latest alias is used.
Existing push/manual release and Linux workflows retain their behavior.

## Activation blockers

Do not enable until the intended source branch is confirmed. `master` is an
explicit proposed source, matching the remote default at implementation time;
the user's intended source remains unresolved. The protected local Deck commit
`a04581d6` was not integrated. If that source is intended, first publish the
reviewed source normally and revise the lane's branch guards and fetch ref.

1. Review/merge the workflow onto remote `master` and confirm Windows daily scope.
2. After confirming source, set `DAILY_RELEASE_SOURCE_BRANCH=master`. Keep
   `DAILY_RELEASE_ENABLED` unset/false and manually dispatch this workflow with
   `publish=false` (the default) to produce artifacts and validate hosted
   x64/ARM64 builds and all tests/packages. Local workflow/version tests alone do not prove those builds.
3. Confirm tag history contains every previously shipped Windows custom version.
   Daily allocation uses the next `6.2.B` above all `v6.1.0-vrrN[.P]` and
   `v6.1.0-daily.B` tags. Existing VRR mappings are unchanged. A future manual VRR
   release must map strictly above the largest daily build; coordinate manual
   release publication with this lane to avoid competing version reservations.
   Newer base-version tags, unknown custom tags, or build >65535 block allocation
   and require explicit version migration. Untagged installed custom versions
   must be accounted for before activation.
4. Set repository Actions variable `DAILY_RELEASE_SOURCE_BRANCH=master` only
   after confirmation, then `DAILY_RELEASE_ENABLED=true`. Missing source
   confirmation blocks every run. Missing enabled keeps scheduled runs and every
   publication inactive; manual artifact validation remains possible. Manual
   publication also requires explicitly choosing `publish=true`. Set enabled to
   `false` to stop scheduled publication.
5. Ensure Actions `GITHUB_TOKEN` may write release contents and branch/tag rules
   permit new daily tags. No PAT or signing credentials are needed.

Every run resolves remote master once and passes the immutable SHA to the build.
Publication uses that SHA as the tag target. The serialized daily lane rechecks
remote tags after builds; changed history or an existing release stops the run.
It creates a draft prerelease, uploads only the Burn EXE, x64 portable ZIP and
SHA256SUMS, downloads them to verify hashes, then publishes with `--latest=false`.
A failed upload leaves a draft for review. A rerun never replaces existing assets;
start a fresh run to allocate a new build after resolving a failed draft.
GitHub schedules are best effort and can run later than their cron time.

Local safeguard test:

```powershell
python -m unittest discover -s scripts/tests -p test_daily_release.py -v
```

No workflow was activated, pushed, or released during implementation. Full hosted
build/packaging validation and source approval remain activation requirements.
