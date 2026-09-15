#pragma once

#include <syrax/syrax.hpp>

#include <string>

// Una migracion es una clase con up() y down(). El schema builder genera el
// DDL correcto para postgres o sqlite, asi que esto no cambia si migras de
// motor.
struct CreateUsersTable : syrax::Migration {
    std::string name() const override { return "001_create_users"; }

    void up(syrax::Schema& schema) override {
        schema.create("users", [](syrax::Blueprint& table) {
            table.id();
            table.string("name");
            table.string("email").unique();
            table.integer("age").defaultTo("0");
            table.timestamps();
        });
    }

    void down(syrax::Schema& schema) override {
        schema.drop("users");
    }
};
