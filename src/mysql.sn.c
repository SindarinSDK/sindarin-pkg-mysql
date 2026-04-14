/* ==============================================================================
 * sindarin-pkg-mysql/src/mysql.sn.c — MySQL client implementation
 * ==============================================================================
 * Implements MyConn, MyStmt, and MyRow via the MySQL C API (libmariadb).
 *
 * Row data is copied out of the result set into heap arrays immediately so
 * that result handles can be freed. Rows carry parallel col_names/col_values/
 * col_nulls arrays freed by the SnArray elem_release callback.
 *
 * Prepared statements store per-parameter typed values (MyParam) and build
 * MYSQL_BIND arrays at execute time from those values. Result rows from
 * prepared statements are fetched by binding all columns as MYSQL_TYPE_STRING
 * with buffers sized from mysql_stmt_store_result() + UPDATE_MAX_LENGTH.
 * ============================================================================== */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include <mysql/mysql.h>

#ifdef _WIN32
static char *strndup(const char *s, size_t n) {
    size_t len = strnlen(s, n);
    char *dup = (char *)malloc(len + 1);
    if (!dup) return NULL;
    memcpy(dup, s, len);
    dup[len] = '\0';
    return dup;
}
#endif

/* ============================================================================
 * Type Definitions
 * ============================================================================ */

typedef __sn__MyConn  RtMyConn;
typedef __sn__MyStmt  RtMyStmt;
typedef __sn__MyRow   RtMyRow;

/* Per-parameter storage for prepared statement bindings */
typedef struct {
    int        type;      /* 0=unset/null, 1=string, 2=int, 3=double */
    char      *str_val;
    long long  int_val;
    double     dbl_val;
} MyParam;

/* ============================================================================
 * Internal Helpers
 * ============================================================================ */

#define CONN_PTR(c)  ((MYSQL *)(uintptr_t)(c)->conn_ptr)
#define STMT_PTR(s)  ((MYSQL_STMT *)(uintptr_t)(s)->stmt_ptr)
#define STMT_CONN(s) ((MYSQL *)(uintptr_t)(s)->conn_ptr)

/* Parse "host=... port=... user=... password=... dbname=..." into fields.
 * Keys and values are separated by '='; pairs are whitespace-delimited. */
static void parse_conn_str(const char *s,
                            char *host, size_t hlen,
                            char *user, size_t ulen,
                            char *pass, size_t plen,
                            char *db,   size_t dlen,
                            unsigned int *port)
{
    const char *p = s;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        const char *kstart = p;
        while (*p && *p != '=') p++;
        if (!*p) break;
        size_t klen = (size_t)(p - kstart);
        p++; /* skip '=' */

        const char *vstart = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        size_t vlen = (size_t)(p - vstart);

        if (klen == 4 && strncmp(kstart, "host", 4) == 0)
            snprintf(host, hlen, "%.*s", (int)vlen, vstart);
        else if (klen == 4 && strncmp(kstart, "port", 4) == 0) {
            char buf[16] = {0};
            snprintf(buf, sizeof(buf), "%.*s", (int)vlen, vstart);
            *port = (unsigned int)atoi(buf);
        } else if (klen == 4 && strncmp(kstart, "user", 4) == 0)
            snprintf(user, ulen, "%.*s", (int)vlen, vstart);
        else if (klen == 8 && strncmp(kstart, "password", 8) == 0)
            snprintf(pass, plen, "%.*s", (int)vlen, vstart);
        else if (klen == 6 && strncmp(kstart, "dbname", 6) == 0)
            snprintf(db, dlen, "%.*s", (int)vlen, vstart);
    }
}

/* ============================================================================
 * Row Building — copies all column data out of result handles
 * ============================================================================ */

static void cleanup_my_row_elem(void *p)
{
    RtMyRow *row = (RtMyRow *)p;
    int count = (int)row->col_count;

    char **names  = (char **)(uintptr_t)row->col_names;
    char **values = (char **)(uintptr_t)row->col_values;
    bool  *nulls  = (bool  *)(uintptr_t)row->col_nulls;

    if (names) {
        for (int i = 0; i < count; i++) free(names[i]);
        free(names);
    }
    if (values) {
        for (int i = 0; i < count; i++) free(values[i]);
        free(values);
    }
    free(nulls);
}

