#include "helpers.hpp"

#include <algorithm>

namespace fs = std::filesystem;
using namespace backup;

namespace
{
std::vector<std::string> sortedPaths(std::vector<std::string> paths)
{
    std::sort(paths.begin(), paths.end());
    return paths;
}

// Backs up with a selection, then proves that the archive, its preview and the
// restored tree contain exactly the expected entries with their metadata.
void verifySelection(const fs::path& workspace, const fs::path& source,
                     const std::string& name,
                     const std::vector<std::string>& includes,
                     const std::vector<std::string>& expectedPaths, int number)
{
    const std::vector<std::string> expected = sortedPaths(expectedPaths);
    normalizeAtimes(source, expected);
    const TreeSnapshot snapshot = snapshotKnownTree(source, expected);

    const fs::path archive =
        workspace / ("selection-" + std::to_string(number) + ".bak");
    BackupOptions options;
    options.includes = includes;
    const auto created =
        BackupEngine::create(source.string(), archive.string(), options);
    check(created.success, name + ": backup failed: " + created.message);
    check(created.entryCount == expected.size(),
          name + ": archived entry count differs from the expectation");

    const auto preview = BackupEngine::preview(
        archive.string(), (workspace / "preview").string());
    check(preview.entries.size() == expected.size(),
          name + ": preview entry count differs from the expectation");

    const fs::path destination =
        workspace / ("selection-out-" + std::to_string(number));
    const auto restored =
        BackupEngine::restore(archive.string(), destination.string(), {});
    check(restored.success, name + ": restore failed: " + restored.message);
    compareCompleteTree(destination / source.filename(), expected, snapshot);
}

// A selection that cannot be honoured must fail before anything is published.
void expectSelectionFailure(const fs::path& workspace, const fs::path& source,
                            const std::vector<std::string>& includes,
                            const std::string& context, int number)
{
    const fs::path archive =
        workspace / ("selection-rejected-" + std::to_string(number) + ".bak");
    BackupOptions options;
    options.includes = includes;
    const auto created =
        BackupEngine::create(source.string(), archive.string(), options);
    check(!created.success, "invalid selection was accepted: " + context);
    check(!fs::exists(archive),
          "rejected selection left an archive behind: " + context);
    check(created.message.find("selected") != std::string::npos,
          "unexpected selection error for " + context + ": " + created.message);
}

void testSelectionHardLinks(const fs::path& workspace)
{
    const fs::path linked = workspace / "linked-src";
    fs::create_directories(linked);
    writeBytes(linked / "a.txt", {'h', 'a', 'r', 'd'});
    check(::link((linked / "a.txt").c_str(), (linked / "b.txt").c_str()) == 0,
          "cannot create hardlink selection fixture");

    // Only the alias is selected: the content entry is missing, so the alias
    // must be stored as a regular file instead of a dangling hardlink.
    {
        const fs::path archive = workspace / "linked-alias.bak";
        BackupOptions options;
        options.includes = {"b.txt"};
        check(BackupEngine::create(linked.string(), archive.string(), options)
                  .success,
              "alias-only selection backup failed");
        const auto preview =
            BackupEngine::preview(archive.string(), workspace.string());
        check(preview.entries.size() == 2,
              "alias-only archive holds extra entries");
        check(preview.entries.back().path == "linked-src/b.txt" &&
                  preview.entries.back().type == "file" &&
                  preview.entries.back().size == 4,
              "selected alias was not stored as a regular file");
        const fs::path destination = workspace / "linked-alias-out";
        check(BackupEngine::restore(archive.string(), destination.string(), {})
                  .success,
              "alias-only restore failed");
        check(readBytes(destination / "linked-src/b.txt") ==
                  std::vector<uint8_t>({'h', 'a', 'r', 'd'}),
              "alias-only restore lost the content");
    }

    // Both names are selected: the archive must keep the hardlink relation.
    {
        const fs::path archive = workspace / "linked-both.bak";
        BackupOptions options;
        options.includes = {"a.txt", "b.txt"};
        check(BackupEngine::create(linked.string(), archive.string(), options)
                  .success,
              "hardlink pair backup failed");
        const auto preview =
            BackupEngine::preview(archive.string(), workspace.string());
        check(std::any_of(preview.entries.begin(), preview.entries.end(),
                          [](const auto& entry)
                          { return entry.type == "hardlink"; }),
              "selection lost the hardlink relationship");
        const fs::path destination = workspace / "linked-both-out";
        check(BackupEngine::restore(archive.string(), destination.string(), {})
                  .success,
              "hardlink pair restore failed");
        struct stat original
        {
        };
        struct stat alias
        {
        };
        check(::lstat((destination / "linked-src/a.txt").c_str(), &original) ==
                      0 &&
                  ::lstat((destination / "linked-src/b.txt").c_str(), &alias) ==
                      0 &&
                  original.st_ino == alias.st_ino && alias.st_nlink == 2,
              "restored selection is no longer a hardlink pair");
    }
}
} // namespace

