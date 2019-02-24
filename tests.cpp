#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include "kv_store.h"

TEST_CASE("Instantiation") {
    CHECK_NOTHROW(hammurabi::kv_store{1000});
}

TEST_CASE("Setting and getting values") {
    hammurabi::kv_store db{1000};
    int t = 123;
    CHECK_NOTHROW(db.set("foo", t));
    int s = 0;
    CHECK(db.get("foo", s));
    CHECK(s == 123);
}

TEST_CASE("Setting and re-setting values") {
    hammurabi::kv_store db{1000};
    int t = 123;
    CHECK_NOTHROW(db.set("foo", t));
    int s = 0;
    CHECK(db.get("foo", s));
    CHECK(s == t);
    t = 42;
    CHECK_NOTHROW(db.set("foo", t));
    s = 0;
    CHECK(db.get("foo", s));
    CHECK(s == t);
}

TEST_CASE("Check existence") {
    hammurabi::kv_store db{1000};
    CHECK_FALSE(db.exists("foo"));
    db.set("foo", 0);
    CHECK(db.exists("foo"));
    db.remove("foo");
    CHECK_FALSE(db.exists("foo"));
}

TEST_CASE("Remove") {
    hammurabi::kv_store db{1000};
    db.set("foo", 42);
    int s = 0;
    CHECK(db.get("foo", s));
    CHECK(s == 42);
    CHECK_NOTHROW(db.remove("foo"));
    CHECK_FALSE(db.get("foo", s));
}

TEST_CASE("Getting the wrong type") {
    hammurabi::kv_store db{1000};
    db.set("foo", 42);
    std::string s;
    CHECK_FALSE(db.get("foo", s));
}