/* Collect rows from a regular (non-prepared) query result */
static SnArray *collect_rows_from_result(MYSQL_RES *res)
{
    SnArray *arr = sn_array_new(sizeof(RtMyRow), 16);
    arr->elem_tag     = SN_TAG_STRUCT;
    arr->elem_release = cleanup_my_row_elem;

    unsigned int   num_fields = mysql_num_fields(res);
    MYSQL_FIELD   *fields     = mysql_fetch_fields(res);
    MYSQL_ROW      row;
    unsigned long *row_lengths;

    while ((row = mysql_fetch_row(res)) != NULL) {
        row_lengths = mysql_fetch_lengths(res);

        char **names  = (char **)calloc((size_t)num_fields, sizeof(char *));
        char **values = (char **)calloc((size_t)num_fields, sizeof(char *));
        bool  *nulls  = (bool  *)calloc((size_t)num_fields, sizeof(bool));

        if (!names || !values || !nulls) {
            fprintf(stderr, "mysql: collect_rows: allocation failed\n");
            exit(1);
        }

        for (unsigned int i = 0; i < num_fields; i++) {
            names[i] = strdup(fields[i].name ? fields[i].name : "");
            if (row[i] == NULL) {
                nulls[i]  = true;
                values[i] = NULL;
            } else {
                nulls[i]  = false;
                values[i] = strndup(row[i], row_lengths[i]);
            }
        }

        RtMyRow r = {0};
        r.col_count  = (long long)num_fields;
        r.col_names  = (long long)(uintptr_t)names;
        r.col_values = (long long)(uintptr_t)values;
        r.col_nulls  = (long long)(uintptr_t)nulls;
        sn_array_push(arr, &r);
    }

    return arr;
}

/* Collect rows from a prepared statement after mysql_stmt_execute().
 * Binds all result columns as MYSQL_TYPE_STRING using max_length buffers
 * obtained from mysql_stmt_store_result() with UPDATE_MAX_LENGTH. */
static SnArray *collect_rows_from_stmt(MYSQL_STMT *stmt)
{
    SnArray *arr = sn_array_new(sizeof(RtMyRow), 16);
    arr->elem_tag     = SN_TAG_STRUCT;
    arr->elem_release = cleanup_my_row_elem;

    MYSQL_RES *meta = mysql_stmt_result_metadata(stmt);
    if (!meta) return arr; /* no result set (e.g. INSERT) */

    unsigned int  num_fields = mysql_num_fields(meta);
    MYSQL_FIELD  *fields     = mysql_fetch_fields(meta);

    /* Tell MySQL to populate max_length for each column */
    my_bool update_max_len = 1;
    mysql_stmt_attr_set(stmt, STMT_ATTR_UPDATE_MAX_LENGTH, &update_max_len);

    if (mysql_stmt_store_result(stmt)) {
        fprintf(stderr, "mysql: stmt store_result: %s\n", mysql_stmt_error(stmt));
        mysql_free_result(meta);
        return arr;
    }

    /* Allocate one MYSQL_BIND per column, all as strings */
    MYSQL_BIND    *res_binds  = (MYSQL_BIND *)   calloc(num_fields, sizeof(MYSQL_BIND));
    char         **bufs       = (char **)         calloc(num_fields, sizeof(char *));
    unsigned long *lengths    = (unsigned long *)  calloc(num_fields, sizeof(unsigned long));
    char          *is_nulls   = (char *)           calloc(num_fields, sizeof(char));

    if (!res_binds || !bufs || !lengths || !is_nulls) {
        fprintf(stderr, "mysql: collect_rows_from_stmt: allocation failed\n");
        exit(1);
    }

    for (unsigned int i = 0; i < num_fields; i++) {
        unsigned long buf_len = fields[i].max_length + 1;
        if (buf_len < 64) buf_len = 64;
        bufs[i] = (char *)calloc(1, buf_len);
        if (!bufs[i]) {
            fprintf(stderr, "mysql: collect_rows_from_stmt: buf allocation failed\n");
            exit(1);
        }
        res_binds[i].buffer_type   = MYSQL_TYPE_STRING;
        res_binds[i].buffer        = bufs[i];
        res_binds[i].buffer_length = buf_len;
        res_binds[i].length        = &lengths[i];
        res_binds[i].is_null       = (my_bool *)&is_nulls[i];
    }

    if (mysql_stmt_bind_result(stmt, res_binds)) {
        fprintf(stderr, "mysql: stmt bind_result: %s\n", mysql_stmt_error(stmt));
        goto cleanup;
    }

    /* Copy field names (metadata freed at end) */
    char **col_names = (char **)calloc(num_fields, sizeof(char *));
    if (!col_names) {
        fprintf(stderr, "mysql: collect_rows_from_stmt: col_names allocation failed\n");
        exit(1);
    }
    for (unsigned int i = 0; i < num_fields; i++)
        col_names[i] = strdup(fields[i].name ? fields[i].name : "");

    int rc;
    while ((rc = mysql_stmt_fetch(stmt)) == 0 || rc == MYSQL_DATA_TRUNCATED) {
        char **names  = (char **)calloc(num_fields, sizeof(char *));
        char **values = (char **)calloc(num_fields, sizeof(char *));
        bool  *nulls  = (bool  *)calloc(num_fields, sizeof(bool));

        if (!names || !values || !nulls) {
            fprintf(stderr, "mysql: collect_rows_from_stmt: row allocation failed\n");
            exit(1);
        }

        for (unsigned int i = 0; i < num_fields; i++) {
            names[i] = strdup(col_names[i]);
            nulls[i] = (bool)is_nulls[i];
            values[i] = nulls[i] ? NULL : strdup(bufs[i] ? bufs[i] : "");
        }

        RtMyRow row = {0};
        row.col_count  = (long long)num_fields;
        row.col_names  = (long long)(uintptr_t)names;
        row.col_values = (long long)(uintptr_t)values;
        row.col_nulls  = (long long)(uintptr_t)nulls;
        sn_array_push(arr, &row);
    }

    for (unsigned int i = 0; i < num_fields; i++) free(col_names[i]);
    free(col_names);

cleanup:
    for (unsigned int i = 0; i < num_fields; i++) free(bufs[i]);
    free(bufs);
    free(lengths);
    free(is_nulls);
    free(res_binds);
    mysql_free_result(meta);

    return arr;
}

