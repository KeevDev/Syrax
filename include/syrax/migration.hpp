#pragma once

#include <drogon/orm/DbClient.h>
#include <drogon/orm/Exception.h>

#include <syrax/db.hpp>

#include <functional>
#include <stdexcept>
#include <utility>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace syrax {

// Dialect vive en db.hpp: es el mismo que usa el query builder.

namespace detail {

// Como se escribe un identificador. MySQL reserva las comillas dobles para
// texto, asi que ahi se usan acentos graves; los otros dos siguen el estandar.
inline std::string quote(const std::string& name, Dialect dialect) {
    return dialect == Dialect::Mysql ? "`" + name + "`" : "\"" + name + "\"";
}

// MySQL no acepta IF NOT EXISTS al crear un indice ni IF EXISTS al borrarlo.
inline std::string ifNotExists(Dialect dialect) {
    return dialect == Dialect::Mysql ? "" : "IF NOT EXISTS ";
}

inline std::string ifExists(Dialect dialect) {
    return dialect == Dialect::Mysql ? "" : "IF EXISTS ";
}

}  // namespace detail

// ---------------------------------------------------------------- Column

// Una columna en construccion. Los modificadores devuelven *this para poder
// encadenar: t.string("email").unique().nullable().
class Column {
public:
    Column(std::string name, std::string pgType, std::string mysqlType, std::string sqliteType)
        : name_{std::move(name)},
          pgType_{std::move(pgType)},
          mysqlType_{std::move(mysqlType)},
          sqliteType_{std::move(sqliteType)} {}

    const std::string& type(Dialect dialect) const {
        switch (dialect) {
            case Dialect::Postgres: return pgType_;
            case Dialect::Mysql:    return mysqlType_;
            case Dialect::Sqlite:   return sqliteType_;
        }
        return pgType_;
    }

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

    // Marca la columna como MODIFICACION de una existente en vez de una nueva.
    // Se escribe al final del encadenado, como en Laravel:
    //   t.string("email", 320).nullable().change();
    Column& change(bool v = true) { change_ = v; return *this; }

    // Quitar el DEFAULT es distinto de no declararlo: hay que pedirlo.
    Column& dropDefault(bool v = true) { dropDefault_ = v; return *this; }

    // Postgres exige USING cuando la conversion de tipo no es implicita
    // (texto -> entero, por ejemplo).
    Column& castUsing(std::string expression) { using_ = std::move(expression); return *this; }

    const std::string& name() const { return name_; }
    bool               hasIndex() const { return index_; }
    bool               isChange() const { return change_; }

    // NOT NULL sin DEFAULT no se puede agregar a una tabla que ya tiene filas.
    bool requiresDefaultWhenAdded() const {
        return !change_ && !nullable_ && !primary_ && default_.empty();
    }

