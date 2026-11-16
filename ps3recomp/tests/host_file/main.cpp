#include "host_file.h"
#include "host_probe.h"

#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using ps3recomp::host::File;
using ps3recomp::host::FileAccess;
using ps3recomp::host::OpenOptions;

namespace {

bool expect(bool condition, const char* message)
{
    if (!condition)
        std::fprintf(stderr, "FAIL: %s\n", message);
    return condition;
}

bool equals(const void* data, std::size_t size, const char* want)
{
    return std::memcmp(data, want, size) == 0;
}

} // namespace

int main()
{
    const fs::path root = fs::temp_directory_path() / "ps3recomp-host-file-contract";
    fs::remove_all(root);
    fs::create_directories(root);

    const fs::path fixture = root / "fixture.bin";
    {
        std::ofstream out(fixture, std::ios::binary);
        out << "0123456789ABCDEF";
    }

    auto file = File::open(fixture.string().c_str(), {
        FileAccess::read_write, false, false, false
    });
    if (!expect(file != nullptr, "open existing fixture"))
        return 1;

    std::array<char, 16> bytes{};
    if (!expect(file->read(bytes.data(), 4) == 4, "read first four bytes") ||
        !expect(equals(bytes.data(), 4, "0123"), "first read contents") ||
        !expect(file->read_at(8, bytes.data(), 4) == 4, "absolute read") ||
        !expect(equals(bytes.data(), 4, "89AB"), "absolute read contents") ||
        !expect(file->read(bytes.data(), 4) == 4, "read after absolute read") ||
        !expect(equals(bytes.data(), 4, "4567"), "absolute read preserves sequential position") ||
        !expect(file->position() == 8, "sequential position is eight") ||
        !expect(file->size() == 16, "file size does not move position") ||
        !expect(file->position() == 8, "size preserves position"))
        return 1;

    file.reset();

    const fs::path append_fixture = root / "append.bin";
    {
        std::ofstream out(append_fixture, std::ios::binary);
        out << "base";
    }
    auto append = File::open(append_fixture.string().c_str(), {
        FileAccess::write_only, false, false, true
    });
    if (!expect(append != nullptr, "open append fixture") ||
        !expect(append->write("+tail", 5) == 5, "append write") ||
        !expect(append->position() == 9, "append write advances position"))
        return 1;
    append.reset();

    auto reopened = File::open(append_fixture.string().c_str(), {
        FileAccess::read_only, false, false, false
    });
    std::array<char, 9> appended{};
    if (!expect(reopened != nullptr, "reopen append fixture") ||
        !expect(reopened->read(appended.data(), appended.size()) == appended.size(),
                "read appended contents") ||
        !expect(equals(appended.data(), appended.size(), "base+tail"),
                "append lands at end of file"))
        return 1;
    reopened.reset();

    const fs::path npd_fixture = root / "encrypted.edat";
    {
        std::ofstream out(npd_fixture, std::ios::binary);
        static const char contents[] = "NPD\0payload";
        out.write(contents, sizeof(contents) - 1);
    }
    std::array<unsigned char, 4> probe_magic{};
    if (!expect(ps3_host_probe_prefix(npd_fixture.string().c_str(),
                                      probe_magic.data(), probe_magic.size()) == 4,
                "native prefix probe reads four bytes") ||
        !expect(equals(probe_magic.data(), probe_magic.size(), "NPD\0"),
                "native prefix probe identifies NPD"))
        return 1;

    const fs::path missing = root / "must-not-be-created.bin";
    auto truncate_only = File::open(missing.string().c_str(), {
        FileAccess::read_write, false, true, false
    });
    if (!expect(truncate_only == nullptr,
                "truncate without create rejects a missing file") ||
        !expect(!fs::exists(missing),
                "truncate without create does not create a file"))
        return 1;

    const fs::path append_truncate_path = root / "append-truncate.bin";
    {
        std::ofstream out(append_truncate_path, std::ios::binary);
        out << "stale";
    }
    auto append_truncate = File::open(append_truncate_path.string().c_str(), {
        FileAccess::write_only, false, true, true
    });
    if (!expect(append_truncate != nullptr,
                "append and truncate can be combined") ||
        !expect(append_truncate->size() == 0,
                "append and truncate empty existing file") ||
        !expect(append_truncate->position() == 0,
                "append and truncate reset position") ||
        !expect(append_truncate->write("new", 3) == 3,
                "write after append and truncate") ||
        !expect(append_truncate->position() == 3,
                "append and truncate write advances position"))
        return 1;
    append_truncate.reset();

    auto append_truncate_reader = File::open(append_truncate_path.string().c_str(), {
        FileAccess::read_only, false, false, false
    });
    std::array<char, 3> append_truncate_data{};
    if (!expect(append_truncate_reader != nullptr,
                "reopen append and truncate fixture") ||
        !expect(append_truncate_reader->read(
                    append_truncate_data.data(), append_truncate_data.size()) ==
                    append_truncate_data.size(),
                "read append and truncate contents") ||
        !expect(equals(append_truncate_data.data(), append_truncate_data.size(), "new"),
                "append and truncate preserves the new write"))
        return 1;
    append_truncate_reader.reset();

    const fs::path concurrent_append_path = root / "concurrent-append.bin";
    {
        std::ofstream out(concurrent_append_path, std::ios::binary);
        out << "base";
    }
    auto append_a = File::open(concurrent_append_path.string().c_str(), {
        FileAccess::write_only, false, false, true
    });
    auto append_b = File::open(concurrent_append_path.string().c_str(), {
        FileAccess::write_only, false, false, true
    });
    if (!expect(append_a != nullptr && append_b != nullptr,
                "open two append descriptors"))
        return 1;
    std::barrier append_start(3);
    bool append_a_ok = false;
    bool append_b_ok = false;
    std::thread append_writer_a([&] {
        append_start.arrive_and_wait();
        append_a_ok = append_a->write("A", 1) == 1;
    });
    std::thread append_writer_b([&] {
        append_start.arrive_and_wait();
        append_b_ok = append_b->write("B", 1) == 1;
    });
    append_start.arrive_and_wait();
    append_writer_a.join();
    append_writer_b.join();
    append_a.reset();
    append_b.reset();
    if (!expect(append_a_ok && append_b_ok, "concurrent append writes succeed"))
        return 1;
    auto concurrent_append_reader = File::open(concurrent_append_path.string().c_str(), {
        FileAccess::read_only, false, false, false
    });
    std::array<char, 6> concurrent_append_data{};
    if (!expect(concurrent_append_reader != nullptr,
                "reopen concurrent append fixture") ||
        !expect(concurrent_append_reader->read(
                    concurrent_append_data.data(), concurrent_append_data.size()) ==
                    concurrent_append_data.size(),
                "read concurrent append contents") ||
        !expect(concurrent_append_data[4] != concurrent_append_data[5],
                "concurrent append records do not overwrite each other"))
        return 1;
    concurrent_append_reader.reset();

    const fs::path first_path = root / "first.bin";
    const fs::path second_path = root / "second.bin";
    {
        std::ofstream first(first_path, std::ios::binary);
        std::ofstream second(second_path, std::ios::binary);
        first << "first-file";
        second << "second-file";
    }

    std::atomic<bool> first_read_passed{false};
    std::atomic<bool> second_read_passed{false};
    std::thread first_reader([&] {
        auto first = File::open(first_path.string().c_str(), {
            FileAccess::read_only, false, false, false
        });
        std::array<char, 10> data{};
        first_read_passed =
            first != nullptr &&
            first->read(data.data(), data.size()) == data.size() &&
            equals(data.data(), data.size(), "first-file");
    });
    std::thread second_reader([&] {
        auto second = File::open(second_path.string().c_str(), {
            FileAccess::read_only, false, false, false
        });
        std::array<char, 11> data{};
        second_read_passed =
            second != nullptr &&
            second->read(data.data(), data.size()) == data.size() &&
            equals(data.data(), data.size(), "second-file");
    });
    first_reader.join();
    second_reader.join();
    if (!expect(first_read_passed && second_read_passed,
                "independent files read concurrently"))
        return 1;

    fs::remove_all(root);
    std::puts("host_file_contract: PASS");
    return 0;
}