/* ============================================================================
 * MyRow Accessors
 * ============================================================================ */

static int find_col(RtMyRow *row, const char *col)
{
    char **names = (char **)(uintptr_t)row->col_names;
    int count = (int)row->col_count;
    for (int i = 0; i < count; i++) {
        if (names[i] && strcmp(names[i], col) == 0)
            return i;
    }
    return -1;
}

char *sn_my_row_get_string(__sn__MyRow *row, char *col)
{
    if (!row || !col) return strdup("");
    int idx = find_col(row, col);
    if (idx < 0) return strdup("");
    bool *nulls = (bool *)(uintptr_t)row->col_nulls;
    if (nulls[idx]) return strdup("");
    char **values = (char **)(uintptr_t)row->col_values;
    return strdup(values[idx] ? values[idx] : "");
}

long long sn_my_row_get_int(__sn__MyRow *row, char *col)
{
    if (!row || !col) return 0;
    int idx = find_col(row, col);
    if (idx < 0) return 0;
    bool *nulls = (bool *)(uintptr_t)row->col_nulls;
    if (nulls[idx]) return 0;
    char **values = (char **)(uintptr_t)row->col_values;
    if (!values[idx]) return 0;
    return (long long)strtoll(values[idx], NULL, 10);
}

double sn_my_row_get_float(__sn__MyRow *row, char *col)
{
    if (!row || !col) return 0.0;
    int idx = find_col(row, col);
    if (idx < 0) return 0.0;
    bool *nulls = (bool *)(uintptr_t)row->col_nulls;
    if (nulls[idx]) return 0.0;
    char **values = (char **)(uintptr_t)row->col_values;
    if (!values[idx]) return 0.0;
    return strtod(values[idx], NULL);
}

bool sn_my_row_is_null(__sn__MyRow *row, char *col)
{
    if (!row || !col) return true;
    int idx = find_col(row, col);
    if (idx < 0) return true;
    bool *nulls = (bool *)(uintptr_t)row->col_nulls;
    return nulls[idx];
}

long long sn_my_row_column_count(__sn__MyRow *row)
{
    if (!row) return 0;
    return row->col_count;
}

char *sn_my_row_column_name(__sn__MyRow *row, long long index)
{
    if (!row || index < 0 || index >= row->col_count) return strdup("");
    char **names = (char **)(uintptr_t)row->col_names;
    return strdup(names[index] ? names[index] : "");
}

/* ============================================================================
 * MyConn
 * ============================================================================ */