    // Las sentencias que convierten la columna existente en lo declarado.
    // Postgres necesita una por aspecto: tipo, nulabilidad y default no se
    // pueden cambiar en un solo ALTER COLUMN.
    std::vector<std::string> alterStatements(const std::string& table, Dialect dialect) const {
        if (dialect == Dialect::Sqlite) {
            throw std::logic_error(
                "syrax: sqlite no soporta ALTER COLUMN. Para cambiar el tipo o las "
                "restricciones de '" + name_ + "' en '" + table +
                "' hay que reconstruir la tabla (crear la nueva, copiar, renombrar) "
                "con Schema::raw(). Es una limitacion del motor, no de syrax.");
        }

        const std::string tabla   = detail::quote(table, dialect);
        const std::string columna = detail::quote(name_, dialect);

        std::vector<std::string> out;

        if (dialect == Dialect::Mysql) {
            // MySQL redefine la columna entera de una vez: el tipo, la
            // nulabilidad y el default van en la misma sentencia.
            std::string modify = "ALTER TABLE " + tabla + " MODIFY COLUMN " + columna + " " +
                                 mysqlType_ + (nullable_ ? " NULL" : " NOT NULL");
            if (!default_.empty()) modify += " DEFAULT " + default_;
            out.push_back(std::move(modify));

            if (default_.empty() && dropDefault_) {
                out.push_back("ALTER TABLE " + tabla + " ALTER COLUMN " + columna +
                              " DROP DEFAULT");
            }
        } else {
            // Postgres necesita una sentencia por aspecto: tipo, nulabilidad y
            // default no se pueden cambiar en un solo ALTER COLUMN.
            const std::string prefix = "ALTER TABLE " + tabla + " ALTER COLUMN " + columna + " ";

            std::string type = prefix + "TYPE " + pgType_;
            if (!using_.empty()) type += " USING " + using_;
            out.push_back(std::move(type));

            out.push_back(prefix + (nullable_ ? "DROP NOT NULL" : "SET NOT NULL"));

            if (!default_.empty())  out.push_back(prefix + "SET DEFAULT " + default_);
            else if (dropDefault_)  out.push_back(prefix + "DROP DEFAULT");
        }

        if (unique_) {
            out.push_back("ALTER TABLE " + tabla + " ADD CONSTRAINT " +
                          detail::quote("uq_" + table + "_" + name_, dialect) + " UNIQUE (" +
                          columna + ")");
        }
        return out;
    }

    std::string definition(Dialect dialect) const {
        std::string sql = detail::quote(name_, dialect) + " " + type(dialect);

        if (primary_)  sql += " PRIMARY KEY";
        if (!nullable_ && !primary_) sql += " NOT NULL";
        if (unique_ && !primary_)    sql += " UNIQUE";
        if (!default_.empty())       sql += " DEFAULT " + default_;

        if (!refTable_.empty()) {
            sql += " REFERENCES " + detail::quote(refTable_, dialect) + "(" +
                   detail::quote(refColumn_, dialect) + ")";
            if (!onDelete_.empty()) sql += " ON DELETE " + onDelete_;
        }
        return sql;
    }

private:
    std::string name_, pgType_, mysqlType_, sqliteType_;
    std::string default_, refTable_, refColumn_, onDelete_;
    std::string using_;
    bool        nullable_    = false;
    bool        unique_      = false;
    bool        primary_     = false;
    bool        index_       = false;
    bool        change_      = false;
    bool        dropDefault_ = false;
};

// ------------------------------------------------------------- Blueprint

// La forma de una tabla. Cada metodo agrega una columna y devuelve una
// referencia para encadenar modificadores.
class Blueprint {
public:
    explicit Blueprint(std::string table) : table_{std::move(table)} {}

    Column& id(std::string name = "id") {
        return add({std::move(name), "BIGSERIAL", "BIGINT AUTO_INCREMENT", "INTEGER"}).primary();
    }
    Column& string(std::string name, int length = 255) {
        const auto varchar = "VARCHAR(" + std::to_string(length) + ")";
        return add({std::move(name), varchar, varchar, "TEXT"});
    }
    Column& text(std::string name)       { return add({std::move(name), "TEXT", "TEXT", "TEXT"}); }
    Column& integer(std::string name)    { return add({std::move(name), "INTEGER", "INT", "INTEGER"}); }
    Column& bigInteger(std::string name) { return add({std::move(name), "BIGINT", "BIGINT", "INTEGER"}); }
    Column& boolean(std::string name)    { return add({std::move(name), "BOOLEAN", "TINYINT(1)", "INTEGER"}); }
    Column& date(std::string name)       { return add({std::move(name), "DATE", "DATE", "TEXT"}); }
    Column& json(std::string name)       { return add({std::move(name), "JSONB", "JSON", "TEXT"}); }

    Column& decimal(std::string name) {
        return add({std::move(name), "DOUBLE PRECISION", "DOUBLE", "REAL"});
    }

    Column& timestamp(std::string name) {
        return add({std::move(name), "TIMESTAMPTZ", "DATETIME", "TEXT"});
    }

