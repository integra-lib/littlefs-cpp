#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <hwlib/persistence/littlefs.hpp>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace
{

using hwlib::persistence::LittlefsEntry;
using hwlib::persistence::LittlefsEntryInfo;
using hwlib::persistence::LittlefsGeometry;

// NOR-like flash in RAM: erased to 0xFF, and able to fail a number of reads on
// demand, the way a flaky bus does at boot.
class RamFlash
{
public:
    static constexpr std::uint32_t BLOCK_SIZE  = 4096U;
    static constexpr std::uint32_t BLOCK_COUNT = 16U;

    RamFlash()
    {
        m_memory.fill(0xFFU);
    }

    int Read(std::uint32_t block, std::uint32_t offset, std::uint32_t size, void* out)
    {
        if (m_failReads > 0)
        {
            --m_failReads;
            return LFS_ERR_IO;
        }
        std::memcpy(out, &m_memory.at((block * BLOCK_SIZE) + offset), size);
        return LFS_ERR_OK;
    }

    int Write(std::uint32_t block, std::uint32_t offset, const void* in, std::uint32_t size)
    {
        std::memcpy(&m_memory.at((block * BLOCK_SIZE) + offset), in, size);
        return LFS_ERR_OK;
    }

    int Erase(std::uint32_t block)
    {
        std::memset(&m_memory.at(block * BLOCK_SIZE), 0xFF, BLOCK_SIZE);
        return LFS_ERR_OK;
    }

    void FailNextReads(int count)
    {
        m_failReads = count;
    }

private:
    std::array<std::uint8_t, BLOCK_SIZE * BLOCK_COUNT> m_memory{};
    int m_failReads{0};
};

class SyncingFlash : public RamFlash
{
public:
    int Sync()
    {
        ++syncs;
        return LFS_ERR_OK;
    }

    int syncs{0};
};

constexpr LittlefsGeometry GEOMETRY{.blockSize = RamFlash::BLOCK_SIZE, .blockCount = RamFlash::BLOCK_COUNT};

using Fs = hwlib::persistence::Littlefs<RamFlash>;

[[nodiscard]] std::span<const std::uint8_t> Bytes(std::string_view text)
{
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

void WriteFile(Fs& fs, std::string_view path, std::string_view text)
{
    Fs::File file{fs};
    ASSERT_EQ(file.Open(path, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC), LFS_ERR_OK);
    ASSERT_EQ(file.Write(Bytes(text)), static_cast<lfs_ssize_t>(text.size()));
}

[[nodiscard]] std::string ReadFile(Fs& fs, std::string_view path)
{
    Fs::File file{fs};
    if (file.Open(path, LFS_O_RDONLY) != LFS_ERR_OK)
    {
        return "<cannot open>";
    }
    std::array<std::uint8_t, 64> buffer{};
    const lfs_ssize_t read = file.Read(buffer);
    return read < 0 ? "<cannot read>"
                    : std::string{reinterpret_cast<const char*>(buffer.data()), static_cast<std::size_t>(read)};
}

TEST(LittlefsTest, MountAloneDoesNotFormatBlankFlash)
{
    RamFlash flash;
    Fs fs{flash, GEOMETRY};
    EXPECT_EQ(fs.Mount(), LFS_ERR_CORRUPT);
    EXPECT_FALSE(fs.IsMounted());
}

TEST(LittlefsTest, MountOrFormatFormatsBlankFlashOnFirstBoot)
{
    RamFlash flash;
    Fs fs{flash, GEOMETRY};
    EXPECT_EQ(fs.MountOrFormat(), LFS_ERR_OK);
    EXPECT_TRUE(fs.IsMounted());
}

TEST(LittlefsTest, KeepsTheDataThroughATransientReadErrorAtMount)
{
    // The a163 original formatted on any mount failure: one read error at boot and
    // every file on the device was gone.
    RamFlash flash;
    {
        Fs fs{flash, GEOMETRY};
        ASSERT_EQ(fs.MountOrFormat(), LFS_ERR_OK);
        WriteFile(fs, "/keep", "precious");
    }

    flash.FailNextReads(1);
    {
        Fs fs{flash, GEOMETRY};
        EXPECT_EQ(fs.MountOrFormat(), LFS_ERR_IO);
        EXPECT_FALSE(fs.IsMounted());
    }

    Fs fs{flash, GEOMETRY};
    ASSERT_EQ(fs.Mount(), LFS_ERR_OK);
    EXPECT_EQ(ReadFile(fs, "/keep"), "precious");
}

TEST(LittlefsTest, WritesAndReadsAFileWithoutAllocating)
{
    // littlefs is built with LFS_NO_MALLOC for these tests: the allocating
    // lfs_file_open does not exist, so this only compiles because File uses
    // lfs_file_opencfg with its own cache.
    RamFlash flash;
    Fs fs{flash, GEOMETRY};
    ASSERT_EQ(fs.MountOrFormat(), LFS_ERR_OK);

    WriteFile(fs, "/data", "hello littlefs");
    EXPECT_EQ(ReadFile(fs, "/data"), "hello littlefs");
}

TEST(LittlefsTest, SeeksTellsAndSizes)
{
    RamFlash flash;
    Fs fs{flash, GEOMETRY};
    ASSERT_EQ(fs.MountOrFormat(), LFS_ERR_OK);
    WriteFile(fs, "/f", "0123456789");

    Fs::File file{fs};
    ASSERT_EQ(file.Open("/f", LFS_O_RDONLY), LFS_ERR_OK);
    EXPECT_EQ(file.Size(), 10);
    EXPECT_EQ(file.Seek(4, LFS_SEEK_SET), 4);
    EXPECT_EQ(file.Tell(), 4);
    std::array<std::uint8_t, 3> part{};
    EXPECT_EQ(file.Read(part), 3);
    EXPECT_EQ((std::string{part.begin(), part.end()}), "456");
    EXPECT_EQ(file.Sync(), LFS_ERR_OK);
}

TEST(LittlefsTest, OpensThePathOfASubstring)
{
    // The a163 original handed a string_view's data() to littlefs as a C string: a
    // view of "/a" inside "/ab" opened "/ab".
    RamFlash flash;
    Fs fs{flash, GEOMETRY};
    ASSERT_EQ(fs.MountOrFormat(), LFS_ERR_OK);
    WriteFile(fs, "/ab", "wrong file");
    WriteFile(fs, "/a", "right file");

    const std::string both = "/ab";
    EXPECT_EQ(ReadFile(fs, std::string_view{both}.substr(0, 2)), "right file");
}

TEST(LittlefsTest, RefusesAPathLongerThanItCanTerminate)
{
    RamFlash flash;
    Fs fs{flash, GEOMETRY};
    ASSERT_EQ(fs.MountOrFormat(), LFS_ERR_OK);
    const std::string tooLong(hwlib::persistence::LITTLEFS_PATH_MAX + 1U, 'x');

    Fs::File file{fs};
    EXPECT_EQ(file.Open(tooLong, LFS_O_RDONLY), LFS_ERR_NAMETOOLONG);
    EXPECT_EQ(fs.Mkdir(tooLong), LFS_ERR_NAMETOOLONG);
    EXPECT_EQ(fs.Remove(tooLong), LFS_ERR_NAMETOOLONG);
    EXPECT_EQ(fs.Rename(tooLong, "/b"), LFS_ERR_NAMETOOLONG);
    lfs_info info{};
    EXPECT_EQ(fs.Stat(tooLong, info), LFS_ERR_NAMETOOLONG);
    EXPECT_EQ(fs.ForEach(tooLong, LittlefsEntry::eAny, [](const LittlefsEntryInfo&) {}), LFS_ERR_NAMETOOLONG);
}

TEST(LittlefsTest, ListsADirectoryWithoutDotEntries)
{
    RamFlash flash;
    Fs fs{flash, GEOMETRY};
    ASSERT_EQ(fs.MountOrFormat(), LFS_ERR_OK);
    ASSERT_EQ(fs.Mkdir("/ring"), LFS_ERR_OK);
    ASSERT_EQ(fs.Mkdir("/ring/sub"), LFS_ERR_OK);
    WriteFile(fs, "/ring/a", "1");
    WriteFile(fs, "/ring/b", "22");

    std::vector<std::string> all;
    ASSERT_EQ(fs.ForEach("/ring", LittlefsEntry::eAny, [&](const LittlefsEntryInfo& e) { all.emplace_back(e.name); }),
              LFS_ERR_OK);
    EXPECT_EQ(all, (std::vector<std::string>{"a", "b", "sub"}));

    std::vector<std::uint32_t> fileSizes;
    ASSERT_EQ(
        fs.ForEach("/ring", LittlefsEntry::eFile, [&](const LittlefsEntryInfo& e) { fileSizes.push_back(e.size); }),
        LFS_ERR_OK);
    EXPECT_EQ(fileSizes, (std::vector<std::uint32_t>{1U, 2U}));

    std::vector<std::string> dirs;
    ASSERT_EQ(fs.ForEach("/ring", LittlefsEntry::eDir, [&](const LittlefsEntryInfo& e) { dirs.emplace_back(e.name); }),
              LFS_ERR_OK);
    EXPECT_EQ(dirs, (std::vector<std::string>{"sub"}));
    EXPECT_EQ(fs.Raw()->mlist, nullptr);
}

TEST(LittlefsTest, CountsFilesAndLeavesNoDirectoryOpen)
{
    // The a163 original returned from FileCount with the directory still open on its
    // stack frame; littlefs then read that dead frame on the next file open.
    // AddressSanitizer caught that in the original, but only while the frame was not
    // reused, which is luck, not a test. littlefs's own list of open files and
    // directories is the direct evidence: nothing may be left on it.
    RamFlash flash;
    Fs fs{flash, GEOMETRY};
    ASSERT_EQ(fs.MountOrFormat(), LFS_ERR_OK);
    ASSERT_EQ(fs.Mkdir("/d"), LFS_ERR_OK);
    WriteFile(fs, "/d/a", "1");
    ASSERT_EQ(fs.Mkdir("/d/sub"), LFS_ERR_OK);

    EXPECT_EQ(fs.FileCount("/d"), 1);
    EXPECT_EQ(fs.Raw()->mlist, nullptr);
    WriteFile(fs, "/d/b", "2");
    EXPECT_EQ(fs.FileCount("/d"), 2);
    EXPECT_EQ(fs.FileCount("/missing"), LFS_ERR_NOENT);
    EXPECT_EQ(fs.Raw()->mlist, nullptr);
}

TEST(LittlefsTest, ClosesTheFileWhenItGoesOutOfScope)
{
    RamFlash flash;
    Fs fs{flash, GEOMETRY};
    ASSERT_EQ(fs.MountOrFormat(), LFS_ERR_OK);
    {
        Fs::File file{fs};
        ASSERT_EQ(file.Open("/scoped", LFS_O_WRONLY | LFS_O_CREAT), LFS_ERR_OK);
        ASSERT_EQ(file.Write(Bytes("kept")), 4);
    }
    // Closing is what commits the data, so it has to have happened.
    EXPECT_EQ(ReadFile(fs, "/scoped"), "kept");
}

TEST(LittlefsTest, ClosesOnlyOnce)
{
    RamFlash flash;
    Fs fs{flash, GEOMETRY};
    ASSERT_EQ(fs.MountOrFormat(), LFS_ERR_OK);

    Fs::File file{fs};
    ASSERT_EQ(file.Open("/f", LFS_O_WRONLY | LFS_O_CREAT), LFS_ERR_OK);
    EXPECT_EQ(file.Close(), LFS_ERR_OK);
    EXPECT_EQ(file.Close(), LFS_ERR_BADF);
    EXPECT_FALSE(file.IsOpen());

    std::array<std::uint8_t, 4> buffer{};
    EXPECT_EQ(file.Read(buffer), LFS_ERR_BADF);
    EXPECT_EQ(file.Write(buffer), LFS_ERR_BADF);
    EXPECT_EQ(file.Seek(0, LFS_SEEK_SET), LFS_ERR_BADF);
    EXPECT_EQ(file.Size(), LFS_ERR_BADF);
    EXPECT_EQ(file.Sync(), LFS_ERR_BADF);
}

TEST(LittlefsTest, ReopeningClosesTheFirstFile)
{
    RamFlash flash;
    Fs fs{flash, GEOMETRY};
    ASSERT_EQ(fs.MountOrFormat(), LFS_ERR_OK);
    WriteFile(fs, "/one", "first");
    WriteFile(fs, "/two", "second");

    Fs::File file{fs};
    ASSERT_EQ(file.Open("/one", LFS_O_RDONLY), LFS_ERR_OK);
    ASSERT_EQ(file.Open("/two", LFS_O_RDONLY), LFS_ERR_OK);
    std::array<std::uint8_t, 6> buffer{};
    EXPECT_EQ(file.Read(buffer), 6);
    EXPECT_EQ((std::string{buffer.begin(), buffer.end()}), "second");
}

TEST(LittlefsTest, CannotBeMovedOrCopied)
{
    // littlefs keeps a pointer to every open file. A moved file would leave that
    // pointer behind — the a163 original's moved-from scoped file closed a null
    // handle and tripped littlefs's own assertion.
    static_assert(!std::is_move_constructible_v<Fs::File>);
    static_assert(!std::is_copy_constructible_v<Fs::File>);
    static_assert(!std::is_move_constructible_v<Fs>);
    SUCCEED();
}

TEST(LittlefsTest, RemountsAfterTheFilesystemObjectIsGone)
{
    RamFlash flash;
    {
        Fs fs{flash, GEOMETRY};
        ASSERT_EQ(fs.MountOrFormat(), LFS_ERR_OK);
        WriteFile(fs, "/x", "1");
    }
    Fs again{flash, GEOMETRY};
    EXPECT_EQ(again.Mount(), LFS_ERR_OK);
    EXPECT_EQ(ReadFile(again, "/x"), "1");
}

TEST(LittlefsTest, RenamesRemovesAndStats)
{
    RamFlash flash;
    Fs fs{flash, GEOMETRY};
    ASSERT_EQ(fs.MountOrFormat(), LFS_ERR_OK);
    WriteFile(fs, "/old", "abc");

    ASSERT_EQ(fs.Rename("/old", "/new"), LFS_ERR_OK);
    lfs_info info{};
    EXPECT_EQ(fs.Stat("/old", info), LFS_ERR_NOENT);
    ASSERT_EQ(fs.Stat("/new", info), LFS_ERR_OK);
    EXPECT_EQ(info.size, 3U);

    EXPECT_EQ(fs.Remove("/new"), LFS_ERR_OK);
    EXPECT_EQ(fs.Stat("/new", info), LFS_ERR_NOENT);
    EXPECT_GT(fs.UsedBlocks(), 0);
}

TEST(LittlefsTest, CallsTheDevicesSyncWhenItHasOne)
{
    SyncingFlash flash;
    hwlib::persistence::Littlefs<SyncingFlash> fs{flash, GEOMETRY};
    ASSERT_EQ(fs.MountOrFormat(), LFS_ERR_OK);
    hwlib::persistence::Littlefs<SyncingFlash>::File file{fs};
    ASSERT_EQ(file.Open("/s", LFS_O_WRONLY | LFS_O_CREAT), LFS_ERR_OK);
    ASSERT_EQ(file.Write(Bytes("x")), 1);
    ASSERT_EQ(file.Sync(), LFS_ERR_OK);
    EXPECT_GT(flash.syncs, 0);
}

TEST(LittlefsTest, NamesItsErrors)
{
    static_assert(hwlib::persistence::LittlefsErrorName(LFS_ERR_CORRUPT) == "LFS_ERR_CORRUPT");
    EXPECT_EQ(hwlib::persistence::LittlefsErrorName(LFS_ERR_NOSPC), "LFS_ERR_NOSPC");
    EXPECT_EQ(hwlib::persistence::LittlefsErrorName(-1000), "unknown littlefs error");
}

} // namespace
