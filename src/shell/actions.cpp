#include <unknwn.h>
#include <windows.h>
#include <shlobj.h>
#include <shobjidl_core.h>

#include <winrt/base.h>

#include <format>
#include <map>

#include "actions.hpp"

namespace symlink_creator {

namespace {

winrt::com_ptr<IShellItem> ShellItem(const path& p) {
    winrt::com_ptr<IShellItem> item;
    winrt::check_hresult(SHCreateItemFromParsingName(p.c_str(), nullptr, IID_PPV_ARGS(item.put())));
    return item;
}

std::optional<path> ItemPath(IShellItem* item) {
    PWSTR name = nullptr;
    if (!item || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &name))) return std::nullopt;
    path result(name);
    CoTaskMemFree(name);
    return result;
}

bool SamePath(const path& a, const path& b) {
    return _wcsicmp(a.lexically_normal().c_str(), b.lexically_normal().c_str()) == 0;
}

std::wstring NameOf(const path& p) {
    return p.filename().empty() ? p.native() : p.filename().native();
}

// Records where each item ended up after a move or copy.
struct ProgressSink : winrt::implements<ProgressSink, IFileOperationProgressSink> {
    std::map<std::wstring, path> results;  // source path (lower case) -> new path

    void Record(IShellItem* item, HRESULT hr, IShellItem* created) {
        if (FAILED(hr) || !created) return;
        auto source = ItemPath(item);
        auto target = ItemPath(created);
        if (!source || !target) return;
        std::wstring key = source->lexically_normal().native();
        CharLowerBuffW(key.data(), static_cast<DWORD>(key.size()));
        results[key] = *target;
    }

    std::optional<path> Find(const path& source) const {
        std::wstring key = source.lexically_normal().native();
        CharLowerBuffW(key.data(), static_cast<DWORD>(key.size()));
        auto it = results.find(key);
        return it == results.end() ? std::nullopt : std::optional<path>(it->second);
    }

    IFACEMETHODIMP StartOperations() override { return S_OK; }
    IFACEMETHODIMP FinishOperations(HRESULT) override { return S_OK; }
    IFACEMETHODIMP PreRenameItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
    IFACEMETHODIMP PostRenameItem(DWORD, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
    IFACEMETHODIMP PreMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
    IFACEMETHODIMP PostMoveItem(DWORD, IShellItem* item, IShellItem*, LPCWSTR, HRESULT hr, IShellItem* created) override {
        Record(item, hr, created);
        return S_OK;
    }
    IFACEMETHODIMP PreCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
    IFACEMETHODIMP PostCopyItem(DWORD, IShellItem* item, IShellItem*, LPCWSTR, HRESULT hr, IShellItem* created) override {
        Record(item, hr, created);
        return S_OK;
    }
    IFACEMETHODIMP PreDeleteItem(DWORD, IShellItem*) override { return S_OK; }
    IFACEMETHODIMP PostDeleteItem(DWORD, IShellItem*, HRESULT, IShellItem*) override { return S_OK; }
    IFACEMETHODIMP PreNewItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
    IFACEMETHODIMP PostNewItem(DWORD, IShellItem*, LPCWSTR, LPCWSTR, DWORD, HRESULT, IShellItem*) override { return S_OK; }
    IFACEMETHODIMP UpdateProgress(UINT, UINT) override { return S_OK; }
    IFACEMETHODIMP ResetTimer() override { return S_OK; }
    IFACEMETHODIMP PauseTimer() override { return S_OK; }
    IFACEMETHODIMP ResumeTimer() override { return S_OK; }
};

winrt::com_ptr<IFileOperation> NewFileOperation(HWND owner, DWORD flags) {
    auto operation = winrt::create_instance<IFileOperation>(CLSID_FileOperation);
    if (owner) operation->SetOwnerWindow(owner);
    winrt::check_hresult(operation->SetOperationFlags(flags));
    return operation;
}

std::optional<LinkInfo> ReadLinkOrWarn(HWND owner, const path& link) {
    auto info = ReadLink(link);
    if (!info) ShowError(owner, NameOf(link) + L" is not a symbolic link or junction.");
    return info;
}

}  // namespace

void OpenTargetLocation(HWND owner, const path& link) {
    const auto info = ReadLinkOrWarn(owner, link);
    if (!info) return;
    if (!Exists(info->target)) {
        ShowError(owner, std::format(L"The target of {} no longer exists:\n\n{}", NameOf(link), info->target.native()));
        return;
    }
    PIDLIST_ABSOLUTE pidl = nullptr;
    HRESULT hr = SHParseDisplayName(info->target.c_str(), nullptr, &pidl, 0, nullptr);
    if (SUCCEEDED(hr)) {
        hr = SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
        CoTaskMemFree(pidl);
    }
    if (FAILED(hr)) ShowError(owner, L"Could not open the target location: " + ErrorText(hr));
}

void CopyTargetPaths(HWND owner, const std::vector<path>& links) {
    std::wstring text;
    for (const auto& link : links) {
        if (auto info = ReadLink(link)) {
            if (!text.empty()) text += L"\r\n";
            text += info->target.native();
        }
    }
    if (text.empty()) return;
    if (!SetClipboardText(owner, text)) ShowError(owner, L"Could not copy to the clipboard.");
}