    // created_at / updated_at con default del motor.
    //
    // El default cubre las filas que entran por fuera de syrax -un seeder, un
    // INSERT a mano, una importacion-; para las que pasan por save(), el valor
    // lo pone el query builder.
    void timestamps() {
        timestamp("created_at").defaultTo(now_);
        timestamp("updated_at").defaultTo(now_);
    }

    // tenant_id, la columna de multi-tenancy por fila.
    //
    // Con indice por el mismo motivo que deleted_at, y es aun mas claro aqui:
    // una vez que el modelo declara `tenant`, TODA consulta suya lleva
    // "tenant_id = ?" -el framework no deja ejecutar ninguna sin el-, asi que
    // es literalmente la columna mas filtrada de la tabla.
    //
    // No es nullable: una fila sin tenant en una tabla multi-tenant no es de
    // nadie, y no aparece en ninguna consulta. Si aparece, es un bug de carga
    // de datos, y vale mas que lo pare la base.
    Column& tenantId(std::string name = "tenant_id") {
        return string(std::move(name)).index();
    }

    // deleted_at, la columna del borrado logico.
    //
    // Nullable y con indice: una vez que softDeletes esta puesto, TODA consulta
    // del modelo lleva "deleted_at IS NULL", asi que es la columna mas
    // consultada de la tabla y la que menos se piensa en indexar.
    Column& softDeletes(std::string name = "deleted_at") {
        return timestamp(std::move(name)).nullable().index();
    }

    Column& foreignId(std::string name, std::string refTable, std::string refColumn = "id") {
        return add({std::move(name), "BIGINT", "BIGINT", "INTEGER"})
            .references(std::move(refTable), std::move(refColumn));
    }

    // --- solo en modo alter (Schema::table) --------------------------------

    void dropColumn(std::string name) { drops_.push_back(std::move(name)); }

    void renameColumn(std::string from, std::string to) {
        renames_.emplace_back(std::move(from), std::move(to));
    }

    void dropIndex(std::string column) { droppedIndexes_.push_back(std::move(column)); }

    // Quita el UNIQUE que puso .unique().change() o el de la creacion.
    void dropUnique(std::string column) { droppedUniques_.push_back(std::move(column)); }

    // Un CHECK con nombre propio, para poder quitarlo despues.
    //   t.check("age_no_negativa", "age >= 0");
    void check(std::string name, std::string expression) {
        checks_.emplace_back(std::move(name), std::move(expression));
    }

    void dropConstraint(std::string name) { droppedConstraints_.push_back(std::move(name)); }

    const std::string&         table() const { return table_; }
    const std::vector<Column>& columns() const { return columns_; }

    const std::vector<std::string>& drops() const { return drops_; }
    const std::vector<std::string>& droppedIndexes() const { return droppedIndexes_; }
    const std::vector<std::string>& droppedUniques() const { return droppedUniques_; }
    const std::vector<std::string>& droppedConstraints() const { return droppedConstraints_; }

    const std::vector<std::pair<std::string, std::string>>& checks() const { return checks_; }

    const std::vector<std::pair<std::string, std::string>>& renames() const {
        return renames_;
    }

private:
    Column& add(Column column) {
        columns_.push_back(std::move(column));
        return columns_.back();
    }

    // Los tres aceptan CURRENT_TIMESTAMP; now() es solo de Postgres.
    static constexpr const char* now_ = "CURRENT_TIMESTAMP";

    std::string         table_;
    std::vector<Column> columns_;

    std::vector<std::string>                         drops_;
    std::vector<std::string>                         droppedIndexes_;
    std::vector<std::string>                         droppedUniques_;
    std::vector<std::string>                         droppedConstraints_;
    std::vector<std::pair<std::string, std::string>> checks_;
    std::vector<std::pair<std::string, std::string>> renames_;
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

