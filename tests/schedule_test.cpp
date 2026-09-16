// El scheduler. Lo que hay que probar es CUANDO vence cada forma de programar,
// y eso se prueba contra instantes escritos a mano: un test que espere a un
// reloj de verdad para ver si un dailyAt("03:00") dispara tardaria un dia.

#include <syrax/schedule.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <ctime>
#include <string>

using Catch::Matchers::ContainsSubstring;

namespace {

struct Limpieza {
    static constexpr auto name = "Limpieza";
    int                   dias = 30;
};

struct Informe {
    static constexpr auto name = "Informe";
};

// Un instante local concreto, para no depender de la zona del que corre esto.
std::int64_t localStamp(int year, int month, int day, int hour, int minute) {
    std::tm parts{};
    parts.tm_year  = year - 1900;
    parts.tm_mon   = month - 1;
    parts.tm_mday  = day;
    parts.tm_hour  = hour;
    parts.tm_min   = minute;
    parts.tm_sec   = 0;
    parts.tm_isdst = -1;

    return static_cast<std::int64_t>(::mktime(&parts));
}

std::string hourMinuteOf(std::int64_t stamp) {
    std::time_t t = static_cast<std::time_t>(stamp);
    std::tm     parts{};
    ::localtime_r(&t, &parts);

    char buffer[6];
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d", parts.tm_hour, parts.tm_min);
    return std::string{buffer};
}

int dayOf(std::int64_t stamp) {
    std::time_t t = static_cast<std::time_t>(stamp);
    std::tm     parts{};
    ::localtime_r(&t, &parts);
    return parts.tm_mday;
}

// El calculo de vencimiento de la ultima tarea registrada.
syrax::schedule::NextAt ultimaNext() {
    REQUIRE(syrax::schedule::size() > 0);
    return syrax::schedule::detail::entries().back().next;
}

}  // namespace

TEST_CASE("nada se registra hasta que hay trabajo que hacer", "[schedule]") {
    syrax::schedule::reset();

    // Una tarea a medias en la lista solo serviria para que el bucle la
    // recorra sin encolar nada.
    auto pendiente = syrax::schedule::every(std::chrono::minutes{5});
    CHECK(syrax::schedule::size() == 0);

    pendiente.dispatch(Limpieza{});
    CHECK(syrax::schedule::size() == 1);

    syrax::schedule::reset();
}

TEST_CASE("every cuenta desde el instante que se le da", "[schedule]") {
    syrax::schedule::reset();
    syrax::schedule::every(std::chrono::minutes{5}).dispatch(Limpieza{});

    const auto next = ultimaNext();
    const auto base = localStamp(2026, 3, 10, 12, 0);

    CHECK(next(base) == base + 300);

    // Y la siguiente se cuenta desde la anterior, no desde el arranque: sin
    // eso, una tarea lenta iria acumulando retraso en cada vuelta.
    CHECK(next(next(base)) == base + 600);

    syrax::schedule::reset();
}

TEST_CASE("every rechaza un intervalo que no avanza", "[schedule]") {
    // Un intervalo de cero daria un vencimiento igual al instante actual y la
    // tarea se dispararia en cada vuelta del bucle, para siempre.
    CHECK_THROWS_AS(syrax::schedule::every(std::chrono::seconds{0}), std::invalid_argument);
    CHECK_THROWS_AS(syrax::schedule::every(std::chrono::seconds{-5}), std::invalid_argument);
}

TEST_CASE("dailyAt apunta a hoy si la hora no paso, y a mañana si si", "[schedule]") {
    syrax::schedule::reset();
    syrax::schedule::dailyAt("03:00").dispatch(Informe{});

    const auto next = ultimaNext();

    SECTION("antes de la hora, es hoy") {
        const auto vence = next(localStamp(2026, 3, 10, 1, 30));
        CHECK(hourMinuteOf(vence) == "03:00");
        CHECK(dayOf(vence) == 10);
    }

    SECTION("despues de la hora, es mañana") {
        const auto vence = next(localStamp(2026, 3, 10, 9, 0));
        CHECK(hourMinuteOf(vence) == "03:00");
        CHECK(dayOf(vence) == 11);
    }

    SECTION("justo en la hora, es mañana: la de hoy ya se encolo") {
        const auto vence = next(localStamp(2026, 3, 10, 3, 0));
        CHECK(dayOf(vence) == 11);
    }

    syrax::schedule::reset();
}

TEST_CASE("dailyAt sigue cayendo a la misma hora al cruzar el cambio horario", "[schedule]") {
    syrax::schedule::reset();
    syrax::schedule::dailyAt("03:00").dispatch(Informe{});

    const auto next = ultimaNext();

    // Si el siguiente dia se calculara sumando 86400 segundos, en el fin de
    // semana del cambio de hora la tarea se correria una hora y acabaria
    // desplazandose sola. Se comprueba sobre 400 dias seguidos para cruzar los
    // dos cambios de cualquier zona.
    auto momento = localStamp(2026, 1, 1, 12, 0);

    for (int dia = 0; dia < 400; ++dia) {
        momento = next(momento);
        INFO("dia " << dia);
        REQUIRE(hourMinuteOf(momento) == "03:00");
    }

    syrax::schedule::reset();
}

TEST_CASE("dailyAt no acepta cualquier cosa como hora", "[schedule]") {
    // Escribir mal un cron no da un error, da una tarea que corre cuando no
    // toca. Aqui si da un error, que es la mitad de la razon de no tener cron.
    CHECK_THROWS_AS(syrax::schedule::dailyAt("3:00"), std::invalid_argument);
    CHECK_THROWS_AS(syrax::schedule::dailyAt("03-00"), std::invalid_argument);
    CHECK_THROWS_AS(syrax::schedule::dailyAt("25:00"), std::invalid_argument);
    CHECK_THROWS_AS(syrax::schedule::dailyAt("03:60"), std::invalid_argument);
    CHECK_THROWS_AS(syrax::schedule::dailyAt(""), std::invalid_argument);
}

TEST_CASE("hourlyAt cae en el minuto pedido de la hora siguiente", "[schedule]") {
    syrax::schedule::reset();
    syrax::schedule::hourlyAt(30).dispatch(Informe{});

    const auto next = ultimaNext();

    const auto antes = next(localStamp(2026, 3, 10, 12, 10));
    CHECK(hourMinuteOf(antes) == "12:30");

    const auto despues = next(localStamp(2026, 3, 10, 12, 45));
    CHECK(hourMinuteOf(despues) == "13:30");

    syrax::schedule::reset();
}

TEST_CASE("hourlyAt rechaza un minuto que no existe", "[schedule]") {
    CHECK_THROWS_AS(syrax::schedule::hourlyAt(60), std::invalid_argument);
    CHECK_THROWS_AS(syrax::schedule::hourlyAt(-1), std::invalid_argument);
}

TEST_CASE("la etiqueta dice cuando y que", "[schedule]") {
    syrax::schedule::reset();

    syrax::schedule::dailyAt("03:00").dispatch(Informe{});
    syrax::schedule::every(std::chrono::minutes{5}).dispatch(Limpieza{});

    const auto labels = syrax::schedule::labels();
    REQUIRE(labels.size() == 2);

    CHECK_THAT(labels[0], ContainsSubstring("03:00"));
    CHECK_THAT(labels[0], ContainsSubstring("Informe"));
    CHECK_THAT(labels[1], ContainsSubstring("300s"));
    CHECK_THAT(labels[1], ContainsSubstring("Limpieza"));

    syrax::schedule::reset();
}
