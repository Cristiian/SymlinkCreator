#include "links.hpp"

#include <shellapi.h>
#include <shlobj.h>
#include <winioctl.h>

#include <cstring>
#include <format>

namespace symlink_creator {

namespace {

constexpr DWORD kInvalidAttributes = INVALID_FILE_ATTRIBUTES;

bool EqualsIgnoreCase(std::wstring_view a, std::wstring_view b) {
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) ==
           CSTR_EQUAL;
}

DWORD Attributes(const path& p) {
    return GetFileAttributesW(p.c_str());
}

bool IsDirectory(DWORD attributes) {
    return attributes != kInvalidAttributes && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

// Mount point of the volume holding `p` (for example "C:\" or "\\server\share\").
std::optional<std::wstring> VolumeMountPoint(const path& p) {
    std::wstring mount(MAX_PATH + 1, L'\0');
    while (!GetVolumePathNameW(p.c_str(), mount.data(), static_cast<DWORD>(mount.size()))) {
        if (GetLastError() != ERROR_FILENAME_EXCED_RANGE || mount.size() >= 32768) return std::nullopt;
        mount.resize(32768);
    }
    mount.resize(wcslen(mount.c_str()));
    return mount;
}

// Stable identifier of the volume holding `p`: its volume GUID path when available.
std::optional<std::wstring> VolumeId(const path& p) {
    auto mount = VolumeMountPoint(p);
    if (!mount) return std::nullopt;
    wchar_t guid[64];
    if (GetVolumeNameForVolumeMountPointW(mount->c_str(), guid, ARRAYSIZE(guid))) return std::wstring(guid);
    return mount;
}

bool SameVolume(const path& a, const path& b) {
    auto va = VolumeId(a);
    auto vb = VolumeId(b);
    return va && vb && EqualsIgnoreCase(*va, *vb);
}

bool IsRemote(const path& p) {
    const auto text = p.native();
    if (text.starts_with(L"\\\\") && !text.starts_with(L"\\\\?\\")) return true;
    auto mount = VolumeMountPoint(p);
    return mount && GetDriveTypeW(mount->c_str()) == DRIVE_REMOTE;
}

// Path components without empty trailing elements.
std::vector<std::wstring> Components(const path& p) {
    std::vector<std::wstring> parts;
    for (const auto& part : p.lexically_normal()) {
        if (!part.empty()) parts.push_back(part.native());
    }
    return parts;
}

// True when `directory` is `folder` or lies somewhere inside it.
bool IsSameOrInside(const path& directory, const path& folder) {
    const auto dir = Components(directory);
    const auto parent = Components(folder);
    if (parent.size() > dir.size()) return false;
    for (size_t i = 0; i < parent.size(); ++i) {
        if (!EqualsIgnoreCase(dir[i], parent[i])) return false;
    }
    return true;
}

// Case-insensitive relative path from folder `base` to `target`. Both must share a root.
std::optional<path> RelativeTo(const path& target, const path& base) {
    const auto t = Components(target);
    const auto b = Components(base);
    if (t.empty() || b.empty() || !EqualsIgnoreCase(t[0], b[0])) return std::nullopt;
    size_t common = 0;
    while (common < t.size() && common < b.size() && EqualsIgnoreCase(t[common], b[common])) ++common;
    path relative;
    for (size_t i = common; i < b.size(); ++i) relative /= L"..";
    for (size_t i = common; i < t.size(); ++i) relative /= t[i];
    if (relative.empty()) relative = L".";
    return relative;
}

// First free name for a link to `source` inside `directory`: "name.ext", "name (2).ext", ...
path FreeLinkPath(const path& source, const path& directory, bool isDirectory) {
    std::wstring name = source.filename().native();
    if (name.empty()) {
        // A drive root such as "C:\" has no file name.
        std::wstring root = source.root_name().native();
        std::erase(root, L':');
        name = L"Drive " + root;
    }
    path candidate = directory / name;
    if (!Exists(candidate)) return candidate;

    const path asPath(name);
    const std::wstring stem = isDirectory ? name : asPath.stem().native();
    const std::wstring extension = isDirectory ? L"" : asPath.extension().native();
    for (int i = 2;; ++i) {
        candidate = directory / std::format(L"{} ({}){}", stem, i, extension);
        if (!Exists(candidate)) return candidate;
    }
}

DWORD CreateSymlink(const path& link, const path& target, bool isDirectory) {
    DWORD flags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
    if (isDirectory) flags |= SYMBOLIC_LINK_FLAG_DIRECTORY;
    return CreateSymbolicLinkW(link.c_str(), target.c_str(), flags) ? ERROR_SUCCESS : GetLastError();
}

// Junctions have no Win32 API: write a mount-point reparse buffer on an empty folder.
DWORD CreateJunction(const path& link, const path& target) {
    std::wstring print = target.native();
    if (print.starts_with(L"\\\\?\\")) print.erase(0, 4);
    const std::wstring substitute = L"\\??\\" + print;

    const size_t substituteBytes = substitute.size() * sizeof(wchar_t);
    const size_t printBytes = print.size() * sizeof(wchar_t);
    const size_t pathBytes = substituteBytes + sizeof(wchar_t) + printBytes + sizeof(wchar_t);
    constexpr size_t kReparseHeader = 8;     // ReparseTag, ReparseDataLength, Reserved
    constexpr size_t kMountPointHeader = 8;  // four name offset/length WORDs
    const size_t dataLength = kMountPointHeader + pathBytes;
    if (kReparseHeader + dataLength > MAXIMUM_REPARSE_DATA_BUFFER_SIZE) return ERROR_FILENAME_EXCED_RANGE;

    std::vector<BYTE> buffer(kReparseHeader + dataLength, 0);
    auto put = [&](size_t offset, auto value) { std::memcpy(buffer.data() + offset, &value, sizeof(value)); };
    put(0, static_cast<DWORD>(IO_REPARSE_TAG_MOUNT_POINT));
    put(4, static_cast<WORD>(dataLength));
    put(8, static_cast<WORD>(0));                                         // SubstituteNameOffset
    put(10, static_cast<WORD>(substituteBytes));                          // SubstituteNameLength
    put(12, static_cast<WORD>(substituteBytes + sizeof(wchar_t)));        // PrintNameOffset
    put(14, static_cast<WORD>(printBytes));                               // PrintNameLength
    std::memcpy(buffer.data() + 16, substitute.data(), substituteBytes);
    std::memcpy(buffer.data() + 16 + substituteBytes + sizeof(wchar_t), print.data(), printBytes);

    if (!CreateDirectoryW(link.c_str(), nullptr)) return GetLastError();
    HANDLE handle = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    DWORD error = ERROR_SUCCESS;
    if (handle == INVALID_HANDLE_VALUE) {
        error = GetLastError();
    } else {
        DWORD returned = 0;
        if (!DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, buffer.data(), static_cast<DWORD>(buffer.size()),
                             nullptr, 0, &returned, nullptr)) {
            error = GetLastError();
        }
        CloseHandle(handle);
    }
    if (error != ERROR_SUCCESS) RemoveDirectoryW(link.c_str());
    return error;
}

DWORD MakeLink(LinkKind kind, const path& link, const path& source, const path& directory, bool isDirectory) {
    switch (kind) {
        case LinkKind::Symlink:
            return CreateSymlink(link, source, isDirectory);
        case LinkKind::RelativeSymlink:
            if (auto relative = RelativeTo(source, directory)) return CreateSymlink(link, *relative, isDirectory);
            return ERROR_NOT_SAME_DEVICE;
        case LinkKind::Junction:
            return CreateJunction(link, source);
        case LinkKind::HardLink:
            return CreateHardLinkW(link.c_str(), source.c_str(), nullptr) ? ERROR_SUCCESS : GetLastError();
    }
    return ERROR_INVALID_PARAMETER;
}

// Removes a symlink or junction without touching its target.
DWORD RemoveLink(const path& link) {
    const DWORD attributes = Attributes(link);
    if (attributes == kInvalidAttributes) return GetLastError();
    if (!(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return ERROR_NOT_A_REPARSE_POINT;
    const BOOL ok = (attributes & FILE_ATTRIBUTE_DIRECTORY) ? RemoveDirectoryW(link.c_str()) : DeleteFileW(link.c_str());
    return ok ? ERROR_SUCCESS : GetLastError();
}

// Reason why `kind` cannot link `source` into `directory`, or nothing when it can.
std::optional<std::wstring> Unsupported(LinkKind kind, const path& source, DWORD attributes, const path& directory) {
    const bool isDirectory = IsDirectory(attributes);
    if (isDirectory && IsSameOrInside(directory, source)) return L"A folder cannot be linked inside itself.";
    switch (kind) {
        case LinkKind::Symlink:
            return std::nullopt;
        case LinkKind::RelativeSymlink:
            if (!SameVolume(source, directory)) return L"Relative symlinks need the source and the link on the same drive.";
            return std::nullopt;
        case LinkKind::Junction:
            if (!isDirectory) return L"Directory junctions can only point to folders.";
            if (IsRemote(source)) return L"Directory junctions cannot point to network locations.";
            return std::nullopt;
        case LinkKind::HardLink:
            if (isDirectory) return L"Hard links can only point to files.";
            if (!SameVolume(source, directory)) return L"Hard links need the source and the link on the same drive.";
            return std::nullopt;
    }
    return L"Unknown link type.";
}

void ShowResults(HWND owner, const std::vector<LinkResult>& results) {
    std::wstring details;
    size_t failures = 0;
    for (const auto& result : results) {
        if (result.error == ERROR_SUCCESS) continue;
        if (++failures <= 10) {
            std::wstring name = result.source.filename().native();
            if (name.empty()) name = result.source.native();
            details += std::format(L"\u2022 {}: {}\n", name, result.message);
        }
    }
    if (failures == 0) return;
    if (failures > 10) details += std::format(L"\u2026and {} more.\n", failures - 10);
    const auto text = std::format(L"Could not create {} of {} link{}:\n\n{}", failures, results.size(),
                                  results.size() == 1 ? L"" : L"s", details);
    MessageBoxW(owner, text.c_str(), kAppName, MB_ICONERROR | MB_OK);
}

// Runs `helper --apply <request file>` elevated. Returns ERROR_SUCCESS once launched.
DWORD RunElevated(const path& helper, const std::vector<LinkRequest>& requests, HWND owner) {
    wchar_t tempDir[MAX_PATH + 1];
    wchar_t tempFile[MAX_PATH + 1];
    if (!GetTempPathW(ARRAYSIZE(tempDir), tempDir) || !GetTempFileNameW(tempDir, L"slc", 0, tempFile)) {
        return GetLastError();
    }
    if (!WriteRequestFile(tempFile, requests)) {
        const DWORD error = GetLastError();
        DeleteFileW(tempFile);
        return error;
    }

    const auto parameters = std::format(L"--apply \"{}\"", tempFile);
    SHELLEXECUTEINFOW info{sizeof(info)};
    info.fMask = SEE_MASK_FLAG_NO_UI;
    info.hwnd = owner;
    info.lpVerb = L"runas";
    info.lpFile = helper.c_str();
    info.lpParameters = parameters.c_str();
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info)) {
        const DWORD error = GetLastError();
        DeleteFileW(tempFile);
        return error;
    }
    return ERROR_SUCCESS;  // The helper reports its own results and deletes the file.
}

}  // namespace