        std::string sql = "CREATE TABLE " + detail::ifNotExists(dialect_) + name(table) + " (\n";
        for (std::size_t i = 0; i < blueprint.columns().size(); ++i) {
            sql += "    " + blueprint.columns()[i].definition(dialect_);
            if (i + 1 < blueprint.columns().size()) sql += ",";
            sql += "\n";
        }
        sql += ")";
        statements_.push_back(std::move(sql));

        addIndexes(table, blueprint);
    }

    void drop(const std::string& table) {
        statements_.push_back("DROP TABLE " + detail::ifExists(dialect_) + name(table));
    }

    // Modifica una tabla existente.
    //
    //   schema.table("users", [](Blueprint& t) {
    //       t.string("phone").nullable();
    //       t.dropColumn("age");
    //       t.renameColumn("name", "full_name");
    //   });
    //
    // Los renames van primero para que puedas renombrar y agregar en la misma
    // migracion sin que choquen los nombres.
    void table(const std::string& table, const std::function<void(Blueprint&)>& build) {
        Blueprint blueprint{table};
        build(blueprint);

        const auto tabla = name(table);

        for (const auto& [from, to] : blueprint.renames()) {
            statements_.push_back("ALTER TABLE " + tabla + " RENAME COLUMN " + name(from) +
                                  " TO " + name(to));
        }

        for (const auto& column : blueprint.columns()) {
            // Una columna marcada con .change() modifica la que ya existe; el
            // resto se agrega.
            if (column.isChange()) {
                for (auto& statement : column.alterStatements(table, dialect_)) {
                    statements_.push_back(std::move(statement));
                }
                continue;
            }

            // Agregar una columna NOT NULL a una tabla con filas falla en los
            // tres motores si no hay DEFAULT. Se detecta aqui para dar un
            // mensaje util en vez de un error de SQL cripto.
            if (column.requiresDefaultWhenAdded()) {
                throw std::logic_error(
                    "syrax: la columna '" + column.name() + "' de la tabla '" + table +
                    "' es NOT NULL sin DEFAULT. Al agregarla a una tabla existente "
                    "usa .nullable() o .defaultTo(...)");
            }

            statements_.push_back("ALTER TABLE " + tabla + " ADD COLUMN " +
                                  column.definition(dialect_));
        }

        for (const auto& [constraint, expression] : blueprint.checks()) {
            statements_.push_back("ALTER TABLE " + tabla + " ADD CONSTRAINT " + name(constraint) +
                                  " CHECK (" + expression + ")");
        }

        for (const auto& column : blueprint.droppedUniques()) {
            dropUnique(table, "uq_" + table + "_" + column);
        }

        for (const auto& constraint : blueprint.droppedConstraints()) {
            dropCheck(table, constraint);
        }

        for (const auto& column : blueprint.drops()) {
            statements_.push_back("ALTER TABLE " + tabla + " DROP COLUMN " + name(column));
        }

        for (const auto& column : blueprint.droppedIndexes()) {
            const auto indice = name("idx_" + table + "_" + column);

            // El indice pertenece a la tabla en MySQL y al esquema en los
            // otros dos, asi que la sentencia no es la misma.
            statements_.push_back(dialect_ == Dialect::Mysql
                                      ? "DROP INDEX " + indice + " ON " + tabla
                                      : "DROP INDEX IF EXISTS " + indice);
        }

        addIndexes(table, blueprint);
    }

    void rename(const std::string& from, const std::string& to) {
        statements_.push_back("ALTER TABLE " + name(from) + " RENAME TO " + name(to));
    }

    // Escape hatch: cuando el builder no alcanza, SQL crudo.
    void raw(std::string sql) { statements_.push_back(std::move(sql)); }

    Dialect                         dialect() const { return dialect_; }
    const std::vector<std::string>& statements() const { return statements_; }

