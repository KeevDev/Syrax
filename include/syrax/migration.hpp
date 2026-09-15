#pragma once

#include <drogon/orm/DbClient.h>
#include <drogon/orm/Exception.h>

#include <syrax/db.hpp>

#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace syrax {

enum class Dialect { Postgres, Sqlite };

// ---------------------------------------------------------------- Column

// Una columna en construccion. Los modificadores devuelven *this para poder
// encadenar: t.string("email").unique().nullable().
class Column {
public:
    Column(std::string name, std::string pgType, std::string sqliteType)
        : name_{std::move(name)},
          pgType_{std::move(pgType)},
          sqliteType_{std::move(sqliteType)} {}

    Column& nullable(bool v = true) { nullable_ = v; return *this; }
    Column& unique(bool v = true)   { unique_ = v;   return *this; }
    Column& primary(bool v = true)  { primary_ = v;  return *this; }
    Column& index(bool v = true)    { index_ = v;    return *this; }

    // El valor va crudo al SQL: usa comillas simples para texto.
    Column& defaultTo(std::string raw) { default_ = std::move(raw); return *this; }

    Column& references(std::string table, std::string column = "id") {
        refTable_  = std::move(table);
        refColumn_ = std::move(column);
        return *this;
    }
    Column& onDeleteCascade() { onDelete_ = "CASCADE"; return *this; }

    const std::string& name() const { return name_; }
    bool               hasIndex() const { return index_; }

    std::string definition(Dialect dialect) const {
        std::string sql = "\"" + name_ + "\" " +
                          (dialect == Dialect::Postgres ? pgType_ : sqliteType_);

        if (primary_)  sql += " PRIMARY KEY";
        if (!nullable_ && !primary_) sql += " NOT NULL";
        if (unique_ && !primary_)    sql += " UNIQUE";
        if (!default_.empty())       sql += " DEFAULT " + default_;

        if (!refTable_.empty()) {
            sql += " REFERENCES \"" + refTable_ + "\"(\"" + refColumn_ + "\")";
            if (!onDelete_.empty()) sql += " ON DELETE " + onDelete_;
        }
        return sql;
    }

private:
    std::string name_, pgType_, sqliteType_;
    std::string default_, refTable_, refColumn_, onDelete_;
    bool        nullable_ = false;
    bool        unique_   = false;
    bool        primary_  = false;
    bool        index_    = false;
};

// ------------------------------------------------------------- Blueprint

// La forma de una tabla. Cada metodo agrega una columna y devuelve una
// referencia para encadenar modificadores.
class Blueprint {
public:
    explicit Blueprint(std::string table) : table_{std::move(table)} {}

    Column& id(std::string name = "id") {
        return add({std::move(name), "BIGSERIAL", "INTEGER"}).primary();
    }
    Column& string(std::string name, int length = 255) {
        return add({std::move(name), "VARCHAR(" + std::to_string(length) + ")", "TEXT"});
    }
    Column& text(std::string name)        { return add({std::move(name), "TEXT", "TEXT"}); }
    Column& integer(std::string name)     { return add({std::move(name), "INTEGER", "INTEGER"}); }
    Column& bigInteger(std::string name)  { return add({std::move(name), "BIGINT", "INTEGER"}); }
    Column& boolean(std::string name)     { return add({std::move(name), "BOOLEAN", "INTEGER"}); }
    Column& decimal(std::string name)     { return add({std::move(name), "DOUBLE PRECISION", "REAL"}); }
    Column& date(std::string name)        { return add({std::move(name), "DATE", "TEXT"}); }
    Column& json(std::string name)        { return add({std::move(name), "JSONB", "TEXT"}); }

    Column& timestamp(std::string name) {
        return add({std::move(name), "TIMESTAMPTZ", "TEXT"});
    }

    // created_at / updated_at con default del motor.
    void timestamps() {
        timestamp("created_at").defaultTo(pgNow_);
        timestamp("updated_at").defaultTo(pgNow_);
    }

    Column& foreignId(std::string name, std::string refTable, std::string refColumn = "id") {
        return add({std::move(name), "BIGINT", "INTEGER"})
            .references(std::move(refTable), std::move(refColumn));
    }

    const std::string&         table() const { return table_; }
    const std::vector<Column>& columns() const { return columns_; }

private:
    Column& add(Column column) {
        columns_.push_back(std::move(column));
        return columns_.back();
    }

    // Ambos motores aceptan CURRENT_TIMESTAMP; now() es solo de Postgres.
    static constexpr const char* pgNow_ = "CURRENT_TIMESTAMP";

    std::string         table_;
    std::vector<Column> columns_;
};

// ---------------------------------------------------------------- Schema

// Acumula sentencias DDL. No toca la base: solo genera SQL, para que una
// migracion se pueda inspeccionar antes de aplicarla.
class Schema {
public:
    explicit Schema(Dialect dialect) : dialect_{dialect} {}

    void create(const std::string& table, const std::function<void(Blueprint&)>& build) {
        Blueprint blueprint{table};
        build(blueprint);

        std::string sql = "CREATE TABLE IF NOT EXISTS \"" + table + "\" (\n";
        for (std::size_t i = 0; i < blueprint.columns().size(); ++i) {
            sql += "    " + blueprint.columns()[i].definition(dialect_);
            if (i + 1 < blueprint.columns().size()) sql += ",";
            sql += "\n";
        }
        sql += ")";
        statements_.push_back(std::move(sql));

        for (const auto& column : blueprint.columns()) {
            if (!column.hasIndex()) continue;

            statements_.push_back("CREATE INDEX IF NOT EXISTS \"idx_" + table + "_" +
                                  column.name() + "\" ON \"" + table + "\" (\"" +
                                  column.name() + "\")");
        }
    }

