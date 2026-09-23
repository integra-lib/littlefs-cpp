#pragma once

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <lfs.h>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>

namespace integra
{

/// The flash under the filesystem. The signatures are those a163-cgm-firmware's
/// flash transport already has, so an existing device satisfies this unchanged.
/// A device that also has `int Sync()` gets it called when littlefs asks.
template<typename Device>
concept LittlefsBlockDevice =
    requires(Device& device, std::uint32_t block, std::uint32_t offset, std::uint32_t size, void* out, const void* in) {
        {
            device.Read(block, offset, size, out)
        } -> std::same_as<int>;
        {
            device.Write(block, offset, in, size)
        } -> std::same_as<int>;
        {
            device.Erase(block)
        } -> std::same_as<int>;
    };

/// Geometry and tuning. The defaults are the values a163-cgm-firmware ran with, so a
/// filesystem it formatted mounts unchanged. `blockCycles` is the one to revisit:
/// littlefs suggests 100 to 1000, and 5 relocates metadata far more often than wear
/// levelling needs. It is not stored on flash, so changing it is safe.
struct LittlefsGeometry
{
    std::uint32_t blockSize{0U};
    std::uint32_t blockCount{0U};
    std::uint32_t readSize{128U};
    std::uint32_t progSize{128U};
    std::int32_t blockCycles{5};
};

/// Longest path accepted, not counting the terminator. Paths are copied into a
/// buffer this size before littlefs sees them.
inline constexpr std::size_t LITTLEFS_PATH_MAX = 255U;

enum class LittlefsEntry : std::uint8_t
{
    eFile,
    eDir,
    eAny,
};

/// What `ForEach` hands the callback for every entry.
struct LittlefsEntryInfo
{
    std::string_view name;
    LittlefsEntry type;
    std::uint32_t size;
};

namespace detail
{

/// littlefs takes C strings; a string_view need not be terminated. The copy is what
/// makes a substring safe to pass — without it littlefs reads on past the view's end
/// into whatever follows.
class LittlefsPath
{
public:
    constexpr explicit LittlefsPath(std::string_view path) noexcept
        : m_fits{path.size() <= LITTLEFS_PATH_MAX}
    {
        if (m_fits)
        {
            std::ranges::copy(path, m_buffer.begin());
        }
    }

    [[nodiscard]] constexpr bool Fits() const noexcept
    {
        return m_fits;
    }

    [[nodiscard]] constexpr const char* CStr() const noexcept
    {
        return m_buffer.data();
    }

private:
    std::array<char, LITTLEFS_PATH_MAX + 1U> m_buffer{};
    bool m_fits;
};

} // namespace detail

/// A littlefs filesystem over a block device, with every buffer inside the object:
/// littlefs itself allocates nothing through this wrapper, and it builds with
/// LFS_NO_MALLOC.
///
/// Mounting is explicit. `MountOrFormat()` formats only when littlefs reports the
/// flash as corrupt, which is what blank flash reports; an I/O error is returned, not
/// answered by erasing the disk — a transient read error at boot must not cost the
/// data.
///
/// The device is borrowed and must outlive the filesystem; the filesystem must
/// outlive every File opened on it.
template<LittlefsBlockDevice Device, std::size_t CACHE_SIZE = 128U, std::size_t LOOKAHEAD_SIZE = 16U>
    requires(CACHE_SIZE != 0U && LOOKAHEAD_SIZE != 0U && LOOKAHEAD_SIZE % 8U == 0U)
class Littlefs
{
public:
    class File;

    Littlefs(Device& device, const LittlefsGeometry& geometry) noexcept
        : m_config{MakeConfig(device, geometry, m_readBuffer, m_progBuffer, m_lookaheadBuffer)}
    {}

    Littlefs(const Littlefs&)            = delete;
    Littlefs& operator=(const Littlefs&) = delete;
    Littlefs(Littlefs&&)                 = delete;
    Littlefs& operator=(Littlefs&&)      = delete;

    ~Littlefs()
    {
        if (m_mounted)
        {
            std::ignore = lfs_unmount(&m_lfs);
        }
    }

