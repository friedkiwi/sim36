#include <doctest/doctest.h>

#include "Monitor/CommandLine.h"

using sim36::monitor::CommandLine;
using sim36::monitor::FormatError;

TEST_CASE("lexer: blank and comment lines produce no words")
{
    CHECK(CommandLine::tokenize("").empty());
    CHECK(CommandLine::tokenize("   ").empty());
    CHECK(CommandLine::tokenize("# comment").empty());
    CHECK(CommandLine::tokenize("  ; comment").empty());
}

TEST_CASE("lexer: hash is a whole-line comment only")
{
    auto words = CommandLine::tokenize("lib #RPGLIB 5");
    REQUIRE(words.size() == 3);
    CHECK(words[1] == "#RPGLIB");
}

TEST_CASE("lexer: semicolon starts an inline comment")
{
    auto words = CommandLine::tokenize("wait idle 120 ; the panel is painted");
    REQUIRE(words.size() == 3);
    CHECK(words[2] == "120");
}

TEST_CASE("lexer: quotes group words and keep empty arguments")
{
    auto words = CommandLine::tokenize("attach disk0 'a path/with space.img' rw");
    REQUIRE(words.size() == 4);
    CHECK(words[2] == "a path/with space.img");

    auto empty = CommandLine::tokenize("set machine name \"\"");
    REQUIRE(empty.size() == 4);
    CHECK(empty[3].empty());
}

TEST_CASE("lexer: backslash escapes the next character")
{
    auto words = CommandLine::tokenize("echo a\\ b c\\;d");
    REQUIRE(words.size() == 3);
    CHECK(words[1] == "a b");
    CHECK(words[2] == "c;d");
    auto trailing = CommandLine::tokenize("x\\");
    REQUIRE(trailing.size() == 1);
    CHECK(trailing[0] == "x\\");
}

TEST_CASE("lexer: unterminated quote is an error")
{
    CHECK_THROWS_AS(CommandLine::tokenize("do 'file"), FormatError);
}