RtMyConn *sn_my_conn_connect(char *conn_str)
{
    if (!conn_str) {
        fprintf(stderr, "MyConn.connect: connStr is NULL\n");
        exit(1);
    }

    char         host[256]  = "localhost";
    char         user[256]  = "";
    char         pass[256]  = "";
    char         db[256]    = "";
    unsigned int port       = 3306;

    parse_conn_str(conn_str, host, sizeof(host), user, sizeof(user),
                   pass, sizeof(pass), db, sizeof(db), &port);

    MYSQL *conn = mysql_init(NULL);
    if (!conn) {
        fprintf(stderr, "MyConn.connect: mysql_init failed\n");
        exit(1);
    }

    if (!mysql_real_connect(conn, host, user, pass,
                            db[0] ? db : NULL, port, NULL, 0)) {
        fprintf(stderr, "MyConn.connect: %s\n", mysql_error(conn));
        mysql_close(conn);
        exit(1);
    }

    RtMyConn *c = __sn__MyConn__new();
    c->conn_ptr = (long long)(uintptr_t)conn;
    return c;
}

void sn_my_conn_exec(RtMyConn *c, char *sql)
{
    if (!c || !sql) return;
    if (mysql_query(CONN_PTR(c), sql)) {
        fprintf(stderr, "mysql: exec: %s\n", mysql_error(CONN_PTR(c)));
        exit(1);
    }
    /* Consume any result to avoid "Commands out of sync" on next query */
    MYSQL_RES *res = mysql_store_result(CONN_PTR(c));
    if (res) mysql_free_result(res);
}

SnArray *sn_my_conn_query(RtMyConn *c, char *sql)
{
    if (!c || !sql) return sn_array_new(sizeof(RtMyRow), 0);
    if (mysql_query(CONN_PTR(c), sql)) {
        fprintf(stderr, "mysql: query: %s\n", mysql_error(CONN_PTR(c)));
        exit(1);
    }
    MYSQL_RES *res = mysql_store_result(CONN_PTR(c));
    if (!res) return sn_array_new(sizeof(RtMyRow), 0);
    SnArray *arr = collect_rows_from_result(res);
    mysql_free_result(res);
    return arr;
}

