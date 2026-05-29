/*
 * Amalgame Standard Library — Amalgame.Database.Oracle
 * Copyright (c) 2026 Bastien MOUGET
 * https://github.com/amalgame-lang/Amalgame
 *
 * Oracle Database binding — dynamic-linked to the Oracle Call
 * Interface (OCI) C library shipped with the vendor-distributed
 * *Oracle Instant Client*. No vendored implementation; the user
 * binary links against `-lclntsh` from the Instant Client install.
 *
 * Works against any Oracle Database release supported by the
 * installed Instant Client (typically the last four major
 * versions: 19c, 21c, 23ai, plus older).
 *
 * Surface (v1):
 *   Open(user, password, connStr) / Close /
 *     IsOpen / LastError                          lifecycle + diag
 *   Exec(sql)                                      DDL / INSERT / UPDATE / DELETE
 *   QueryAll(sql) -> List<List<string>>            SELECT all rows × cols
 *   Changes()                                      rows affected by last Exec
 *   ServerVersion()                                Oracle server release string
 *
 * `connStr` follows the Oracle "Easy Connect" form (no
 * tnsnames.ora needed):
 *
 *   "//127.0.0.1:1521/XEPDB1"
 *   "//db.example.com/PRODSVC"
 *   "myalias"  (TNS alias from $ORACLE_HOME/network/admin/tnsnames.ora)
 *
 * NULL cells in QueryAll materialise as the empty string —
 * callers that need to distinguish NULL from `""` should wait
 * for v2's bind-variable + typed-accessor surface.
 *
 * Threading: AmalgameOracle* is single-owner. OCI is thread-safe
 * when each thread owns its own service-context handle, which is
 * the single-owner property anyway. Distinct handles per thread
 * are fine.
 *
 * Memory: OCI statement handles are OCIHandleFree'd as soon as
 * we've copied the data we need. The service-context, error,
 * and environment handles live for the lifetime of the
 * AmalgameOracle* and are released in Close(). GC finalizer
 * registered so a leaked handle still releases everything
 * eventually.
 *
 * Column buffers: v1 allocates a 4001-byte buffer per column and
 * binds it as SQLT_STR. That covers VARCHAR2(4000), NUMBER (~40
 * chars when text-formatted by OCI), DATE/TIMESTAMP (~30 chars),
 * and ROWID (~18 chars) without truncation. CLOB / LONG / BLOB
 * columns truncate at 4000 bytes — v2 will add LOB streaming.
 *
 * Out of scope (v2):
 *   - Bind variables (`:1` / `:name`) via OCIBindBy{Pos,Name}
 *   - OCIStmtPrepare2 + cursor / statement caching
 *   - Typed column accessors (AsInt / AsBytes / AsTimestamp)
 *   - Session pooling (OCISessionPool*) — Oracle connections
 *     are expensive enough that pooling is a real ask
 *   - PL/SQL OUT / INOUT bind variables
 *   - LOB streaming (OCILobRead / OCILobWrite)
 *   - Continuous query notification
 */

#ifndef AMALGAME_DATABASE_ORACLE_H
#define AMALGAME_DATABASE_ORACLE_H

#include "_runtime.h"
#include "Amalgame_Collections.h"

/* The Instant Client SDK installs oci.h directly on the include
 * path the user wires in via AMALGAME_EXTRA_CFLAGS — no need for
 * <oracle/oci.h> nested layouts. */
#if defined(__has_include)
#  if __has_include(<oci.h>)
#    include <oci.h>
#  elif __has_include(<oracle/oci.h>)
#    include <oracle/oci.h>
#  else
#    error "oci.h not found. Install the Oracle Instant Client SDK and set AMALGAME_EXTRA_CFLAGS=-I.../sdk/include."
#  endif
#else
#  include <oci.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Default per-column buffer size for SELECT result binding.
 * 4000 bytes covers VARCHAR2(4000), the maximum non-LOB string
 * column width in standard Oracle. Plus 1 for the NUL terminator
 * OCI writes when we bind via SQLT_STR. */
#ifndef AMORA_COL_BUFSZ
#  define AMORA_COL_BUFSZ 4001
#endif