std::wstring KindName(LinkKind kind) {
    switch (kind) {
        case LinkKind::Symlink: return L"Symlink";
        case LinkKind::RelativeSymlink: return L"Relative symlink";
        case LinkKind::Junction: return L"Directory junction";
        case LinkKind::HardLink: return L"Hard link";
    }
    return L"Link";
}

std::wstring KindId(LinkKind kind) {
    switch (kind) {
        case LinkKind::Symlink: return L"symlink";
        case LinkKind::RelativeSymlink: return L"relative";
        case LinkKind::Junction: return L"junction";
        case LinkKind::HardLink: return L"hardlink";
    }
    return L"";
}

std::optional<LinkKind> ParseKind(std::wstring_view name) {
    for (auto kind : {LinkKind::Symlink, LinkKind::RelativeSymlink, LinkKind::Junction, LinkKind::HardLink}) {
        if (EqualsIgnoreCase(name, KindId(kind))) return kind;
    }
    return std::nullopt;
}

bool ClipboardHasFiles() {
    return IsClipboardFormatAvailable(CF_HDROP) != FALSE;
}

std::vector<path> ReadClipboardFiles() {
    std::vector<path> files;
    if (!ClipboardHasFiles()) return files;

    // Another application may hold the clipboard for a moment.
    bool opened = false;
    for (int attempt = 0; attempt < 10 && !(opened = OpenClipboard(nullptr)); ++attempt) Sleep(20);
    if (!opened) return files;

    if (auto drop = static_cast<HDROP>(GetClipboardData(CF_HDROP))) {
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; ++i) {
            const UINT length = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring name(length, L'\0');
            DragQueryFileW(drop, i, name.data(), length + 1);
            if (!name.empty()) files.emplace_back(std::move(name));
        }
    }
    CloseClipboard();
    return files;
}