    [[nodiscard]] int Mount() noexcept
    {
        const int result = lfs_mount(&m_lfs, &m_config);
        m_mounted        = result == LFS_ERR_OK;
        return result;
    }

    /// Mounts, and on first boot — littlefs reports blank or garbage flash as
    /// LFS_ERR_CORRUPT — formats and mounts again. Any other failure is returned as
    /// it is and nothing is erased.
    [[nodiscard]] int MountOrFormat() noexcept
    {
        const int result = Mount();
        if (result != LFS_ERR_CORRUPT)
        {
            return result;
        }
        if (const int formatted = Format(); formatted != LFS_ERR_OK)
        {
            return formatted;
        }
        return Mount();
    }

    /// Erases the filesystem. Unmounts first if mounted.
    [[nodiscard]] int Format() noexcept
    {
        if (m_mounted)
        {
            std::ignore = Unmount();
        }
        return lfs_format(&m_lfs, &m_config);
    }

    [[nodiscard]] int Unmount() noexcept
    {
        const int result = lfs_unmount(&m_lfs);
        m_mounted        = false;
        return result;
    }

    [[nodiscard]] bool IsMounted() const noexcept
    {
        return m_mounted;
    }

    [[nodiscard]] int Mkdir(std::string_view path) noexcept
    {
        return WithPath(path, [this](const char* p) { return lfs_mkdir(&m_lfs, p); });
    }

    [[nodiscard]] int Remove(std::string_view path) noexcept
    {
        return WithPath(path, [this](const char* p) { return lfs_remove(&m_lfs, p); });
    }

    [[nodiscard]] int Rename(std::string_view from, std::string_view to) noexcept
    {
        const detail::LittlefsPath source{from};
        const detail::LittlefsPath target{to};
        if (!source.Fits() || !target.Fits())
        {
            return LFS_ERR_NAMETOOLONG;
        }
        return lfs_rename(&m_lfs, source.CStr(), target.CStr());
    }

    [[nodiscard]] int Stat(std::string_view path, lfs_info& info) noexcept
    {
        return WithPath(path, [this, &info](const char* p) { return lfs_stat(&m_lfs, p, &info); });
    }

    /// Blocks in use, or a negative error.
    [[nodiscard]] lfs_ssize_t UsedBlocks() noexcept
    {
        return lfs_fs_size(&m_lfs);
    }

    /// Calls `callback(const LittlefsEntryInfo&)` for every entry of `path` of the
    /// requested type, "." and ".." left out. The directory is always closed before
    /// returning, on every path out: littlefs keeps a pointer to an open directory
    /// and writes through it on later commits, so a directory left open on a
    /// finished stack frame is memory corruption waiting for the next write.
    template<typename Callback>
    [[nodiscard]] int ForEach(std::string_view path, LittlefsEntry type, Callback&& callback) noexcept
    {
        const detail::LittlefsPath dirPath{path};
        if (!dirPath.Fits())
        {
            return LFS_ERR_NAMETOOLONG;
        }

        lfs_dir_t dir{};
        if (const int opened = lfs_dir_open(&m_lfs, &dir, dirPath.CStr()); opened < 0)
        {
            return opened;
        }

        int result = LFS_ERR_OK;
        lfs_info info{};
        while (true)
        {
            const int read = lfs_dir_read(&m_lfs, &dir, &info);
            if (read <= 0)
            {
                result = read;
                break;
            }
            const std::string_view name{info.name};
            if (name == "." || name == "..")
            {
                continue;
            }
            const LittlefsEntry entryType = info.type == LFS_TYPE_DIR ? LittlefsEntry::eDir : LittlefsEntry::eFile;
            if (type == LittlefsEntry::eAny || type == entryType)
            {
                callback(LittlefsEntryInfo{name, entryType, info.size});
            }
        }

        const int closed = lfs_dir_close(&m_lfs, &dir);
        return result < 0 ? result : closed;
    }

    /// Regular files directly in `path`, or a negative error.
    [[nodiscard]] lfs_ssize_t FileCount(std::string_view path) noexcept
    {
        lfs_ssize_t count = 0;
        const int result  = ForEach(path, LittlefsEntry::eFile, [&count](const LittlefsEntryInfo&) { ++count; });
        return result < 0 ? result : count;
    }