private:
    std::string name(const std::string& identifier) const {
        return detail::quote(identifier, dialect_);
    }

    void addIndexes(const std::string& table, const Blueprint& blueprint) {
        for (const auto& column : blueprint.columns()) {
            if (!column.hasIndex()) continue;

            statements_.push_back("CREATE INDEX " + detail::ifNotExists(dialect_) +
                                  name("idx_" + table + "_" + column.name()) + " ON " +
                                  name(table) + " (" + name(column.name()) + ")");
        }
    }

    // Un UNIQUE en MySQL es un indice y se quita como tal; en Postgres y en
    // sqlite es una constraint con nombre propio.
    void dropUnique(const std::string& table, const std::string& constraint) {
        statements_.push_back(dialect_ == Dialect::Mysql
                                  ? "ALTER TABLE " + name(table) + " DROP INDEX " + name(constraint)
                                  : "ALTER TABLE " + name(table) + " DROP CONSTRAINT IF EXISTS " +
                                        name(constraint));
    }

    void dropCheck(const std::string& table, const std::string& constraint) {
        statements_.push_back(dialect_ == Dialect::Mysql
                                  ? "ALTER TABLE " + name(table) + " DROP CHECK " + name(constraint)
                                  : "ALTER TABLE " + name(table) + " DROP CONSTRAINT IF EXISTS " +
                                        name(constraint));
    }

    Dialect                  dialect_;
    bool                     quiet_ = false;
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
    Migrator() : Migrator(loadedConnection()) {}

    // La conexion se puede pasar a mano para migrar algo que no es la base
    // por defecto: otra conexion del proyecto, o una de pruebas.
    explicit Migrator(const db::Connection& conn) : conn_{conn} {
        dialect_ = db::engineFromName(conn_.engine);

        const auto port = std::to_string(conn_.port != 0 ? conn_.port : db::defaultPort(dialect_));

        switch (dialect_) {
            case Dialect::Postgres:
                client_ = drogon::orm::DbClient::newPgClient(
                    "host=" + conn_.host + " port=" + port + " dbname=" + conn_.database +
                        " user=" + conn_.username + " password=" + conn_.password,
                    1);
                break;

            case Dialect::Mysql:
                client_ = drogon::orm::DbClient::newMysqlClient(
                    "host=" + conn_.host + " port=" + port + " dbname=" + conn_.database +
                        " user=" + conn_.username + " password=" + conn_.password,
                    1);
                break;

            case Dialect::Sqlite:
                client_ = drogon::orm::DbClient::newSqlite3Client("filename=" + conn_.file, 1);
                break;
        }
    }

    // Calla el progreso. Lo usa el kit de tests: una suite que migra antes de
    // cada binario no quiere seis lineas de "aplicando" tapando el fallo que
    // esta buscando. Los errores siguen saliendo por cerr.
    Migrator& quiet(bool value = true) {
        quiet_ = value;
        return *this;
    }

    template <typename M>
    Migrator& add() {
        migrations_.push_back(std::make_unique<M>());
        return *this;
    }

    int migrate() {
        if (!checkConnection()) return 1;
        ensureControlTable();

        int applied = 0;
        for (const auto& migration : migrations_) {
            if (isApplied(migration->name())) continue;

            Schema schema{dialect_};
            migration->up(schema);

            out() << "  aplicando " << migration->name() << std::flush;
            if (!runAll(schema)) return 1;

            client_->execSqlSync("INSERT INTO syrax_migrations (name) VALUES (" +
                                 placeholder(1) + ")",
                                 migration->name());
            out() << "  ok\n";
            ++applied;
        }

        if (applied == 0) out() << "  nada pendiente\n";
        return 0;
    }

    // Revierte la ultima migracion aplicada.
    int rollback() {
        if (!checkConnection()) return 1;
        ensureControlTable();

        const auto result =
            client_->execSqlSync("SELECT name FROM syrax_migrations ORDER BY id DESC LIMIT 1");
        if (result.empty()) {
            out() << "  no hay nada que revertir\n";
            return 0;
        }

        const auto last = result.front()["name"].template as<std::string>();

        for (const auto& migration : migrations_) {
            if (migration->name() != last) continue;

            Schema schema{dialect_};
            migration->down(schema);

            out() << "  revirtiendo " << last << std::flush;
            if (!runAll(schema)) return 1;

            client_->execSqlSync(
                "DELETE FROM syrax_migrations WHERE name = " + placeholder(1), last);
            out() << "  ok\n";
            return 0;
        }

        std::cerr << "\nerror: '" << last
                  << "' esta aplicada pero ya no existe en el codigo\n";
        return 1;
    }

    int status() {
        if (!checkConnection()) return 1;
        ensureControlTable();

        for (const auto& migration : migrations_) {
            std::cout << "  [" << (isApplied(migration->name()) ? "x" : " ") << "] "
                      << migration->name() << "\n";
        }
        return 0;
    }

