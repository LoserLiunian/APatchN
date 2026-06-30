#pragma once
// Minimal Java .properties reader. Replaces the `java-properties` crate for
// reading module.prop (key=value, '#'/'!' comments, line continuation,
// backslash escapes incl. \uXXXX). Content is treated as UTF-8.
#include <map>
#include <string>

namespace apd::properties {

std::map<std::string, std::string> parse(const std::string &content);
std::map<std::string, std::string> parse_file(const std::string &path);

} // namespace apd::properties
