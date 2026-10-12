# API-only update checks and release compatibility

Ordinary backend and frontend checks fetch only GitHub release API pages.
They never fetch metadata assets or installers. The release body's single
`<!-- nightly-identity: JSON -->` record supplies the canonical nightly
tag, version, source SHA and sequence. It is generated after the existing
qualification gates, using the same identity as the compatibility asset.
Malformed, duplicate, missing or incoherent identities fail closed; stable
selection, architecture/package completion, prerelease opt-out/defaults and
upgrade/downgrade ordering retain their existing policy. Installers remain
user-initiated release-page downloads.

## Staged transition

1. Publish a qualified bridge version carrying both the API record and the
   legacy metadata/checksum assets. Do not delete or rewrite historical releases.
2. Installed 2.0.0/current host nightlies and existing Moonlight nightly clients
   keep finding the bridge through their old contract. Their already-installed
   code still fetches the metadata asset until the user upgrades; a server-side
   deletion cannot correct that code safely.
3. New clients read API identities only. Historical nightlies without the API
   record are ignored rather than guessed from dates, titles or target branches.
   New bridge/future nightlies remain discoverable, including from old installed
   version families. Metadata removal requires a separate explicit minimum
   supported-client policy; there is no arbitrary automatic sunset here.

## Exact new nightly public asset list

`MoonlightSetup-<version>.exe`, `MoonlightPortable-x64-<version>.zip`,
`Moonlight-<version>.dmg`, `Moonlight-<version>-x86_64.AppImage`,
`Moonlight-<version>-x86_64.flatpak`, `SHA256SUMS`, and
`nightly-metadata.json`: exactly seven assets. The portable package is useful
for users who do not want an installation; the remaining packages serve separate platforms.

`SHA256SUMS` remains useful for user-initiated package integrity checks. It is
not fetched by an availability check and is not a signature or independent
publisher authentication. It hashes precisely the published downloads, including
the legacy identity asset, never itself.

Moonlight already publishes useful platform packages plus the two
compatibility/integrity assets; it has no redundant daily manifest or separate
release provenance asset to remove. Artifact source SHA/checksums remain in
metadata and the release API identity. Existing CI download-and-hash verification
before draft publication stays intact; build/publish verification is distinct
from an installed application's availability check.

Scheduled no-source-change skips, exact tag/source checks, draft-first
publication and `latest=false` are retained. CI may read historical metadata
assets to establish a verified publishing baseline; that does not run in an
installed application's ordinary update check.