    /// The littlefs handle, for anything this wrapper does not cover.
    [[nodiscard]] lfs_t* Raw() noexcept
    {
        return &m_lfs;
    }

    /// A littlefs file with its cache inside the object, opened with
    /// lfs_file_opencfg so no allocation happens.
    ///
    /// Neither copyable nor movable. littlefs keeps a pointer to every open file and
    /// updates it on commits, so the object has to stay where it was opened. That is
    /// also what a moved-from file cannot then get wrong: the original's moved-from
    /// scoped file kept believing it was open and closed a null handle.
    class File
    {
    public:
        explicit File(Littlefs& fs) noexcept
            : m_fs{fs}
        {}

        File(const File&)            = delete;
        File& operator=(const File&) = delete;
        File(File&&)                 = delete;
        File& operator=(File&&)      = delete;

        ~File()
        {
            if (m_open)
            {
                std::ignore = Close();
            }
        }

        /// `flags` are littlefs's LFS_O_* flags. Opening an already open file closes
        /// it first.
        [[nodiscard]] int Open(std::string_view path, int flags) noexcept
        {
            if (m_open)
            {
                std::ignore = Close();
            }
            const detail::LittlefsPath filePath{path};
            if (!filePath.Fits())
            {
                return LFS_ERR_NAMETOOLONG;
            }
            m_config.buffer  = m_cache.data();
            const int result = lfs_file_opencfg(&m_fs.m_lfs, &m_file, filePath.CStr(), flags, &m_config);
            m_open           = result == LFS_ERR_OK;
            return result;
        }

        [[nodiscard]] int Close() noexcept
        {
            if (!m_open)
            {
                return LFS_ERR_BADF;
            }
            m_open = false;
            return lfs_file_close(&m_fs.m_lfs, &m_file);
        }

        /// Bytes read, 0 at the end of the file, or a negative error.
        [[nodiscard]] lfs_ssize_t Read(std::span<std::uint8_t> data) noexcept
        {
            return m_open ? lfs_file_read(&m_fs.m_lfs, &m_file, data.data(), static_cast<lfs_size_t>(data.size()))
                          : LFS_ERR_BADF;
        }

        /// Bytes written, or a negative error.
        [[nodiscard]] lfs_ssize_t Write(std::span<const std::uint8_t> data) noexcept
        {
            return m_open ? lfs_file_write(&m_fs.m_lfs, &m_file, data.data(), static_cast<lfs_size_t>(data.size()))
                          : LFS_ERR_BADF;
        }

        /// `whence` is LFS_SEEK_SET, LFS_SEEK_CUR or LFS_SEEK_END. The new position,
        /// or a negative error.
        [[nodiscard]] lfs_soff_t Seek(lfs_soff_t offset, int whence) noexcept
        {
            return m_open ? lfs_file_seek(&m_fs.m_lfs, &m_file, offset, whence) : LFS_ERR_BADF;
        }

        [[nodiscard]] lfs_soff_t Tell() noexcept
        {
            return m_open ? lfs_file_tell(&m_fs.m_lfs, &m_file) : LFS_ERR_BADF;
        }

        [[nodiscard]] lfs_soff_t Size() noexcept
        {
            return m_open ? lfs_file_size(&m_fs.m_lfs, &m_file) : LFS_ERR_BADF;
        }

        [[nodiscard]] int Sync() noexcept
        {
            return m_open ? lfs_file_sync(&m_fs.m_lfs, &m_file) : LFS_ERR_BADF;
        }

        [[nodiscard]] bool IsOpen() const noexcept
        {
            return m_open;
        }

    private:
        Littlefs& m_fs;
        lfs_file_t m_file{};
        lfs_file_config m_config{};
        std::array<std::uint8_t, CACHE_SIZE> m_cache{};
        bool m_open{false};
    };

private:
    template<typename Operation>
    [[nodiscard]] static int WithPath(std::string_view path, Operation&& operation) noexcept
    {
        const detail::LittlefsPath cPath{path};
        return cPath.Fits() ? operation(cPath.CStr()) : LFS_ERR_NAMETOOLONG;
    }