void ChangeTarget(HWND owner, const path& link, const std::optional<path>& helper) {
    const auto info = ReadLinkOrWarn(owner, link);
    if (!info) return;

    auto dialog = winrt::create_instance<IFileOpenDialog>(CLSID_FileOpenDialog);
    FILEOPENDIALOGOPTIONS options = 0;
    dialog->GetOptions(&options);
    options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST;
    if (info->isDirectory) options |= FOS_PICKFOLDERS;
    dialog->SetOptions(options);
    const auto title = std::format(L"New target for {}", NameOf(link));
    dialog->SetTitle(title.c_str());
    const path start = Exists(info->target) ? info->target.parent_path() : link.parent_path();
    if (Exists(start)) {
        try {
            dialog->SetFolder(ShellItem(start).get());
        } catch (...) {
        }
    }
    if (FAILED(dialog->Show(owner))) return;  // Cancelled.
    winrt::com_ptr<IShellItem> picked;
    if (FAILED(dialog->GetResult(picked.put()))) return;
    const auto target = ItemPath(picked.get());
    if (!target) return;

    LinkKind kind = info->kind;
    // A relative link cannot cross drives: keep it working by making it absolute.
    if (kind == LinkKind::RelativeSymlink && !IsSupported(kind, {*target}, link.parent_path())) kind = LinkKind::Symlink;
    CreateLinksAndReport({{kind, *target, link.parent_path(), link, true}}, owner, helper);
}

void ConvertLinks(HWND owner, const std::vector<path>& links, LinkKind kind, const std::optional<path>& helper) {
    std::vector<LinkRequest> requests;
    for (const auto& link : links) {
        if (auto info = ReadLink(link); info && info->kind != kind) {
            requests.push_back({kind, info->target, link.parent_path(), link, true});
        }
    }
    if (!requests.empty()) CreateLinksAndReport(requests, owner, helper);
}

void ReplaceWithCopies(HWND owner, const std::vector<path>& links) {
    struct Pending {
        path link;
        path parked;  // the link, renamed out of the way while its target is copied
    };
    std::vector<Pending> pending;
    std::vector<LinkResult> failures;

    auto operation = NewFileOperation(owner, FOF_NOCONFIRMMKDIR);
    for (const auto& link : links) {
        const auto info = ReadLink(link);
        if (!info) continue;
        if (!Exists(info->target)) {
            failures.push_back({link, {}, ERROR_FILE_NOT_FOUND, L"The target no longer exists."});
            continue;
        }
        const path parked = link.parent_path() / std::format(L"{}.slc-{}", NameOf(link), GetTickCount64());
        if (!MoveFileExW(link.c_str(), parked.c_str(), 0)) {
            const DWORD error = GetLastError();
            failures.push_back({link, {}, error, ErrorText(error)});
            continue;
        }
        try {
            winrt::check_hresult(operation->CopyItem(ShellItem(info->target).get(), ShellItem(link.parent_path()).get(),
                                                     NameOf(link).c_str(), nullptr));
            pending.push_back({link, parked});
        } catch (const winrt::hresult_error& e) {
            MoveFileExW(parked.c_str(), link.c_str(), 0);
            failures.push_back({link, {}, static_cast<DWORD>(e.code()), std::wstring(e.message())});
        }
    }

    if (!pending.empty()) operation->PerformOperations();

    for (const auto& item : pending) {
        if (Exists(item.link) && !ReadLink(item.link)) {
            // The copy is in place: remove the parked link (never its target).
            const DWORD attributes = GetFileAttributesW(item.parked.c_str());
            if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
                RemoveDirectoryW(item.parked.c_str());
            } else {
                DeleteFileW(item.parked.c_str());
            }
        } else {
            // Copy cancelled or failed: put the link back.
            MoveFileExW(item.parked.c_str(), item.link.c_str(), 0);
            failures.push_back({item.link, {}, ERROR_CANCELLED, L"The copy was cancelled or failed; the link was kept."});
        }
        SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHW, item.link.parent_path().c_str(), nullptr);
    }

    if (!failures.empty()) {
        std::wstring text = L"Some links were not replaced:\n\n";
        for (const auto& failure : failures) text += std::format(L"• {}: {}\n", NameOf(failure.source), failure.message);
        ShowError(owner, text);
    }
}

void MoveAndLeaveLinks(HWND owner, const std::vector<path>& sources, const path& destination,
                       const std::optional<path>& helper) {
    std::vector<path> moving;
    for (const auto& source : sources) {
        if (!SamePath(source.parent_path(), destination)) moving.push_back(source);
    }
    if (moving.empty()) {
        ShowError(owner, L"The cut items are already in this folder.");
        return;
    }

    auto operation = NewFileOperation(owner, FOF_NOCONFIRMMKDIR | FOF_ALLOWUNDO);
    auto sink = winrt::make_self<ProgressSink>();
    DWORD cookie = 0;
    winrt::check_hresult(operation->Advise(sink.get(), &cookie));
    const auto folder = ShellItem(destination);
    for (const auto& source : moving) {
        winrt::check_hresult(operation->MoveItem(ShellItem(source).get(), folder.get(), nullptr, nullptr));
    }
    operation->PerformOperations();
    operation->Unadvise(cookie);

    std::vector<LinkRequest> requests;
    for (const auto& source : moving) {
        if (auto moved = sink->Find(source); moved && !Exists(source)) {
            requests.push_back({LinkKind::Symlink, *moved, source.parent_path(), source, false});
        }
    }
    if (requests.empty()) return;  // Nothing moved (cancelled or failed; the shell already reported why).

    // The clipboard still points at the old paths, which are about to become links.
    ClearClipboard();
    CreateLinksAndReport(requests, owner, helper);
}

}  // namespace symlink_creator
