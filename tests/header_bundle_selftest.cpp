// Self-test for the header-bundle container format (v1.0.0 #42).
//
// Locks the two properties the feature relies on:
//   1. Round-trip: unpack(pack(X)) == X.
//   2. Determinism (invariant #1): the SAME set of files packs to byte-identical
//      output regardless of insertion order or duplicate writes.
// Plus basic malformed-input rejection. Exit code 0 = pass, 1 = fail.
//
#include "../src/common/header_bundle_format.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using suco::header_bundle::File;
using suco::header_bundle::materialize;
using suco::header_bundle::pack;
using suco::header_bundle::remap_include_flags;
using suco::header_bundle::unpack;

static int g_fails = 0;
#define CHECK(cond, msg)                                              \
    do {                                                             \
        if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_fails; }  \
    } while (0)

int main() {
    std::vector<File> files = {
        {"src/a.h", "content of a\n"},
        {"src/b.h", std::string("\0\1\2binary\xff", 9)},   // binary-safe
        {"deep/nested/c.hpp", ""},                          // empty file
    };

    // 1. Round-trip.
    std::string archive = pack(files);
    std::vector<File> back;
    CHECK(unpack(archive, back), "round-trip unpack returned false");
    CHECK(back.size() == files.size(), "round-trip entry count mismatch");
    // pack() sorts by path, so compare against the sorted expectation.
    CHECK(back.size() == 3 && back[0].path == "deep/nested/c.hpp" &&
              back[1].path == "src/a.h" && back[2].path == "src/b.h",
          "entries not sorted by path");
    CHECK(back.size() == 3 && back[2].content == std::string("\0\1\2binary\xff", 9),
          "binary content not preserved");

    // 2. Determinism: shuffle the input order -> identical bytes.
    std::vector<File> shuffled = {files[2], files[0], files[1]};
    CHECK(pack(shuffled) == archive, "packing is not order-independent");

    // 3. Dedup: a later write of the same path wins, deterministically.
    std::vector<File> dup = {{"x.h", "OLD"}, {"x.h", "NEW"}};
    std::vector<File> dup_out;
    CHECK(unpack(pack(dup), dup_out), "dup unpack failed");
    CHECK(dup_out.size() == 1 && dup_out[0].content == "NEW", "dedup did not keep last write");

    // 4. Malformed input is rejected, not crashed on.
    std::vector<File> junk_out;
    CHECK(!unpack("not-an-archive", junk_out), "bad magic accepted");
    CHECK(!unpack(archive.substr(0, archive.size() - 3), junk_out), "truncated archive accepted");
    CHECK(!unpack("", junk_out), "empty input accepted");

    // 5. Include-path remapping (design §6.3 — the crux of remote preprocessing).
    {
        const std::string root = "/home/u/proj";
        const std::string dest = "/tmp/ws";
        std::vector<std::string> flags = {
            "-I/home/u/proj/include",   // inside root -> remap
            "-I/home/u/proj",           // the root itself -> dest
            "-I/usr/include",           // system -> untouched
            "-DFOO=1",                  // non-include -> untouched
            "-I", "/home/u/proj/gen",   // separated form -> remap the path token
        };
        auto r = remap_include_flags(flags, root, dest);
        CHECK(r.size() == 6, "remap changed token count");
        CHECK(r[0] == "-I/tmp/ws/include", "internal include not remapped");
        CHECK(r[1] == "-I/tmp/ws", "root include not remapped to dest");
        CHECK(r[2] == "-I/usr/include", "system include wrongly remapped");
        CHECK(r[3] == "-DFOO=1", "non-include flag altered");
        CHECK(r[4] == "-I" && r[5] == "/tmp/ws/gen", "separated -I form not remapped");
    }

    // 6. materialize(): unpack to a real dir; content matches; zip-slip refused.
    {
        fs::path ws = fs::temp_directory_path() / "suco_hb_selftest_ws";
        fs::remove_all(ws);
        std::vector<File> bundle = {
            {"inc/a.h", "AAA"},
            {"inc/sub/b.h", "BBB"},
        };
        CHECK(materialize(pack(bundle), ws.string()), "materialize failed");
        auto read = [](const fs::path& p) {
            std::ifstream f(p, std::ios::binary);
            std::ostringstream ss; ss << f.rdbuf(); return ss.str();
        };
        CHECK(read(ws / "inc/a.h") == "AAA", "materialized a.h content wrong");
        CHECK(read(ws / "inc/sub/b.h") == "BBB", "materialized nested b.h content wrong");

        std::vector<File> evil = {{"../escape.h", "PWNED"}};
        CHECK(!materialize(pack(evil), (ws / "guard").string()), "zip-slip path accepted");
        fs::remove_all(ws);
    }

    if (g_fails == 0) { std::printf("header_bundle_selftest: all checks passed\n"); return 0; }
    std::printf("header_bundle_selftest: %d check(s) failed\n", g_fails);
    return 1;
}