typedef struct AmalgameOracle {
    OCIEnv*    env;          /* OCI environment; NULL when closed */
    OCIError*  err;          /* shared error handle */
    OCISvcCtx* svc;          /* service context (post-login); NULL when closed */
    char*      last_error;   /* GC-strdup'd, or NULL */
    i64        last_changes; /* rows affected by the last Exec */
    int        tx_open;      /* v0.3: set by Begin, cleared by Commit/Rollback.
                              * When 1, Exec / ExecBind suppress
                              * OCI_COMMIT_ON_SUCCESS so the user's transaction
                              * survives multiple statements. */
} AmalgameOracle;

/* ── Helpers ────────────────────────────────────────── */

static inline code_string _amor_err_dup(const char* msg) {
    if (!msg) return NULL;
    size_t n = strlen(msg);
    char* p = (char*) code_alloc(n + 1);
    memcpy(p, msg, n + 1);
    return p;
}

/* Pull the latest message off OCIErrorGet against the shared
 * error handle. Strips the trailing newline OCI always appends. */
static inline code_string _amor_err_from_handle(OCIError* err) {
    if (!err) return _amor_err_dup("null error handle");
    text buf[1024];
    sb4  code = 0;
    sword rc = OCIErrorGet(err, 1, NULL, &code, buf, (ub4) sizeof(buf),
                           OCI_HTYPE_ERROR);
    if (rc != OCI_SUCCESS) return _amor_err_dup("OCIErrorGet failed");
    size_t n = strlen((const char*) buf);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) n--;
    char* p = (char*) code_alloc(n + 1);
    if (n > 0) memcpy(p, buf, n);
    p[n] = '\0';
    return p;
}

/* GC finalizer — releases OCI handles if the user dropped the
 * AmalgameOracle* without calling Close. Safe to call twice
 * because Close zeroes the pointers. */
static void _amor_finalize(void* obj, void* cd) {
    (void) cd;
    AmalgameOracle* db = (AmalgameOracle*) obj;
    if (!db) return;
    if (db->svc && db->err) {
        OCILogoff(db->svc, db->err);
        db->svc = NULL;
    }
    if (db->err) {
        OCIHandleFree(db->err, OCI_HTYPE_ERROR);
        db->err = NULL;
    }
    if (db->env) {
        OCIHandleFree(db->env, OCI_HTYPE_ENV);
        db->env = NULL;
    }
}

static inline AmalgameOracle* _amor_alloc(void) {
    AmalgameOracle* db =
        (AmalgameOracle*) GC_MALLOC(sizeof(AmalgameOracle));
    db->env          = NULL;
    db->err          = NULL;
    db->svc          = NULL;
    db->last_error   = NULL;
    db->last_changes = 0;
    db->tx_open      = 0;
    GC_register_finalizer(db, _amor_finalize, NULL, NULL, NULL);
    return db;
}

/* ── Lifecycle ──────────────────────────────────────── */

static inline AmalgameOracle* Amalgame_Database_Oracle_Open(
        code_string user, code_string password, code_string connStr) {
    AmalgameOracle* db = _amor_alloc();
    if (!user)     { db->last_error = _amor_err_dup("null user");     return db; }
    if (!password) { db->last_error = _amor_err_dup("null password"); return db; }
    if (!connStr)  { db->last_error = _amor_err_dup("null connStr");  return db; }

    sword rc = OCIEnvCreate(&db->env, OCI_DEFAULT | OCI_OBJECT,
                            NULL, NULL, NULL, NULL, 0, NULL);
    if (rc != OCI_SUCCESS || !db->env) {
        db->last_error = _amor_err_dup("OCIEnvCreate failed");
        return db;
    }
    rc = OCIHandleAlloc(db->env, (void**) &db->err,
                        OCI_HTYPE_ERROR, 0, NULL);
    if (rc != OCI_SUCCESS) {
        db->last_error = _amor_err_dup("OCIHandleAlloc(ERROR) failed");
        OCIHandleFree(db->env, OCI_HTYPE_ENV);
        db->env = NULL;
        return db;
    }

    rc = OCILogon2(db->env, db->err, &db->svc,
                   (const OraText*) user,     (ub4) strlen(user),
                   (const OraText*) password, (ub4) strlen(password),
                   (const OraText*) connStr,  (ub4) strlen(connStr),
                   OCI_DEFAULT);
    if (rc != OCI_SUCCESS) {
        db->last_error = _amor_err_from_handle(db->err);
        OCIHandleFree(db->err, OCI_HTYPE_ERROR);
        OCIHandleFree(db->env, OCI_HTYPE_ENV);
        db->err = NULL;
        db->env = NULL;
        db->svc = NULL;
        return db;
    }
    return db;
}