bool IsSupported(LinkKind kind, const std::vector<path>& sources, const path& directory) {
    if (sources.empty()) return false;
    for (const auto& source : sources) {
        const DWORD attributes = Attributes(source);
        if (attributes == kInvalidAttributes || Unsupported(kind, source, attributes, directory)) return false;
    }
    return true;
}

LinkResult CreateLink(LinkKind kind, const path& source, const path& directory) {
    return CreateLink(LinkRequest{kind, source, directory});
}

LinkResult CreateLink(const LinkRequest& request) {
    const path& source = request.source;
    const path directory = request.linkPath ? request.linkPath->parent_path() : request.directory;
    LinkResult result{source};
    auto fail = [&](DWORD error, std::wstring message = {}) {
        result.error = error;
        result.message = message.empty() ? ErrorText(error) : std::move(message);
        return result;
    };

    const DWORD attributes = Attributes(source);
    if (attributes == kInvalidAttributes) return fail(GetLastError());
    if (auto reason = Unsupported(request.kind, source, attributes, directory)) return fail(ERROR_NOT_SUPPORTED, *reason);

    const bool isDirectory = IsDirectory(attributes);
    path link = request.linkPath ? *request.linkPath : FreeLinkPath(source, directory, isDirectory);

    if (request.replace) {
        // Build the new link next to the old one, then swap, so a failure leaves the old link intact.
        if (!ReadLink(link)) return fail(ERROR_NOT_A_REPARSE_POINT, link.filename().native() + L" is not a link.");
        const path temporary = directory / std::format(L"{}.slc-{}", link.filename().native(), GetTickCount64());
        if (const DWORD error = MakeLink(request.kind, temporary, source, directory, isDirectory)) return fail(error);
        if (const DWORD error = RemoveLink(link)) {
            RemoveLink(temporary);
            return fail(error);
        }
        if (!MoveFileExW(temporary.c_str(), link.c_str(), 0)) {
            const DWORD error = GetLastError();
            return fail(error, std::format(L"The new link was left as {}: {}", temporary.native(), ErrorText(error)));
        }
        result.link = link;
        SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHW, directory.c_str(), nullptr);
        return result;
    }

    if (request.linkPath && Exists(link)) return fail(ERROR_ALREADY_EXISTS);
    if (const DWORD error = MakeLink(request.kind, link, source, directory, isDirectory)) return fail(error);
    result.link = link;
    SHChangeNotify(isDirectory ? SHCNE_MKDIR : SHCNE_CREATE, SHCNF_PATHW, link.c_str(), nullptr);
    return result;
}

