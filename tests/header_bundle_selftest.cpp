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
#include <string>
#include <vector>

using suco::header_bundle::File;
using suco::header_bundle::pack;
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

    if (g_fails == 0) { std::printf("header_bundle_selftest: all checks passed\n"); return 0; }
    std::printf("header_bundle_selftest: %d check(s) failed\n", g_fails);
    return 1;
}