private:
    // Un sumidero con rdbuf nulo se traga lo que se le escriba. Sale mas
    // limpio que repartir un if por cada traza de progreso.
    std::ostream& out() const {
        static std::ostream sink{nullptr};
        return quiet_ ? sink : std::cout;
    }

    // Drogon reintenta la conexion en bucle y escupe el mismo error cada
    // segundo, sin decir que revisar. Una consulta trivial primero convierte
    // ese muro de ruido en un mensaje accionable.
    bool checkConnection() {
        try {
            client_->execSqlSync("SELECT 1");
            return true;
        } catch (const std::exception& e) {
            std::cerr << "\nerror: no pude conectar a la base de datos.\n\n";

            if (dialect_ == Dialect::Sqlite) {
                std::cerr << "  archivo: " << conn_.file << "\n";
            } else {
                std::cerr << "  motor:    " << db::engineName(dialect_) << "\n"
                          << "  host:     " << conn_.host << ":"
                          << (conn_.port != 0 ? conn_.port : db::defaultPort(dialect_)) << "\n"
                          << "  base:     " << conn_.database << "\n"
                          << "  usuario:  " << conn_.username << "\n\n"
                          << "  Revisa DB_* en tu .env. Si el puerto lo ocupa otro servidor\n"
                          << "  (es comun tener varios locales), cambia DB_PORT y reinicia\n"
                          << "  el contenedor con: docker compose down && docker compose up -d\n";
            }

            std::cerr << "\n  detalle: " << e.what() << "\n\n";
            return false;
        }
    }

    std::string placeholder(int n) const {
        return dialect_ == Dialect::Postgres ? "$" + std::to_string(n) : "?";
    }

    void ensureControlTable() {
        switch (dialect_) {
            case Dialect::Postgres:
                client_->execSqlSync("CREATE TABLE IF NOT EXISTS syrax_migrations ("
                                     "id BIGSERIAL PRIMARY KEY, name TEXT NOT NULL UNIQUE, "
                                     "applied_at TIMESTAMPTZ NOT NULL DEFAULT now())");
                return;

            case Dialect::Mysql:
                client_->execSqlSync("CREATE TABLE IF NOT EXISTS syrax_migrations ("
                                     "id BIGINT AUTO_INCREMENT PRIMARY KEY, "
                                     "name VARCHAR(191) NOT NULL UNIQUE, "
                                     "applied_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP)");
                return;

            case Dialect::Sqlite:
                client_->execSqlSync("CREATE TABLE IF NOT EXISTS syrax_migrations ("
                                     "id INTEGER PRIMARY KEY AUTOINCREMENT, "
                                     "name TEXT NOT NULL UNIQUE, "
                                     "applied_at TEXT NOT NULL DEFAULT (datetime('now')))");
                return;
        }
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

    // El .env se lee antes de construir la conexion, no dentro: asi el
    // constructor que la recibe hecha no toca el entorno.
    static db::Connection loadedConnection() {
        db::loadDotEnv();
        return db::envConnection();
    }

    db::Connection                          conn_;
    drogon::orm::DbClientPtr                client_;
    std::vector<std::unique_ptr<Migration>> migrations_;
    Dialect                                 dialect_ = Dialect::Postgres;
    bool                                    quiet_   = false;
};

}  // namespace syrax