bool Exists(const path& p) {
    // GetFileAttributes does not follow links, so broken links count as existing names.
    return Attributes(p) != kInvalidAttributes;
}

std::optional<LinkInfo> ReadLink(const path& link) {
    const DWORD attributes = Attributes(link);
    if (attributes == kInvalidAttributes || !(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return std::nullopt;

    HANDLE handle = CreateFileW(link.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return std::nullopt;
    std::vector<BYTE> buffer(MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
    DWORD returned = 0;
    const BOOL ok = DeviceIoControl(handle, FSCTL_GET_REPARSE_POINT, nullptr, 0, buffer.data(),
                                    static_cast<DWORD>(buffer.size()), &returned, nullptr);
    CloseHandle(handle);
    if (!ok || returned < 16) return std::nullopt;

    auto get = [&](size_t offset, auto value) {
        std::memcpy(&value, buffer.data() + offset, sizeof(value));
        return value;
    };
    const DWORD tag = get(0, DWORD{});
    size_t pathBuffer = 0;
    ULONG flags = 0;
    if (tag == IO_REPARSE_TAG_SYMLINK) {
        flags = get(16, ULONG{});
        pathBuffer = 20;
    } else if (tag == IO_REPARSE_TAG_MOUNT_POINT) {
        pathBuffer = 16;
    } else {
        return std::nullopt;
    }
    auto name = [&](size_t offsetField) {
        const size_t offset = get(offsetField, WORD{});
        const size_t length = get(offsetField + 2, WORD{});
        if (pathBuffer + offset + length > returned) return std::wstring();
        std::wstring text(length / sizeof(wchar_t), L'\0');
        std::memcpy(text.data(), buffer.data() + pathBuffer + offset, length);
        return text;
    };
    const std::wstring substitute = name(8);
    std::wstring raw = name(12);  // print name
    if (raw.empty()) {
        raw = substitute;
        if (raw.starts_with(L"\\??\\UNC\\")) raw = L"\\\\" + raw.substr(8);
        else if (raw.starts_with(L"\\??\\")) raw.erase(0, 4);
    }
    // Volume mount points ("\??\Volume{...}") are not links this tool manages.
    if (tag == IO_REPARSE_TAG_MOUNT_POINT && substitute.starts_with(L"\\??\\Volume{")) return std::nullopt;
    if (raw.empty()) return std::nullopt;

    constexpr ULONG kSymlinkFlagRelative = 1;
    LinkInfo info{};
    info.isDirectory = IsDirectory(attributes);
    info.rawTarget = raw;
    if (tag == IO_REPARSE_TAG_MOUNT_POINT) {
        info.kind = LinkKind::Junction;
        info.target = raw;
    } else if (flags & kSymlinkFlagRelative) {
        info.kind = LinkKind::RelativeSymlink;
        info.target = (link.parent_path() / raw).lexically_normal();
    } else {
        info.kind = LinkKind::Symlink;
        info.target = raw;
    }
    return info;
}

bool ClipboardIsCut() {
    static const UINT format = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
    if (!ClipboardHasFiles() || !IsClipboardFormatAvailable(format) || !OpenClipboard(nullptr)) return false;
    DWORD effect = 0;
    if (HGLOBAL data = GetClipboardData(format)) {
        if (GlobalSize(data) >= sizeof(DWORD)) {
            if (auto* value = static_cast<const DWORD*>(GlobalLock(data))) {
                effect = *value;
                GlobalUnlock(data);
            }
        }
    }
    CloseClipboard();
    return (effect & DROPEFFECT_MOVE) != 0;
}

bool SetClipboardText(HWND owner, const std::wstring& text) {
    bool opened = false;
    for (int attempt = 0; attempt < 10 && !(opened = OpenClipboard(owner)); ++attempt) Sleep(20);
    if (!opened) return false;
    bool ok = false;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void* target = GlobalLock(memory)) {
            std::memcpy(target, text.c_str(), bytes);
            GlobalUnlock(memory);
            ok = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
        }
        if (!ok) GlobalFree(memory);
    }
    CloseClipboard();
    return ok;
}

void ClearClipboard() {
    if (OpenClipboard(nullptr)) {
        EmptyClipboard();
        CloseClipboard();
    }
}

std::vector<LinkResult> CreateLinksAndReport(const std::vector<LinkRequest>& requests, HWND owner,
                                             const std::optional<path>& helper) {
    std::vector<LinkResult> results;
    std::vector<LinkRequest> elevated;
    for (const auto& request : requests) {
        auto result = CreateLink(request);
        // Without Developer Mode, symlinks need administrator rights: retry those elevated.
        if (result.error == ERROR_PRIVILEGE_NOT_HELD && helper) {
            elevated.push_back(request);
        } else {
            results.push_back(std::move(result));
        }
    }

    if (!elevated.empty()) {
        const DWORD error = RunElevated(*helper, elevated, owner);
        if (error != ERROR_SUCCESS) {
            // ERROR_CANCELLED: the user declined the UAC prompt.
            const auto message = error == ERROR_CANCELLED
                                     ? std::wstring(L"Administrator permission was declined. Turn on Developer Mode "
                                                    L"to create symlinks without it.")
                                     : ErrorText(error);
            for (const auto& request : elevated) results.push_back({request.source, {}, error, message});
        }
    }

    ShowResults(owner, results);
    return results;
}

bool WriteRequestFile(const path& file, const std::vector<LinkRequest>& requests) {
    // Tabs and line breaks cannot appear in Windows file names, so they are safe separators.
    std::wstring text = L"\uFEFF";
    for (const auto& request : requests) {
        text += std::format(L"{}\t{}\t{}\t{}\t{}\n", KindId(request.kind), request.source.native(),
                            request.directory.native(), request.linkPath ? request.linkPath->native() : L"",
                            request.replace ? 1 : 0);
    }
    HANDLE handle = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY,
                                nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const auto bytes = static_cast<DWORD>(text.size() * sizeof(wchar_t));
    const bool ok = WriteFile(handle, text.data(), bytes, &written, nullptr) && written == bytes;
    CloseHandle(handle);
    return ok;
}

std::vector<LinkRequest> ReadRequestFile(const path& file) {
    std::vector<LinkRequest> requests;
    HANDLE handle = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return requests;
    LARGE_INTEGER size{};
    std::wstring text;
    if (GetFileSizeEx(handle, &size) && size.QuadPart > 0 && size.QuadPart < (16 << 20)) {
        text.resize(static_cast<size_t>(size.QuadPart) / sizeof(wchar_t));
        DWORD read = 0;
        if (!ReadFile(handle, text.data(), static_cast<DWORD>(text.size() * sizeof(wchar_t)), &read, nullptr)) {
            text.clear();
        }
    }
    CloseHandle(handle);

    std::wstring_view rest(text);
    if (rest.starts_with(L'\uFEFF')) rest.remove_prefix(1);
    while (!rest.empty()) {
        const size_t end = rest.find(L'\n');
        std::wstring_view line = rest.substr(0, end);
        rest = end == std::wstring_view::npos ? std::wstring_view{} : rest.substr(end + 1);
        if (line.ends_with(L'\r')) line.remove_suffix(1);

        std::vector<std::wstring_view> fields;
        for (size_t start = 0;;) {
            const size_t tab = line.find(L'\t', start);
            fields.push_back(line.substr(start, tab == std::wstring_view::npos ? tab : tab - start));
            if (tab == std::wstring_view::npos) break;
            start = tab + 1;
        }
        if (fields.size() != 5) continue;
        auto kind = ParseKind(fields[0]);
        if (!kind) continue;
        LinkRequest request{*kind, path(fields[1]), path(fields[2])};
        if (!fields[3].empty()) request.linkPath = path(fields[3]);
        request.replace = fields[4] == L"1";
        requests.push_back(std::move(request));
    }
    return requests;
}

std::wstring ErrorText(DWORD error) {
    wchar_t* buffer = nullptr;
    const DWORD length =
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, error, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring text = length ? std::wstring(buffer, length) : std::format(L"Error {}.", error);
    LocalFree(buffer);
    while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r' || text.back() == L' ')) text.pop_back();
    return text;
}

void ShowError(HWND owner, const std::wstring& text) {
    MessageBoxW(owner, text.c_str(), kAppName, MB_ICONERROR | MB_OK);
}

}  // namespace symlink_creator