static inline void Amalgame_Database_Oracle_Close(AmalgameOracle* db) {
    if (!db) return;
    if (db->svc && db->err) {
        OCILogoff(db->svc, db->err);
        db->svc = NULL;
    }
    if (db->err) {
        OCIHandleFree(db->err, OCI_HTYPE_ERROR);
        db->err = NULL;
    }
    if (db->env) {
        OCIHandleFree(db->env, OCI_HTYPE_ENV);
        db->env = NULL;
    }
}

static inline code_bool Amalgame_Database_Oracle_IsOpen(AmalgameOracle* db) {
    return (db && db->svc) ? 1 : 0;
}

static inline code_string Amalgame_Database_Oracle_LastError(AmalgameOracle* db) {
    if (!db || !db->last_error) return (code_string) "";
    return db->last_error;
}

/* ── Exec ───────────────────────────────────────────── */

/* Run a no-result SQL statement (DDL / INSERT / UPDATE / DELETE).
 * Autocommits via OCI_COMMIT_ON_SUCCESS — Oracle defaults to
 * implicit transactions, and exposing transaction control to v1
 * would multiply the surface area without a clear ask. v2 will
 * add Begin/Commit/Rollback. */
static inline code_bool Amalgame_Database_Oracle_Exec(
        AmalgameOracle* db, code_string sql) {
    if (!db || !db->svc || !db->err) {
        if (db) db->last_error = _amor_err_dup("connection not open");
        return 0;
    }
    if (!sql) {
        db->last_error = _amor_err_dup("null sql");
        return 0;
    }

    OCIStmt* stmt = NULL;
    sword rc = OCIHandleAlloc(db->env, (void**) &stmt,
                              OCI_HTYPE_STMT, 0, NULL);
    if (rc != OCI_SUCCESS) {
        db->last_error = _amor_err_dup("OCIHandleAlloc(STMT) failed");
        return 0;
    }

    rc = OCIStmtPrepare(stmt, db->err,
                        (const OraText*) sql, (ub4) strlen(sql),
                        OCI_NTV_SYNTAX, OCI_DEFAULT);
    if (rc != OCI_SUCCESS) {
        db->last_error = _amor_err_from_handle(db->err);
        OCIHandleFree(stmt, OCI_HTYPE_STMT);
        return 0;
    }

    /* iters=1 for non-SELECT; OCI silently ignores it for DDL.
     * Skip OCI_COMMIT_ON_SUCCESS while a user-controlled transaction
     * is open (Begin → Commit/Rollback) so multi-statement
     * transactions survive across Exec calls. Outside a transaction
     * we keep the v1 autocommit-on-success behaviour for backwards
     * compatibility. */
    ub4 exec_mode = db->tx_open ? OCI_DEFAULT : OCI_COMMIT_ON_SUCCESS;
    rc = OCIStmtExecute(db->svc, stmt, db->err,
                        1, 0, NULL, NULL,
                        exec_mode);
    if (rc != OCI_SUCCESS && rc != OCI_SUCCESS_WITH_INFO) {
        db->last_error = _amor_err_from_handle(db->err);
        OCIHandleFree(stmt, OCI_HTYPE_STMT);
        return 0;
    }

    ub4 n = 0;
    if (OCIAttrGet(stmt, OCI_HTYPE_STMT, &n, NULL,
                   OCI_ATTR_ROW_COUNT, db->err) == OCI_SUCCESS) {
        db->last_changes = (i64) n;
    } else {
        db->last_changes = 0;
    }

    OCIHandleFree(stmt, OCI_HTYPE_STMT);
    db->last_error = _amor_err_dup("");
    return 1;
}

/* ── QueryAll ───────────────────────────────────────── */

/* SELECT and return every row as a List<List<string>>. Each cell
 * is bound via OCIDefineByPos with SQLT_STR — OCI does the text
 * conversion for NUMBER, DATE, TIMESTAMP, RAW, ROWID. NULL cells
 * materialise as "" via the indicator vector.
 *
 * On error the outer list is empty and LastError is set. */
