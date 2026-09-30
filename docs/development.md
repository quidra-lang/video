# Development and release workflow

Video uses the permanent `develop` branch for unreleased work and `main` for
the latest published source once Video has published its first release. Before
that first release, `main` may contain repository bootstrap history only and is
not a release or installation identity. Routine work goes directly to `develop`;
never force-move either permanent branch.

`project.toml` is the metadata source of truth. After changing version,
compatibility, native sources, or native dependencies, run:

```sh
quidra package sync .
quidra package validate .
```

Core owns that parser/generator. Video owns FFmpeg semantics and its native
implementation; it does not own a parallel metadata parser.

Video uses the exact same `MAJOR.MINOR.PATCH` version as Core and Math. The
first-party package version is lockstep and must not be chosen independently.

Video uses the same `MAJOR.MINOR.PATCH` version as Core and Math; it never
chooses or advances an independent package version. A release requires the
same-version Core and Math tags to exist as immutable tags. Release Core
`vX.Y.Z` first, Math `vX.Y.Z` second, then Video `vX.Y.Z`.
Test `develop`, merge it into `main`, then let the release workflow validate
the Core native-ABI boundary, package metadata, and integration against those
exact released dependencies before creating the matching immutable tag and
GitHub Release.