    void drop(const std::string& table) {
        statements_.push_back("DROP TABLE IF EXISTS \"" + table + "\"");
    }

    // Escape hatch: cuando el builder no alcanza, SQL crudo.
    void raw(std::string sql) { statements_.push_back(std::move(sql)); }

    Dialect                         dialect() const { return dialect_; }
    const std::vector<std::string>& statements() const { return statements_; }

private:
    Dialect                  dialect_;
    std::vector<std::string> statements_;
};

// ------------------------------------------------------------- Migration

class Migration {
public:
    virtual ~Migration() = default;

    // Identifica la migracion en la tabla de control. Usa un prefijo
    // ordenable: "001_create_users".
    virtual std::string name() const = 0;

    virtual void up(Schema& schema)   = 0;
    virtual void down(Schema& schema) = 0;
};

}  // namespace syrax

namespace syrax {

// -------------------------------------------------------------- Migrator

// Aplica y revierte migraciones, llevando registro en la tabla
// syrax_migrations. Usa SQL sincrono a proposito: esto corre desde el CLI,
// donde bloquear no molesta y el codigo queda mucho mas simple.
class Migrator {
public:
    Migrator() {
        db::loadDotEnv();

        const auto engine = db::env("DB_ENGINE", "postgres");
        sqlite_           = (engine == "sqlite" || engine == "sqlite3");
        dialect_          = sqlite_ ? Dialect::Sqlite : Dialect::Postgres;

        if (sqlite_) {
            client_ = drogon::orm::DbClient::newSqlite3Client(
                "filename=" + db::env("DB_FILE", "app.db"), 1);
        } else {
            client_ = drogon::orm::DbClient::newPgClient(
                "host=" + db::env("DB_HOST", "127.0.0.1") +
                    " port=" + db::env("DB_PORT", "5432") +
                    " dbname=" + db::env("DB_NAME", "app") +
                    " user=" + db::env("DB_USER", "postgres") +
                    " password=" + db::env("DB_PASSWORD", "postgres"),
                1);
        }
    }

    template <typename M>
    Migrator& add() {
        migrations_.push_back(std::make_unique<M>());
        return *this;
    }

    int migrate() {
        ensureControlTable();

        int applied = 0;
        for (const auto& migration : migrations_) {
            if (isApplied(migration->name())) continue;

            Schema schema{dialect_};
            migration->up(schema);

            std::cout << "  aplicando " << migration->name() << std::flush;
            if (!runAll(schema)) return 1;

            client_->execSqlSync("INSERT INTO syrax_migrations (name) VALUES (" +
                                 placeholder(1) + ")",
                                 migration->name());
            std::cout << "  ok\n";
            ++applied;
        }

        if (applied == 0) std::cout << "  nada pendiente\n";
        return 0;
    }

    // Revierte la ultima migracion aplicada.
    int rollback() {
        ensureControlTable();

        const auto result =
            client_->execSqlSync("SELECT name FROM syrax_migrations ORDER BY id DESC LIMIT 1");
        if (result.empty()) {
            std::cout << "  no hay nada que revertir\n";
            return 0;
        }

        const auto last = result.front()["name"].template as<std::string>();

        for (const auto& migration : migrations_) {
            if (migration->name() != last) continue;

            Schema schema{dialect_};
            migration->down(schema);

            std::cout << "  revirtiendo " << last << std::flush;
            if (!runAll(schema)) return 1;

            client_->execSqlSync(
                "DELETE FROM syrax_migrations WHERE name = " + placeholder(1), last);
            std::cout << "  ok\n";
            return 0;
        }

        std::cerr << "\nerror: '" << last
                  << "' esta aplicada pero ya no existe en el codigo\n";
        return 1;
    }

    int status() {
        ensureControlTable();

        for (const auto& migration : migrations_) {
            std::cout << "  [" << (isApplied(migration->name()) ? "x" : " ") << "] "
                      << migration->name() << "\n";
        }
        return 0;
    }

private:
    std::string placeholder(int n) const {
        return sqlite_ ? "?" : "$" + std::to_string(n);
    }

    void ensureControlTable() {
        client_->execSqlSync(
            sqlite_ ? "CREATE TABLE IF NOT EXISTS syrax_migrations ("
                      "id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL UNIQUE, "
                      "applied_at TEXT NOT NULL DEFAULT (datetime('now')))"
                    : "CREATE TABLE IF NOT EXISTS syrax_migrations ("
                      "id BIGSERIAL PRIMARY KEY, name TEXT NOT NULL UNIQUE, "
                      "applied_at TIMESTAMPTZ NOT NULL DEFAULT now())");
    }

    bool isApplied(const std::string& name) {
        const auto result = client_->execSqlSync(
            "SELECT 1 FROM syrax_migrations WHERE name = " + placeholder(1), name);
        return !result.empty();
    }

    bool runAll(const Schema& schema) {
        try {
            for (const auto& sql : schema.statements()) {
                client_->execSqlSync(sql);
            }
            return true;
        } catch (const drogon::orm::DrogonDbException& e) {
            std::cerr << "\nerror: " << e.base().what() << "\n";
            return false;
        }
    }

    drogon::orm::DbClientPtr                client_;
    std::vector<std::unique_ptr<Migration>> migrations_;
    Dialect                                 dialect_ = Dialect::Postgres;
    bool                                    sqlite_  = false;
};

}  // namespace syrax