static inline AmalgameList* Amalgame_Database_Oracle_QueryAll(
        AmalgameOracle* db, code_string sql) {
    AmalgameList* rows = AmalgameList_new();
    if (!db || !db->svc || !db->err) {
        if (db) db->last_error = _amor_err_dup("connection not open");
        return rows;
    }
    if (!sql) {
        db->last_error = _amor_err_dup("null sql");
        return rows;
    }

    OCIStmt* stmt = NULL;
    sword rc = OCIHandleAlloc(db->env, (void**) &stmt,
                              OCI_HTYPE_STMT, 0, NULL);
    if (rc != OCI_SUCCESS) {
        db->last_error = _amor_err_dup("OCIHandleAlloc(STMT) failed");
        return rows;
    }

    rc = OCIStmtPrepare(stmt, db->err,
                        (const OraText*) sql, (ub4) strlen(sql),
                        OCI_NTV_SYNTAX, OCI_DEFAULT);
    if (rc != OCI_SUCCESS) {
        db->last_error = _amor_err_from_handle(db->err);
        OCIHandleFree(stmt, OCI_HTYPE_STMT);
        return rows;
    }

    /* iters=0 → describe-only execute (don't fetch a row yet,
     * just expose result-set metadata via OCIParamGet). */
    rc = OCIStmtExecute(db->svc, stmt, db->err,
                        0, 0, NULL, NULL, OCI_DEFAULT);
    if (rc != OCI_SUCCESS && rc != OCI_SUCCESS_WITH_INFO) {
        db->last_error = _amor_err_from_handle(db->err);
        OCIHandleFree(stmt, OCI_HTYPE_STMT);
        return rows;
    }

    ub4 ncols = 0;
    if (OCIAttrGet(stmt, OCI_HTYPE_STMT, &ncols, NULL,
                   OCI_ATTR_PARAM_COUNT, db->err) != OCI_SUCCESS) {
        db->last_error = _amor_err_from_handle(db->err);
        OCIHandleFree(stmt, OCI_HTYPE_STMT);
        return rows;
    }
    if (ncols == 0) {
        OCIHandleFree(stmt, OCI_HTYPE_STMT);
        db->last_error = _amor_err_dup("");
        return rows;
    }

    /* Allocate one re-used buffer + indicator + length per column.
     * code_alloc places them on the GC heap so they're collected
     * when this function returns and `rows` no longer references
     * them. */
    char** bufs = (char**) code_alloc(ncols * sizeof(char*));
    sb2*   inds = (sb2*)   code_alloc(ncols * sizeof(sb2));
    ub2*   lens = (ub2*)   code_alloc(ncols * sizeof(ub2));

    for (ub4 j = 0; j < ncols; j++) {
        bufs[j] = (char*) code_alloc(AMORA_COL_BUFSZ);
        inds[j] = 0;
        lens[j] = 0;
        OCIDefine* def = NULL;
        sword drc = OCIDefineByPos(stmt, &def, db->err, (ub4) (j + 1),
                                    bufs[j], (sb4) AMORA_COL_BUFSZ,
                                    SQLT_STR,
                                    &inds[j], &lens[j], NULL,
                                    OCI_DEFAULT);
        if (drc != OCI_SUCCESS) {
            db->last_error = _amor_err_from_handle(db->err);
            OCIHandleFree(stmt, OCI_HTYPE_STMT);
            return rows;
        }
    }

    i64 nrows = 0;
    while (1) {
        sword frc = OCIStmtFetch2(stmt, db->err, 1,
                                   OCI_FETCH_NEXT, 0, OCI_DEFAULT);
        if (frc == OCI_NO_DATA) break;
        if (frc != OCI_SUCCESS && frc != OCI_SUCCESS_WITH_INFO) {
            db->last_error = _amor_err_from_handle(db->err);
            OCIHandleFree(stmt, OCI_HTYPE_STMT);
            return rows;
        }
        AmalgameList* one = AmalgameList_new();
        for (ub4 j = 0; j < ncols; j++) {
            if (inds[j] == -1) {
                /* NULL cell. */
                char* dup = (char*) code_alloc(1);
                dup[0] = '\0';
                AmalgameList_add(one, (void*) dup);
            } else {
                /* OCI null-terminates SQLT_STR; lens[j] is the
                 * byte count incl. NUL on some platforms, sans NUL
                 * on others — strlen is the portable measure. */
                size_t n = strlen(bufs[j]);
                char* dup = (char*) code_alloc(n + 1);
                if (n > 0) memcpy(dup, bufs[j], n);
                dup[n] = '\0';
                AmalgameList_add(one, (void*) dup);
            }
        }
        AmalgameList_add(rows, (void*) one);
        nrows++;
    }

    db->last_changes = nrows;
    OCIHandleFree(stmt, OCI_HTYPE_STMT);
    db->last_error = _amor_err_dup("");
    return rows;
}

