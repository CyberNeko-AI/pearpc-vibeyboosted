#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include "io/prom/chrpboot.h"

static unsigned checks;
static void expect(const std::string &script, const char *wantPath, const char *wantArgs = "")
{
    std::string path = "stale", args = "stale";
    bool found = prom_parse_chrp_boot_script(script.c_str(), path, args);
    ++checks;
    if (found != (wantPath != nullptr) || (found && (path != wantPath || args != wantArgs)) ||
        (!found && (!path.empty() || !args.empty()))) {
        fprintf(stderr, "FAIL case %u: found=%d path='%s' args='%s'\n", checks, found, path.c_str(), args.c_str());
        exit(1);
    }
}
int main(int argc, char **argv)
{
    // The order of definitions and the word 'boot' inside a name must not
    // redirect the Linux entry to the CD or to a message's opening quote.
    const std::string definitions =
        ": bootcd \" cd:,\\\\:tbxi\" $boot ;\n"
        ": bootyaboot \" Loading second stage...\" .printf 100 ms "
        "load-base release-load-area \" hd:2,\\\\yaboot\" $boot ;\n"
        ": bootof quit ;\n";
    expect(definitions + "0 interactive @ = if\n bootyaboot\nthen\n", "hd:2,\\\\yaboot");
    expect(definitions + "bootyaboot\n", "hd:2,\\\\yaboot");
    expect(definitions, nullptr); // A definition alone is not a boot request.
    expect(": notboot \" hd:2,wrong\" $boot ;", nullptr);
    expect("boot hd:2,\\yaboot\n", "hd:2,\\yaboot");
    expect("\tboot\tcd:2,\\install -v root=/dev/hda3\r\n", "cd:2,\\install", "-v root=/dev/hda3");
    expect("\" hd:2,\\yaboot arg=1\" $boot", "hd:2,\\yaboot", "arg=1");
    expect("bootyaboot hd:2,wrong", nullptr);
    expect("reboot hd:2,wrong", nullptr);
    expect("\" boot hd:2,wrong\" .printf\nboot hd:2,right", "hd:2,right");
    expect("\\ boot hd:2,wrong\nboot hd:2,right", "hd:2,right");
    expect("( boot hd:2,wrong ) boot hd:2,right", "hd:2,right");
    expect("", nullptr);
    expect("boot", nullptr);
    expect("\" unterminated $boot", nullptr);
    expect(": bootyaboot \" hd:2,wrong\" $boot", nullptr);
    expect("boot " + std::string(1024, 'a'), nullptr);
    expect("\" \" $boot", nullptr);
    if (argc >= 2) {
        std::ifstream in(argv[1]);
        if (!in) return 2;
        std::ostringstream contents;
        contents << in.rdbuf();
        std::string file = contents.str();
        size_t start = file.find("<BOOT-SCRIPT>"), end = file.find("</BOOT-SCRIPT>");
        if (start == std::string::npos || end == std::string::npos || end < start + 13) return 2;
        expect(file.substr(start + 13, end - start - 13), argc > 2 ? argv[2] : "hd:2,\\\\yaboot");
    }
    printf("PASS: %u CHRP boot-script checks\n", checks);
}
