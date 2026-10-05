/* PearPC -- limited CHRP boot-script recognition (GPL-2.0-or-later). */
#include "chrpboot.h"
#include <cctype>
#include <cstring>

namespace {
struct Token {
    std::string text;
    bool quoted = false;
};

bool nextToken(const char *&p, Token &token)
{
    for (;;) {
        while (std::isspace(static_cast<unsigned char>(*p))) ++p;
        if (!*p) return false;
        // Forth line comments and parenthesized comments are standalone words.
        if (*p == '\\' && (!p[1] || std::isspace(static_cast<unsigned char>(p[1])))) {
            while (*p && *p != '\n' && *p != '\r') ++p;
        } else if (*p == '(' && std::isspace(static_cast<unsigned char>(p[1]))) {
            const char *end = std::strchr(p + 1, ')');
            if (!end) return false;
            p = end + 1;
        } else {
            break;
        }
    }
    token.quoted = (*p == '"');
    if (token.quoted) {
        const char *start = ++p;
        if (*p == ' ') start = ++p; // Forth's string-opening delimiter
        const char *end = std::strchr(p, '"');
        if (!end) return false;
        token.text.assign(start, end);
        p = end + 1;
    } else {
        const char *start = p;
        while (*p && !std::isspace(static_cast<unsigned char>(*p))) ++p;
        token.text.assign(start, p);
    }
    return true;
}

bool splitTarget(const std::string &target, std::string &path, std::string &args)
{
    size_t first = target.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return false;
    size_t end = target.find_first_of(" \t\r\n", first);
    std::string candidate = target.substr(first, end - first);
    if (candidate.size() > 1023) return false;
    path = candidate;
    args.clear();
    if (end != std::string::npos) {
        first = target.find_first_not_of(" \t\r\n", end);
        if (first != std::string::npos) {
            args = target.substr(first, target.find_last_not_of(" \t\r\n") - first + 1);
        }
    }
    return true;
}
}

bool prom_parse_chrp_boot_script(const char *script, std::string &path, std::string &args)
{
    path.clear();
    args.clear();
    const char *p = script;
    Token token, previous;
    std::string yabootTarget;
    while (nextToken(p, token)) {
        if (!token.quoted && token.text == ":") {
            Token name, body, last;
            if (!nextToken(p, name) || name.quoted) return false;
            std::string target;
            bool closed = false;
            while (nextToken(p, body)) {
                if (!body.quoted && body.text == ";") { closed = true; break; }
                if (!body.quoted && body.text == "$boot" && last.quoted) target = last.text;
                last = body;
            }
            if (!closed) return false;
            // Only follow the known yaboot entry point, not bootcd/bootof.
            if (name.text == "bootyaboot") yabootTarget = target;
            previous = Token();
            continue;
        }
        if (!token.quoted && token.text == "boot") {
            Token destination;
            if (!nextToken(p, destination) || destination.quoted) return false;
            const char *end = p;
            while (*end && *end != '\n' && *end != '\r') ++end;
            return splitTarget(destination.text + std::string(p, end), path, args);
        }
        if (!token.quoted && token.text == "$boot" && previous.quoted) {
            return splitTarget(previous.text, path, args);
        }
        if (!token.quoted && token.text == "bootyaboot" && !yabootTarget.empty()) {
            return splitTarget(yabootTarget, path, args);
        }
        previous = token;
    }
    return false;
}