/* Rows affected by the last Exec, or row count of the last QueryAll. */
static inline i64 Amalgame_Database_Oracle_Changes(AmalgameOracle* db) {
    return db ? db->last_changes : 0;
}

/* ────────────────────────────────────────────────────────────────
 * v0.3 surface — parameter binding + transactions.
 *
 * Placeholders are Oracle's `:1`, `:2`, … (NOT `?`). Binding goes
 * through OCIBindByPos with SQLT_STR; Oracle applies its standard
 * implicit type conversion at server side (CHAR → NUMBER /
 * DATE / TIMESTAMP / etc.).
 *
 * NULL list entries become SQL NULL via the indicator (-1).
 *
 * Arity mismatches surface as "param count mismatch: got X, sql
 * expects Y" via OCI_ATTR_BIND_COUNT.
 *
 * Transactions: Begin sets `tx_open=1`, which causes Exec/ExecBind
 * to skip OCI_COMMIT_ON_SUCCESS. Commit/Rollback invoke
 * OCITransCommit / OCITransRollback then clear the flag, returning
 * autocommit semantics for subsequent standalone statements.
 * Backwards-compatible: existing code that never calls Begin keeps
 * v1's autocommit behaviour.
 * ──────────────────────────────────────────────────────────────── */

/* Shared bind helper: allocates the statement, prepares, validates
 * arity, binds every param via OCIBindByPos (SQLT_STR). Returns the
 * statement handle on success (caller frees), NULL on failure (with
 * db->last_error set). Caller-allocated lens/inds buffers survive
 * the call so the indicator pointers passed to OCIBindByPos stay
 * valid through OCIStmtExecute. */
static inline OCIStmt* _amor_prepare_bound(
    AmalgameOracle* db, code_string sql, AmalgameList* params,
    sb2** outInds, ub2** outLens)
{
    OCIStmt* stmt = NULL;
    sword rc = OCIHandleAlloc(db->env, (void**) &stmt,
                              OCI_HTYPE_STMT, 0, NULL);
    if (rc != OCI_SUCCESS) {
        db->last_error = _amor_err_dup("OCIHandleAlloc(STMT) failed");
        return NULL;
    }
    rc = OCIStmtPrepare(stmt, db->err,
                        (const OraText*) sql, (ub4) strlen(sql),
                        OCI_NTV_SYNTAX, OCI_DEFAULT);
    if (rc != OCI_SUCCESS) {
        db->last_error = _amor_err_from_handle(db->err);
        OCIHandleFree(stmt, OCI_HTYPE_STMT);
        return NULL;
    }
    int n = params ? (int) AmalgameList_size(params) : 0;
    ub4 bcount = 0;
    if (OCIAttrGet(stmt, OCI_HTYPE_STMT, &bcount, NULL,
                   OCI_ATTR_BIND_COUNT, db->err) != OCI_SUCCESS) {
        db->last_error = _amor_err_from_handle(db->err);
        OCIHandleFree(stmt, OCI_HTYPE_STMT);
        return NULL;
    }
    if ((ub4) n != bcount) {
        char buf[128];
        snprintf(buf, sizeof(buf),
            "param count mismatch: got %d, sql expects %u", n, (unsigned) bcount);
        db->last_error = _amor_err_dup(buf);
        OCIHandleFree(stmt, OCI_HTYPE_STMT);
        return NULL;
    }
    sb2* inds = (n > 0) ? (sb2*) calloc((size_t)n, sizeof(sb2)) : NULL;
    ub2* lens = (n > 0) ? (ub2*) calloc((size_t)n, sizeof(ub2)) : NULL;
    for (int i = 0; i < n; i++) {
        code_string v = (code_string) AmalgameList_get(params, i);
        OCIBind* bnd = NULL;
        if (v) {
            ub4 vlen = (ub4) strlen(v);
            inds[i] = 0;
            lens[i] = (ub2) (vlen + 1);   /* incl. NUL for SQLT_STR */
            rc = OCIBindByPos(stmt, &bnd, db->err, (ub4)(i + 1),
                              (void*) v, (sb4) lens[i],
                              SQLT_STR,
                              &inds[i], NULL, NULL,
                              0, NULL, OCI_DEFAULT);
        } else {
            inds[i] = -1;
            lens[i] = 0;
            rc = OCIBindByPos(stmt, &bnd, db->err, (ub4)(i + 1),
                              NULL, 0, SQLT_STR,
                              &inds[i], NULL, NULL,
                              0, NULL, OCI_DEFAULT);
        }
        if (rc != OCI_SUCCESS) {
            db->last_error = _amor_err_from_handle(db->err);
            free(inds); free(lens);
            OCIHandleFree(stmt, OCI_HTYPE_STMT);
            return NULL;
        }
    }
    *outInds = inds;
    *outLens = lens;
    return stmt;
}

