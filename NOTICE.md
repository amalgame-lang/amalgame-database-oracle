# NOTICE — amalgame-database-oracle

## Authorship

Copyright 2026 Bastien Mouget. Original work — see
`runtime/Amalgame_Database_Oracle.h`.

Part of the Amalgame ecosystem
([github.com/amalgame-lang/Amalgame](https://github.com/amalgame-lang/Amalgame)).
External contributions are paused at the ecosystem level; see the
main repo's `CONTRIBUTING.md` for the policy.

AI tools (Anthropic Claude) were used during development. Per
the project's authorship policy, AI is treated as a tool, not a
co-author at law.

## Licence

Apache License 2.0. See `LICENSE` for the full text.

## Third-party content

**None vendored.** This package binds to the Oracle Call
Interface (OCI) C library shipped with the *Oracle Instant
Client*. The Instant Client is provided by Oracle Corporation
under their own
[OTN Development and Distribution Licence](https://www.oracle.com/downloads/licenses/instant-client-lic.html)
— free of charge for development and production use, but
restricted on redistribution.

Users must download the Instant Client themselves from
<https://www.oracle.com/database/technologies/instant-client/downloads.html>.
This package neither bundles nor redistributes Oracle's binaries
or headers.

The user binary dynamically links against `libclntsh` (the
Instant Client's shared library); under Oracle's licence this is
the supported and intended use pattern.

## Trademarks

"Oracle", "Oracle Database", and "Oracle Instant Client" are
registered trademarks of Oracle Corporation. "OCI" (the Oracle
Call Interface) is also Oracle's nomenclature. This repository
uses those names solely to identify the database engine and
client library the package binds to. No trademark claim is
asserted.