    [[nodiscard]] static Device& DeviceOf(const lfs_config* config) noexcept
    {
        return *static_cast<Device*>(config->context);
    }

    static int ReadBlock(const lfs_config* c, lfs_block_t block, lfs_off_t off, void* buffer, lfs_size_t size)
    {
        return DeviceOf(c).Read(block, off, size, buffer);
    }

    static int ProgBlock(const lfs_config* c, lfs_block_t block, lfs_off_t off, const void* buffer, lfs_size_t size)
    {
        return DeviceOf(c).Write(block, off, buffer, size);
    }

    static int EraseBlock(const lfs_config* c, lfs_block_t block)
    {
        return DeviceOf(c).Erase(block);
    }

    static int SyncDevice(const lfs_config* c)
    {
        if constexpr (requires(Device& device) {
                          {
                              device.Sync()
                          } -> std::same_as<int>;
                      })
        {
            return DeviceOf(c).Sync();
        }
        else
        {
            return LFS_ERR_OK;
        }
    }

    [[nodiscard]] static lfs_config MakeConfig(
        Device& device, const LittlefsGeometry& geometry, std::array<std::uint8_t, CACHE_SIZE>& readBuffer,
        std::array<std::uint8_t, CACHE_SIZE>& progBuffer,
        std::array<std::uint8_t, LOOKAHEAD_SIZE>& lookaheadBuffer) noexcept
    {
        lfs_config config{};
        config.context          = &device;
        config.read             = &ReadBlock;
        config.prog             = &ProgBlock;
        config.erase            = &EraseBlock;
        config.sync             = &SyncDevice;
        config.read_size        = geometry.readSize;
        config.prog_size        = geometry.progSize;
        config.block_size       = geometry.blockSize;
        config.block_count      = geometry.blockCount;
        config.block_cycles     = geometry.blockCycles;
        config.cache_size       = static_cast<lfs_size_t>(CACHE_SIZE);
        config.lookahead_size   = static_cast<lfs_size_t>(LOOKAHEAD_SIZE);
        config.read_buffer      = readBuffer.data();
        config.prog_buffer      = progBuffer.data();
        config.lookahead_buffer = lookaheadBuffer.data();
        return config;
    }

    // Declared before m_config, which points into them.
    std::array<std::uint8_t, CACHE_SIZE> m_readBuffer{};
    std::array<std::uint8_t, CACHE_SIZE> m_progBuffer{};
    std::array<std::uint8_t, LOOKAHEAD_SIZE> m_lookaheadBuffer{};
    lfs_config m_config;
    lfs_t m_lfs{};
    bool m_mounted{false};
};

/// A name for a littlefs error code, for a log line.
[[nodiscard]] constexpr std::string_view LittlefsErrorName(int error) noexcept
{
    switch (error)
    {
    case LFS_ERR_OK: return "LFS_ERR_OK";
    case LFS_ERR_IO: return "LFS_ERR_IO";
    case LFS_ERR_CORRUPT: return "LFS_ERR_CORRUPT";
    case LFS_ERR_NOENT: return "LFS_ERR_NOENT";
    case LFS_ERR_EXIST: return "LFS_ERR_EXIST";
    case LFS_ERR_NOTDIR: return "LFS_ERR_NOTDIR";
    case LFS_ERR_ISDIR: return "LFS_ERR_ISDIR";
    case LFS_ERR_NOTEMPTY: return "LFS_ERR_NOTEMPTY";
    case LFS_ERR_BADF: return "LFS_ERR_BADF";
    case LFS_ERR_FBIG: return "LFS_ERR_FBIG";
    case LFS_ERR_INVAL: return "LFS_ERR_INVAL";
    case LFS_ERR_NOSPC: return "LFS_ERR_NOSPC";
    case LFS_ERR_NOMEM: return "LFS_ERR_NOMEM";
    case LFS_ERR_NOATTR: return "LFS_ERR_NOATTR";
    case LFS_ERR_NAMETOOLONG: return "LFS_ERR_NAMETOOLONG";
    default: return "unknown littlefs error";
    }
}

} // namespace integra