static inline code_bool Amalgame_Database_Oracle_ExecBind(
        AmalgameOracle* db, code_string sql, AmalgameList* params)
{
    if (!db || !db->svc || !db->err) {
        if (db) db->last_error = _amor_err_dup("connection not open");
        return 0;
    }
    if (!sql) { db->last_error = _amor_err_dup("null sql"); return 0; }
    sb2* inds = NULL;
    ub2* lens = NULL;
    OCIStmt* stmt = _amor_prepare_bound(db, sql, params, &inds, &lens);
    if (!stmt) return 0;
    ub4 exec_mode = db->tx_open ? OCI_DEFAULT : OCI_COMMIT_ON_SUCCESS;
    sword rc = OCIStmtExecute(db->svc, stmt, db->err,
                              1, 0, NULL, NULL, exec_mode);
    if (rc != OCI_SUCCESS && rc != OCI_SUCCESS_WITH_INFO) {
        db->last_error = _amor_err_from_handle(db->err);
        OCIHandleFree(stmt, OCI_HTYPE_STMT);
        free(inds); free(lens);
        return 0;
    }
    ub4 nrc = 0;
    if (OCIAttrGet(stmt, OCI_HTYPE_STMT, &nrc, NULL,
                   OCI_ATTR_ROW_COUNT, db->err) == OCI_SUCCESS) {
        db->last_changes = (i64) nrc;
    } else {
        db->last_changes = 0;
    }
    OCIHandleFree(stmt, OCI_HTYPE_STMT);
    free(inds); free(lens);
    db->last_error = _amor_err_dup("");
    return 1;
}