void testSelection()
{
    TempDirectory temp;
    const fs::path workspace = temp.path;
    const fs::path source = workspace / "sel-src";
    fs::create_directories(source / "docs" / "manual");
    fs::create_directories(source / "src" / "nested");
    fs::create_directories(source / "empty");
    writeBytes(source / "docs" / "manual" / "guide.txt",
               {'g', 'u', 'i', 'd', 'e', '\n'});
    writeBytes(source / "docs" / "readme.md", {'r', 'e', 'a', 'd', '\n'});
    writeBytes(source / "src" / "main.cpp", {'m', 'a', 'i', 'n', '\n'});
    writeBytes(source / "src" / "nested" / "deep.bin", {0, 1, 2, 3, 255});
    writeBytes(source / "top.txt", {'t', 'o', 'p', '\n'});
    fs::create_symlink("top.txt", source / "link.txt");
    check(::chmod((source / "docs" / "readme.md").c_str(), 0740) == 0 &&
              ::chmod((source / "src" / "main.cpp").c_str(), 0600) == 0,
          "cannot prepare selection metadata fixture");

    int number = 0;
    verifySelection(workspace, source, "whole tree", {}, listTree(source),
                    number++);
    verifySelection(
        workspace, source, "one nested file", {"docs/manual/guide.txt"},
        {".", "docs", "docs/manual", "docs/manual/guide.txt"}, number++);
    verifySelection(
        workspace, source, "directory subtree", {"src"},
        {".", "src", "src/main.cpp", "src/nested", "src/nested/deep.bin"},
        number++);
    verifySelection(
        workspace, source, "two branches", {"top.txt", "src/nested/deep.bin"},
        {".", "top.txt", "src", "src/nested", "src/nested/deep.bin"}, number++);
    verifySelection(workspace, source, "empty directory", {"empty"},
                    {".", "empty"}, number++);
    verifySelection(workspace, source, "symlink", {"link.txt"},
                    {".", "link.txt"}, number++);
    verifySelection(
        workspace, source, "non-normalized duplicate paths",
        {"./docs//manual/../manual/guide.txt", "docs/manual/guide.txt"},
        {".", "docs", "docs/manual", "docs/manual/guide.txt"}, number++);

    expectSelectionFailure(workspace, source, {"missing.txt"}, "missing entry",
                           number++);
    expectSelectionFailure(workspace, source, {"/etc/passwd"}, "absolute path",
                           number++);
    expectSelectionFailure(workspace, source, {"../outside"}, "parent escape",
                           number++);
    expectSelectionFailure(workspace, source, {""}, "empty path", number++);
    expectSelectionFailure(workspace, source, {"."}, "the root itself",
                           number++);

    // The listing used by the selection UI must describe what create() stores.
    const auto listing = BackupEngine::scanSource(source.string());
    check(listing.size() + 1 == listTree(source).size(),
          "scanSource entry count differs from the tree");
    check(listing.front().path == "docs",
          "scanSource does not report stable, sorted paths");
    bool sawDirectory = false;
    bool sawSymlink = false;
    for (const auto& entry : listing)
    {
        if (entry.path == "src")
        {
            sawDirectory = entry.type == "directory";
        }
        if (entry.path == "link.txt")
        {
            sawSymlink = entry.type == "symlink";
        }
        if (entry.path == "top.txt")
        {
            check(entry.type == "file" && entry.size == 4 &&
                      entry.mtimeSeconds > 0,
                  "scanSource lost file metadata");
        }
    }
    check(sawDirectory && sawSymlink, "scanSource lost entry types");

    testSelectionHardLinks(workspace);
}
