// Shared link-creation logic used by the shell extension (SymlinkCreator.dll)
// and the helper executable (SymlinkCreator.exe).
#pragma once

#include <windows.h>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace symlink_creator {

using std::filesystem::path;

inline constexpr wchar_t kAppName[] = L"SymlinkCreator";

enum class LinkKind {
    Symlink,          // Symbolic link with an absolute target.
    RelativeSymlink,  // Symbolic link with a target relative to the link folder.
    Junction,         // Directory junction (mount point). Folders only.
    HardLink,         // Hard link. Files on the same volume only.
};

struct LinkRequest {
    LinkKind kind;
    path source;     // Existing file or folder the link points to.
    path directory;  // Folder where the link is created.
    // Exact path of the link. When empty, a free name based on the source is chosen inside `directory`.
    std::optional<path> linkPath = std::nullopt;
    // Replace the existing link at `linkPath` (change its target or type). Only links are ever replaced.
    bool replace = false;
};

// An existing symbolic link or directory junction.
struct LinkInfo {
    LinkKind kind;    // Symlink, RelativeSymlink or Junction.
    path rawTarget;   // Target as stored in the link (relative for relative symlinks).
    path target;      // Absolute target.
    bool isDirectory; // The link itself is a directory link.
};

struct LinkResult {
    path source;
    path link;            // Created link (empty on failure).
    DWORD error = 0;      // Win32 error code, 0 on success.
    std::wstring message; // Human-readable reason on failure.
};

// Display name of a link kind, for menus and messages.
std::wstring KindName(LinkKind kind);

// Parses/produces the command-line names: symlink, relative, junction, hardlink.
std::optional<LinkKind> ParseKind(std::wstring_view name);
std::wstring KindId(LinkKind kind);

// True when the clipboard holds files or folders (CF_HDROP). Does not open the clipboard.
bool ClipboardHasFiles();

// Files and folders currently on the clipboard (copied or cut in Explorer).
std::vector<path> ReadClipboardFiles();

// True when the files on the clipboard were cut (Ctrl+X) rather than copied.
bool ClipboardIsCut();

// Puts plain text on the clipboard.
bool SetClipboardText(HWND owner, const std::wstring& text);

// Empties the clipboard (as Explorer does after pasting cut files).
void ClearClipboard();

// Reads a symbolic link or junction. Nothing for regular files, folders and volume mount points.
std::optional<LinkInfo> ReadLink(const path& link);

bool Exists(const path& p);

// Whether `kind` can link every source into `directory` (volume, file/folder rules).
bool IsSupported(LinkKind kind, const std::vector<path>& sources, const path& directory);

// Creates one link to `source` inside `directory`, choosing a free name ("name (2).ext").
LinkResult CreateLink(LinkKind kind, const path& source, const path& directory);

// Creates (or replaces) the link described by `request`.
LinkResult CreateLink(const LinkRequest& request);

// Creates all links. Symlinks that fail for lack of privilege are retried in an
// elevated `helper --apply` process when `helper` is given. Shows a message box
// with any failures. Returns the results of the links handled in this process.
std::vector<LinkResult> CreateLinksAndReport(const std::vector<LinkRequest>& requests, HWND owner,
                                             const std::optional<path>& helper);

// Request file passed to the elevated helper, UTF-16LE, one request per line:
// kind<TAB>source<TAB>directory<TAB>linkPath<TAB>replace(0|1)
bool WriteRequestFile(const path& file, const std::vector<LinkRequest>& requests);
std::vector<LinkRequest> ReadRequestFile(const path& file);

// Text of a Win32 error code.
std::wstring ErrorText(DWORD error);

void ShowError(HWND owner, const std::wstring& text);

}  // namespace symlink_creator