static inline AmalgameList* Amalgame_Database_Oracle_QueryBindAll(
        AmalgameOracle* db, code_string sql, AmalgameList* params)
{
    AmalgameList* rows = AmalgameList_new();
    if (!db || !db->svc || !db->err) {
        if (db) db->last_error = _amor_err_dup("connection not open");
        return rows;
    }
    if (!sql) { db->last_error = _amor_err_dup("null sql"); return rows; }
    sb2* inds = NULL;
    ub2* lens = NULL;
    OCIStmt* stmt = _amor_prepare_bound(db, sql, params, &inds, &lens);
    if (!stmt) return rows;
    /* iters=0 → describe-only execute (same pattern as QueryAll). */
    sword rc = OCIStmtExecute(db->svc, stmt, db->err,
                              0, 0, NULL, NULL, OCI_DEFAULT);
    if (rc != OCI_SUCCESS && rc != OCI_SUCCESS_WITH_INFO) {
        db->last_error = _amor_err_from_handle(db->err);
        OCIHandleFree(stmt, OCI_HTYPE_STMT);
        free(inds); free(lens);
        return rows;
    }
    ub4 ncols = 0;
    if (OCIAttrGet(stmt, OCI_HTYPE_STMT, &ncols, NULL,
                   OCI_ATTR_PARAM_COUNT, db->err) != OCI_SUCCESS) {
        db->last_error = _amor_err_from_handle(db->err);
        OCIHandleFree(stmt, OCI_HTYPE_STMT);
        free(inds); free(lens);
        return rows;
    }
    if (ncols == 0) {
        OCIHandleFree(stmt, OCI_HTYPE_STMT);
        free(inds); free(lens);
        db->last_error = _amor_err_dup("");
        return rows;
    }
    char** bufs   = (char**) code_alloc(ncols * sizeof(char*));
    sb2*   outI   = (sb2*)   code_alloc(ncols * sizeof(sb2));
    ub2*   outL   = (ub2*)   code_alloc(ncols * sizeof(ub2));
    for (ub4 j = 0; j < ncols; j++) {
        bufs[j] = (char*) code_alloc(AMORA_COL_BUFSZ);
        outI[j] = 0;
        outL[j] = 0;
        OCIDefine* def = NULL;
        sword drc = OCIDefineByPos(stmt, &def, db->err, (ub4)(j + 1),
                                    bufs[j], (sb4) AMORA_COL_BUFSZ,
                                    SQLT_STR,
                                    &outI[j], &outL[j], NULL,
                                    OCI_DEFAULT);
        if (drc != OCI_SUCCESS) {
            db->last_error = _amor_err_from_handle(db->err);
            OCIHandleFree(stmt, OCI_HTYPE_STMT);
            free(inds); free(lens);
            return rows;
        }
    }
    i64 nrows = 0;
    while (1) {
        sword frc = OCIStmtFetch2(stmt, db->err, 1,
                                   OCI_FETCH_NEXT, 0, OCI_DEFAULT);
        if (frc == OCI_NO_DATA) break;
        if (frc != OCI_SUCCESS && frc != OCI_SUCCESS_WITH_INFO) {
            db->last_error = _amor_err_from_handle(db->err);
            OCIHandleFree(stmt, OCI_HTYPE_STMT);
            free(inds); free(lens);
            return rows;
        }
        AmalgameList* one = AmalgameList_new();
        for (ub4 j = 0; j < ncols; j++) {
            if (outI[j] == -1) {
                char* dup = (char*) code_alloc(1);
                dup[0] = '\0';
                AmalgameList_add(one, (void*) dup);
            } else {
                size_t nn = strlen(bufs[j]);
                char* dup = (char*) code_alloc(nn + 1);
                if (nn > 0) memcpy(dup, bufs[j], nn);
                dup[nn] = '\0';
                AmalgameList_add(one, (void*) dup);
            }
        }
        AmalgameList_add(rows, (void*) one);
        nrows++;
    }
    db->last_changes = nrows;
    OCIHandleFree(stmt, OCI_HTYPE_STMT);
    free(inds); free(lens);
    db->last_error = _amor_err_dup("");
    return rows;
}

static inline code_bool Amalgame_Database_Oracle_Begin(AmalgameOracle* db) {
    if (!db || !db->svc || !db->err) {
        if (db) db->last_error = _amor_err_dup("connection not open");
        return 0;
    }
    /* Oracle starts an implicit transaction on the first DML
     * statement; the only state we need to track is whether the
     * user has opened a multi-statement transaction. */
    db->tx_open = 1;
    db->last_error = _amor_err_dup("");
    return 1;
}

static inline code_bool Amalgame_Database_Oracle_Commit(AmalgameOracle* db) {
    if (!db || !db->svc || !db->err) {
        if (db) db->last_error = _amor_err_dup("connection not open");
        return 0;
    }
    sword rc = OCITransCommit(db->svc, db->err, OCI_DEFAULT);
    if (rc != OCI_SUCCESS) {
        db->last_error = _amor_err_from_handle(db->err);
        return 0;
    }
    db->tx_open = 0;
    db->last_error = _amor_err_dup("");
    return 1;
}

static inline code_bool Amalgame_Database_Oracle_Rollback(AmalgameOracle* db) {
    if (!db || !db->svc || !db->err) {
        if (db) db->last_error = _amor_err_dup("connection not open");
        return 0;
    }
    sword rc = OCITransRollback(db->svc, db->err, OCI_DEFAULT);
    if (rc != OCI_SUCCESS) {
        db->last_error = _amor_err_from_handle(db->err);
        return 0;
    }
    db->tx_open = 0;
    db->last_error = _amor_err_dup("");
    return 1;
}

/* Oracle server release string via OCIServerVersion — e.g.
 * "Oracle Database 23ai Free Release 23.4.0.24.05 - Develop, Learn,
 * and Run for Free". Empty when the connection is closed. */
static inline code_string Amalgame_Database_Oracle_ServerVersion(AmalgameOracle* db) {
    if (!db || !db->svc || !db->err) return (code_string) "";
    text buf[512];
    sword rc = OCIServerVersion(db->svc, db->err, buf,
                                (ub4) sizeof(buf), OCI_HTYPE_SVCCTX);
    if (rc != OCI_SUCCESS) return (code_string) "";
    return _amor_err_dup((const char*) buf);
}

#endif /* AMALGAME_DATABASE_ORACLE_H */
