# amalgame-database-oracle

Oracle Database binding for
[Amalgame](https://github.com/amalgame-lang/Amalgame). Dynamic-
linked to the **Oracle Call Interface (OCI)** shipped with the
vendor-distributed **Oracle Instant Client** — no vendored
library. Sibling of
[`amalgame-database-postgresql`](https://github.com/amalgame-lang/amalgame-database-postgresql),
[`amalgame-database-mysql`](https://github.com/amalgame-lang/amalgame-database-mysql),
and
[`amalgame-database-mssql`](https://github.com/amalgame-lang/amalgame-database-mssql);
same dynamic-link manifest pattern.

Works against any Oracle Database release supported by the
installed Instant Client — typically the last four major
versions (19c, 21c, 23ai, plus older).

## Prerequisites

Oracle ships a single redistributable kit, the **Instant Client**,
that contains both runtime (`libclntsh.so*`) and the SDK headers
(`oci.h`). Download from:

<https://www.oracle.com/database/technologies/instant-client/downloads.html>

You need *two* packages from that page:

1. **Basic** (or *Basic Light*) — runtime shared library
2. **SDK** — `oci.h` + import library

Unpack both into the same directory:

```bash
cd /opt
sudo unzip -o ~/Downloads/instantclient-basic-linux.x64-21.13.0.0.0dbru.zip
sudo unzip -o ~/Downloads/instantclient-sdk-linux.x64-21.13.0.0.0dbru.zip
# → /opt/instantclient_21_13/{libclntsh.so.21.1, sdk/include/oci.h, …}
```

Point amc at the unpacked install via the standard env vars
amc forwards to gcc:

```bash
export AMALGAME_EXTRA_CFLAGS="-I/opt/instantclient_21_13/sdk/include"
export AMALGAME_EXTRA_LDFLAGS="-L/opt/instantclient_21_13 -Wl,-rpath,/opt/instantclient_21_13"
```

On the **deploy** machine you need the Instant Client *Basic*
package (the SDK is build-time only). Either install to a system
path on `LD_LIBRARY_PATH` / `DYLD_LIBRARY_PATH`, or ship the
shared lib alongside the binary with an explicit `-Wl,-rpath,…`
at link time.

### Per-OS quick reference

| OS | Notes |
|---|---|
| Linux x86_64 | Glibc 2.17+; both `.zip` and `.rpm` flavours shipped by Oracle |
| Linux ARM64 | Available since Instant Client 19.10+ |
| macOS (Intel) | `.dmg` installer; ship through Gatekeeper notarisation if redistributing |
| macOS (Apple Silicon) | Available since Instant Client 19.16+ |
| Windows x64 | `.zip` shipped; link `-lclntsh` against the `oci.dll` import lib |

## Install

```bash
amc package add oracle                                              # via index
amc package add github.com/amalgame-lang/amalgame-database-oracle@v0.1.0
```

Requires **amc 0.8.19+**.

## Surface

```amalgame
import Amalgame.Database.Oracle

let db = Oracle.Open("system", "oracle", "//127.0.0.1:1521/XEPDB1")
if (!Oracle.IsOpen(db)) {
    Console.WriteLine("connect failed: " + Oracle.LastError(db))
    return
}

// Oracle has no "DROP TABLE IF EXISTS" pre-23ai — swallow ORA-00942.
Oracle.Exec(db, "DROP TABLE notes")
Oracle.Exec(db, "CREATE TABLE notes ("
    + "id NUMBER GENERATED ALWAYS AS IDENTITY PRIMARY KEY, "
    + "body VARCHAR2(200))")
Oracle.Exec(db, "INSERT INTO notes (body) VALUES ('hello oracle')")

let rows = Oracle.QueryAll(db, "SELECT id, body FROM notes ORDER BY id")
let i: int = 0
while (i < rows.Count()) {
    let row = rows.Get(i)
    Console.WriteLine(row.Get(0) + ": " + row.Get(1))
    i = i + 1
}

Oracle.Close(db)
```

### v0.1.0 method surface

| Method | Returns | Notes |
|---|---|---|
| `Oracle.Open(user, password, connStr)` | `AmalgameOracle*` | Easy Connect form or TNS alias |
| `Oracle.Close(db)` | `void` | Idempotent; GC also closes leaked handles |
| `Oracle.IsOpen(db)` | `bool` | Live connection check |
| `Oracle.LastError(db)` | `string` | Empty on success; ORA-xxxxx text |
| `Oracle.Exec(db, sql)` | `bool` | DDL / INSERT / UPDATE / DELETE; autocommit |
| `Oracle.QueryAll(db, sql)` | `List<List<string>>` | SELECT, text mode (`SQLT_STR`) |
| `Oracle.Changes(db)` | `int` | Rows affected by last Exec / row count of last QueryAll |
| `Oracle.ServerVersion(db)` | `string` | "Oracle Database 23ai Free Release 23.4.0.24.05 …" |

### Connection string (Easy Connect)

```text
//127.0.0.1:1521/XEPDB1              # local Oracle XE PDB
//db.example.com/PRODSVC             # default port 1521
//db.example.com:1521/PRODSVC:dedicated  # explicit server mode
myalias                              # TNS alias from tnsnames.ora
```

Easy Connect (`//host:port/service`) is the recommended form: it
needs no `tnsnames.ora` and no `$ORACLE_HOME`-relative config. If
you prefer TNS aliases, set `TNS_ADMIN` to the directory
containing your `tnsnames.ora` before calling `Open`.

## Pixel layout / data model

The v1 surface binds every output column with `SQLT_STR` and a
fixed 4001-byte buffer. That covers VARCHAR2(4000) (Oracle's
non-LOB string maximum), NUMBER (~40 chars text-formatted),
DATE/TIMESTAMP (~30 chars), RAW (~hex-pair-per-byte up to ~2 KB),
and ROWID (~18 chars). CLOB / BLOB / LONG columns truncate at
4000 bytes — v2 will add `OCILobRead` streaming.

NULL cells materialise as `""` via the OCI indicator vector —
callers that need to distinguish `NULL` from `''` should wait
for v2's bind-variable + typed-accessor surface.

Autocommit is on for `Exec()` (`OCI_COMMIT_ON_SUCCESS`) — Oracle
otherwise opens an implicit transaction. v2 will surface
`Begin / Commit / Rollback`.

## Deferred to v2

- Bind variables (`:1`, `:name`) via `OCIBindByPos` / `OCIBindByName`
- `OCIStmtPrepare2` + statement / cursor caching
- Typed column accessors (`AsInt(col)`, `AsBytes(col)`, `AsTimestamp(col)`)
- Session pooling (`OCISessionPool*`) — Oracle connections are
  expensive enough that pooling is a real ask
- PL/SQL OUT / INOUT bind variables
- LOB streaming (`OCILobRead` / `OCILobWrite` / `OCILobAppend`)
- Continuous query notification
- Explicit transaction control (`Begin` / `Commit` / `Rollback`)

## Threading

`AmalgameOracle*` is single-owner. OCI is thread-safe when each
thread owns its own service-context handle — which is the
single-owner property anyway. Distinct handles per thread are
fine.

## Tests

```bash
./tests/run_tests.sh /path/to/amc
```

Triple-gated runner. The Instant Client SDK AND a reachable
Oracle server on `127.0.0.1:1521` AND `ORACLE_USER` +
`ORACLE_PASSWORD` env vars all need to be set, else every case
SKIPs cleanly. Start a server locally with:

```bash
docker run --rm -d --name oratest -p 1521:1521 \
  -e ORACLE_PASSWORD=oracle \
  gvenzl/oracle-xe:21-slim
```

(The `gvenzl/oracle-xe` image is a community-maintained
slimmed-down Oracle XE; the official `container-registry.oracle.com`
images work too but require an oracle.com SSO login.)

Then export:

```bash
export ORACLE_HOST=127.0.0.1 ORACLE_PORT=1521
export ORACLE_USER=system ORACLE_PASSWORD=oracle
export ORACLE_CONNSTR=//127.0.0.1:1521/XEPDB1
export ORACLE_HOME=/opt/instantclient_21_13   # where you unpacked it
```

before running the suite.

## Licence

Apache-2.0 — see [`LICENSE`](LICENSE) and [`NOTICE.md`](NOTICE.md).

The Oracle Instant Client is distributed by Oracle under their
[OTN Development and Distribution Licence](https://www.oracle.com/downloads/licenses/instant-client-lic.html)
— free for development and production use, restricted on
redistribution. This package neither bundles nor redistributes
Oracle's binaries or headers; users install the Instant Client
themselves.