RtMyStmt *sn_my_conn_prepare(RtMyConn *c, char *sql)
{
    if (!c || !sql) {
        fprintf(stderr, "MyConn.prepare: NULL argument\n");
        exit(1);
    }

    MYSQL_STMT *stmt = mysql_stmt_init(CONN_PTR(c));
    if (!stmt) {
        fprintf(stderr, "MyConn.prepare: mysql_stmt_init failed\n");
        exit(1);
    }

    if (mysql_stmt_prepare(stmt, sql, (unsigned long)strlen(sql))) {
        fprintf(stderr, "MyConn.prepare: %s\n", mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        exit(1);
    }

    unsigned long param_count = mysql_stmt_param_count(stmt);

    MyParam *params = NULL;
    if (param_count > 0) {
        params = (MyParam *)calloc((size_t)param_count, sizeof(MyParam));
        if (!params) {
            fprintf(stderr, "MyConn.prepare: allocation failed\n");
            mysql_stmt_close(stmt);
            exit(1);
        }
    }

    RtMyStmt *s = __sn__MyStmt__new();
    s->conn_ptr    = (long long)(uintptr_t)CONN_PTR(c);
    s->stmt_ptr    = (long long)(uintptr_t)stmt;
    s->params      = (long long)(uintptr_t)params;
    s->param_count = (long long)param_count;
    return s;
}

char *sn_my_conn_last_error(RtMyConn *c)
{
    if (!c) return strdup("");
    const char *msg = mysql_error(CONN_PTR(c));
    return strdup(msg ? msg : "");
}

void sn_my_conn_dispose(RtMyConn *c)
{
    if (!c) return;
    mysql_close(CONN_PTR(c));
    c->conn_ptr = 0;
}

/* ============================================================================
 * MyStmt — parameter binding and execution
 * ============================================================================ */

static void check_param_index(RtMyStmt *s, long long index, const char *fn)
{
    if (!s || index < 1 || index > s->param_count) {
        fprintf(stderr, "MyStmt.%s: index %lld out of range (1..%lld)\n",
                fn, index, s ? s->param_count : 0LL);
        exit(1);
    }
}

void sn_my_stmt_bind_string(RtMyStmt *s, long long index, char *value)
{
    check_param_index(s, index, "bindString");
    MyParam *params = (MyParam *)(uintptr_t)s->params;
    int i = (int)index - 1;
    free(params[i].str_val);
    params[i].type    = value ? 1 : 0;
    params[i].str_val = value ? strdup(value) : NULL;
}

void sn_my_stmt_bind_int(RtMyStmt *s, long long index, long long value)
{
    check_param_index(s, index, "bindInt");
    MyParam *params = (MyParam *)(uintptr_t)s->params;
    int i = (int)index - 1;
    free(params[i].str_val);
    params[i].str_val = NULL;
    params[i].type    = 2;
    params[i].int_val = value;
}

void sn_my_stmt_bind_float(RtMyStmt *s, long long index, double value)
{
    check_param_index(s, index, "bindFloat");
    MyParam *params = (MyParam *)(uintptr_t)s->params;
    int i = (int)index - 1;
    free(params[i].str_val);
    params[i].str_val = NULL;
    params[i].type    = 3;
    params[i].dbl_val = value;
}

void sn_my_stmt_bind_null(RtMyStmt *s, long long index)
{
    check_param_index(s, index, "bindNull");
    MyParam *params = (MyParam *)(uintptr_t)s->params;
    int i = (int)index - 1;
    free(params[i].str_val);
    params[i].str_val = NULL;
    params[i].type    = 0;
}

/* Build MYSQL_BIND array from MyParam values, bind, and execute */
static void stmt_bind_and_execute(RtMyStmt *s)
{
    int       n      = (int)s->param_count;
    MyParam  *params = (MyParam *)(uintptr_t)s->params;

    MYSQL_BIND *binds = (MYSQL_BIND *)calloc((size_t)(n > 0 ? n : 1), sizeof(MYSQL_BIND));
    char       *nulls = (char *)      calloc((size_t)(n > 0 ? n : 1), sizeof(char));

    if (!binds || !nulls) {
        fprintf(stderr, "MyStmt: execute: allocation failed\n");
        exit(1);
    }

    for (int i = 0; i < n; i++) {
        nulls[i] = (params[i].type == 0) ? 1 : 0;
        binds[i].is_null = (my_bool *)&nulls[i];

        switch (params[i].type) {
        case 1: /* string */
            binds[i].buffer_type   = MYSQL_TYPE_STRING;
            binds[i].buffer        = params[i].str_val;
            binds[i].buffer_length = params[i].str_val
                                   ? (unsigned long)strlen(params[i].str_val) : 0;
            break;
        case 2: /* int */
            binds[i].buffer_type = MYSQL_TYPE_LONGLONG;
            binds[i].buffer      = &params[i].int_val;
            break;
        case 3: /* double */
            binds[i].buffer_type = MYSQL_TYPE_DOUBLE;
            binds[i].buffer      = &params[i].dbl_val;
            break;
        default: /* null */
            binds[i].buffer_type = MYSQL_TYPE_NULL;
            break;
        }
    }

    if (n > 0 && mysql_stmt_bind_param(STMT_PTR(s), binds)) {
        fprintf(stderr, "MyStmt: bind_param: %s\n", mysql_stmt_error(STMT_PTR(s)));
        free(binds);
        free(nulls);
        exit(1);
    }

    if (mysql_stmt_execute(STMT_PTR(s))) {
        fprintf(stderr, "MyStmt: execute: %s\n", mysql_stmt_error(STMT_PTR(s)));
        free(binds);
        free(nulls);
        exit(1);
    }

    free(binds);
    free(nulls);
}

void sn_my_stmt_exec(RtMyStmt *s)
{
    if (!s) return;
    stmt_bind_and_execute(s);
}

SnArray *sn_my_stmt_query(RtMyStmt *s)
{
    if (!s) return sn_array_new(sizeof(RtMyRow), 0);
    stmt_bind_and_execute(s);
    return collect_rows_from_stmt(STMT_PTR(s));
}

void sn_my_stmt_reset(RtMyStmt *s)
{
    if (!s) return;
    int      n      = (int)s->param_count;
    MyParam *params = (MyParam *)(uintptr_t)s->params;
    for (int i = 0; i < n; i++) {
        free(params[i].str_val);
        params[i] = (MyParam){0};
    }
    mysql_stmt_reset(STMT_PTR(s));
}

void sn_my_stmt_dispose(RtMyStmt *s)
{
    if (!s) return;
    int      n      = (int)s->param_count;
    MyParam *params = (MyParam *)(uintptr_t)s->params;
    if (params) {
        for (int i = 0; i < n; i++) free(params[i].str_val);
        free(params);
    }
    mysql_stmt_close(STMT_PTR(s));
    s->stmt_ptr    = 0;
    s->conn_ptr    = 0;
    s->params      = 0;
    s->param_count = 0;
}
