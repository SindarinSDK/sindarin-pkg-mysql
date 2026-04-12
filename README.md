# sindarin-pkg-mysql

A MySQL/MariaDB client for the [Sindarin](https://github.com/SindarinSDK/sindarin-compiler) programming language, backed by [libmariadb](https://mariadb.com/kb/en/mariadb-connector-c/). Supports direct SQL execution, row queries with typed accessors, and prepared statements with parameter binding and reuse.

## Installation

Add the package as a dependency in your `sn.yaml`:

```yaml
dependencies:
- name: sindarin-pkg-mysql
  git: git@github.com:SindarinSDK/sindarin-pkg-mysql.git
  branch: main
```

Then run `sn --install` to fetch the package.

## Quick Start

```sindarin
import "sindarin-pkg-mysql/src/mysql"

fn main(): void =>
    var conn: MyConn = MyConn.connect("host=localhost port=3306 user=root password=secret dbname=mydb")

    conn.exec("CREATE TABLE users (id INT AUTO_INCREMENT PRIMARY KEY, name VARCHAR(100), age INT)")
    conn.exec("INSERT INTO users (name, age) VALUES ('Alice', 30)")

    var rows: MyRow[] = conn.query("SELECT * FROM users ORDER BY id")
    print(rows[0].getString("name"))
    print(rows[0].getInt("age"))

    conn.dispose()
```

---

## MyConn

```sindarin
import "sindarin-pkg-mysql/src/mysql"
```

A database connection. The connection string uses space-separated `key=value` pairs.

| Method | Signature | Description |
|--------|-----------|-------------|
| `connect` | `static fn connect(connStr: str): MyConn` | Connect to a MySQL/MariaDB server |
| `exec` | `fn exec(sql: str): void` | Execute SQL with no results (CREATE, INSERT, UPDATE, DELETE) |
| `query` | `fn query(sql: str): MyRow[]` | Execute a SELECT and return all rows |
| `prepare` | `fn prepare(sql: str): MyStmt` | Create a prepared statement |
| `lastError` | `fn lastError(): str` | Last error message from the server |
| `dispose` | `fn dispose(): void` | Close the connection |

**Connection string keys:**

| Key | Default | Description |
|-----|---------|-------------|
| `host` | `localhost` | Server hostname or IP |
| `port` | `3306` | Server port |
| `user` | _(none)_ | Username |
| `password` | _(none)_ | Password |
| `dbname` | _(none)_ | Database name |

```sindarin
var conn: MyConn = MyConn.connect("host=127.0.0.1 port=3306 user=root password=secret dbname=mydb")

conn.exec("CREATE TABLE IF NOT EXISTS items (id INT AUTO_INCREMENT PRIMARY KEY, name VARCHAR(100), price DOUBLE)")
conn.exec("INSERT INTO items (name, price) VALUES ('widget', 9.99)")

conn.dispose()
```

---

## MyRow

A single result row. Column values are accessed by name using typed getters. All values are copied at query time so the row is safe to use after the query returns.

| Method | Signature | Description |
|--------|-----------|-------------|
| `getString` | `fn getString(col: str): str` | Column value as string (`""` for NULL) |
| `getInt` | `fn getInt(col: str): int` | Column value as integer (`0` for NULL) |
| `getFloat` | `fn getFloat(col: str): double` | Column value as float (`0.0` for NULL) |
| `isNull` | `fn isNull(col: str): bool` | True if the column is SQL NULL |
| `columnCount` | `fn columnCount(): int` | Number of columns in this row |
| `columnName` | `fn columnName(index: int): str` | Column name at the given zero-based index |

```sindarin
var rows: MyRow[] = conn.query("SELECT name, price, notes FROM items")

for i: int = 0; i < rows.length; i += 1 =>
    print(rows[i].getString("name"))
    print(rows[i].getFloat("price"))
    if rows[i].isNull("notes") =>
        print("no notes\n")
```

---

## MyStmt

A prepared statement with parameter binding. Parameters use `?` placeholders and are indexed from 1. Bind methods return `self` for chaining. Statements can be reset and re-executed with new bindings.

| Method | Signature | Description |
|--------|-----------|-------------|
| `bindString` | `fn bindString(index: int, value: str): MyStmt` | Bind a string to the given parameter (1-based) |
| `bindInt` | `fn bindInt(index: int, value: int): MyStmt` | Bind an integer to the given parameter (1-based) |
| `bindFloat` | `fn bindFloat(index: int, value: double): MyStmt` | Bind a float to the given parameter (1-based) |
| `bindNull` | `fn bindNull(index: int): MyStmt` | Bind SQL NULL to the given parameter (1-based) |
| `exec` | `fn exec(): void` | Execute with no results |
| `query` | `fn query(): MyRow[]` | Execute and return all result rows |
| `reset` | `fn reset(): void` | Clear all bindings for re-use |
| `dispose` | `fn dispose(): void` | Free statement resources |

```sindarin
var stmt: MyStmt = conn.prepare("INSERT INTO items (name, price) VALUES (?, ?)")

stmt.bindString(1, "gadget").bindFloat(2, 24.99).exec()

stmt.reset()
stmt.bindString(1, "doohickey").bindNull(2).exec()

stmt.dispose()
```

Prepared statements can also return rows:

```sindarin
var sel: MyStmt = conn.prepare("SELECT * FROM items WHERE price < ?")
var rows: MyRow[] = sel.bindFloat(1, 20.0).query()
sel.dispose()
```

---

## Examples

### Basic CRUD

```sindarin
import "sindarin-pkg-mysql/src/mysql"

fn main(): void =>
    var conn: MyConn = MyConn.connect("host=localhost user=root password=secret dbname=mydb")

    conn.exec("CREATE TABLE IF NOT EXISTS products (id INT AUTO_INCREMENT PRIMARY KEY, name VARCHAR(100), stock INT)")
    conn.exec("INSERT INTO products (name, stock) VALUES ('alpha', 10)")
    conn.exec("INSERT INTO products (name, stock) VALUES ('beta', 5)")

    var rows: MyRow[] = conn.query("SELECT * FROM products ORDER BY id")
    for i: int = 0; i < rows.length; i += 1 =>
        print($"{rows[i].getString(\"name\")}: {rows[i].getInt(\"stock\")}\n")

    conn.exec("UPDATE products SET stock = 0 WHERE name = 'beta'")
    conn.exec("DELETE FROM products WHERE stock = 0")

    conn.dispose()
```

### Bulk insert with prepared statement

```sindarin
import "sindarin-pkg-mysql/src/mysql"

fn main(): void =>
    var conn: MyConn = MyConn.connect("host=localhost user=root password=secret dbname=mydb")
    conn.exec("CREATE TABLE IF NOT EXISTS log (msg TEXT, level INT)")

    var stmt: MyStmt = conn.prepare("INSERT INTO log (msg, level) VALUES (?, ?)")

    stmt.bindString(1, "started").bindInt(2, 1).exec().reset()
    stmt.bindString(1, "processing").bindInt(2, 1).exec().reset()
    stmt.bindString(1, "done").bindInt(2, 2).exec()

    stmt.dispose()
    conn.dispose()
```

### Parameterized query returning rows

```sindarin
import "sindarin-pkg-mysql/src/mysql"

fn main(): void =>
    var conn: MyConn = MyConn.connect("host=localhost user=root password=secret dbname=mydb")

    var sel: MyStmt = conn.prepare("SELECT name, age FROM users WHERE age >= ? ORDER BY age")
    var rows: MyRow[] = sel.bindInt(1, 18).query()

    for i: int = 0; i < rows.length; i += 1 =>
        print($"{rows[i].getString(\"name\")} ({rows[i].getInt(\"age\")})\n")

    sel.dispose()
    conn.dispose()
```

---

## Development

```bash
# Install dependencies (required before make test)
sn --install

make test    # Build and run all tests
make clean   # Remove build artifacts
```

Tests require a running MySQL or MariaDB server. Set the following environment variables before running:

| Variable | Default | Description |
|----------|---------|-------------|
| `MYSQL_HOST` | `127.0.0.1` | Server hostname or IP |
| `MYSQL_PORT` | `3306` | Server port |
| `MYSQL_DATABASE` | `testdb` | Database name |
| `MYSQL_USER` | `root` | Username |
| `MYSQL_PASSWORD` | _(none)_ | Password |

## Dependencies

- [sindarin-pkg-libs](https://github.com/SindarinSDK/sindarin-pkg-libs) — provides pre-built `libmariadbclient` static libraries for Linux, macOS, and Windows.
- [sindarin-pkg-sdk](https://github.com/SindarinSDK/sindarin-pkg-sdk) — Sindarin standard library.

## License

This package is licensed under the [MIT License](LICENSE).

### Third-Party Library Notice

This package statically links [MariaDB Connector/C](https://github.com/mariadb-corporation/mariadb-connector-c) (`libmariadb`), which is licensed under **LGPL 2.1 or later**. Under the terms of the LGPL:

- You may use this package in proprietary applications.
- If you modify the MariaDB Connector/C library itself, you must make the modified source available.
- Static linking requires that you provide object files sufficient for relinking on request (LGPL 2.1, Section 6).

The unmodified MariaDB Connector/C source code is available at: https://github.com/mariadb-corporation/mariadb-connector-c

See [sindarin-pkg-libs/THIRD-PARTY.md](https://github.com/SindarinSDK/sindarin-pkg-libs/blob/main/THIRD-PARTY.md) for the complete list of bundled native library licenses.
