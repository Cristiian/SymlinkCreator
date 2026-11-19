// SymlinkCreator.dll: IExplorerCommand handlers for the Explorer context menu.
//
// Two top-level commands:
//   "Paste as symlink"  - one click, absolute symlink.
//   "Paste as link"     - submenu with relative symlink, directory junction and hard link.
// They are registered through the sparse package (package/AppxManifest.xml), and Windows 11
// shows packaged commands in both the new menu and "Show more options".

#include <unknwn.h>
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl_core.h>

#include <winrt/base.h>

#include <memory>

#include "../common/ids.hpp"
#include "../common/links.hpp"
#include "actions.hpp"

using namespace symlink_creator;

namespace {

HMODULE g_module = nullptr;

// Site (the Explorer view) shared by a top-level command and its subcommands.
struct SiteHolder {
    winrt::com_ptr<IUnknown> site;
};

struct Destination {
    std::optional<path> directory;
    bool multipleSelected = false;
};

// SymlinkCreator.exe next to this DLL, used to retry symlinks with administrator rights.
std::optional<path> HelperPath() {
    std::wstring module(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(g_module, module.data(), static_cast<DWORD>(module.size()));
        if (length == 0) return std::nullopt;
        if (length < module.size()) {
            module.resize(length);
            break;
        }
        module.resize(module.size() * 2);
    }
    path helper = path(module).parent_path() / L"SymlinkCreator.exe";
    if (GetFileAttributesW(helper.c_str()) == INVALID_FILE_ATTRIBUTES) return std::nullopt;
    return helper;
}

// File-system path of `item` when it is a real folder on disk (not a library, zip file or virtual folder).
std::optional<path> FolderPath(IShellItem* item) {
    PWSTR name = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &name))) return std::nullopt;
    path folder(name);
    CoTaskMemFree(name);
    const DWORD attributes = GetFileAttributesW(folder.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) return std::nullopt;
    return folder;
}

// Folder shown in the Explorer window that hosts the menu.
std::optional<path> SiteFolder(IUnknown* site) {
    if (!site) return std::nullopt;
    winrt::com_ptr<IShellBrowser> browser;
    if (FAILED(IUnknown_QueryService(site, SID_STopLevelBrowser, IID_PPV_ARGS(browser.put())))) return std::nullopt;
    winrt::com_ptr<IShellView> view;
    if (FAILED(browser->QueryActiveShellView(view.put()))) return std::nullopt;
    auto folderView = view.try_as<IFolderView>();
    if (!folderView) return std::nullopt;
    winrt::com_ptr<IPersistFolder2> folder;
    if (FAILED(folderView->GetFolder(IID_PPV_ARGS(folder.put())))) return std::nullopt;
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(folder->GetCurFolder(&pidl))) return std::nullopt;
    winrt::com_ptr<IShellItem> item;
    const HRESULT hr = SHCreateItemFromIDList(pidl, IID_PPV_ARGS(item.put()));
    CoTaskMemFree(pidl);
    return SUCCEEDED(hr) ? FolderPath(item.get()) : std::nullopt;
}

// Where links go: the right-clicked folder, or the open folder when clicking its background.
Destination ResolveDestination(IShellItemArray* items, IUnknown* site) {
    if (items) {
        DWORD count = 0;
        if (SUCCEEDED(items->GetCount(&count))) {
            if (count > 1) return {std::nullopt, true};
            winrt::com_ptr<IShellItem> item;
            if (count == 1 && SUCCEEDED(items->GetItemAt(0, item.put()))) {
                if (auto folder = FolderPath(item.get())) return {folder};
            }
        }
    }
    return {SiteFolder(site)};
}

HWND OwnerWindow(IUnknown* site) {
    HWND owner = nullptr;
    if (site) IUnknown_GetWindow(site, &owner);
    return owner;
}

// True when every copied item can be pasted here as `kind`. The clipboard keeps the last copied
// items, so this also hides the commands when, for example, the copied folder is the one clicked.
bool CanPaste(LinkKind kind, IShellItemArray* items, IUnknown* site) {
    if (!ClipboardHasFiles()) return false;
    const auto destination = ResolveDestination(items, site);
    return destination.directory && IsSupported(kind, ReadClipboardFiles(), *destination.directory);
}

void PasteLinks(LinkKind kind, IShellItemArray* items, IUnknown* site) {
    const HWND owner = OwnerWindow(site);
    const auto destination = ResolveDestination(items, site);
    if (destination.multipleSelected) {
        ShowError(owner, L"Select a single folder to paste the links into.");
        return;
    }
    if (!destination.directory) {
        ShowError(owner, L"This location is not a folder on disk, so links cannot be created here.");
        return;
    }
    const auto sources = ReadClipboardFiles();
    if (sources.empty()) {
        ShowError(owner, L"Copy or cut some files or folders first, then paste them as links.");
        return;
    }
    std::vector<LinkRequest> requests;
    for (const auto& source : sources) requests.push_back({kind, source, *destination.directory});
    CreateLinksAndReport(requests, owner, HelperPath());
}

// Selected items: the array Explorer passes, or the selection of the hosting view.
std::vector<path> SelectedPaths(IShellItemArray* items, IUnknown* site) {
    winrt::com_ptr<IShellItemArray> selection;
    if (items) {
        selection.copy_from(items);
    } else if (site) {
        winrt::com_ptr<IShellBrowser> browser;
        winrt::com_ptr<IShellView> view;
        if (SUCCEEDED(IUnknown_QueryService(site, SID_STopLevelBrowser, IID_PPV_ARGS(browser.put()))) &&
            SUCCEEDED(browser->QueryActiveShellView(view.put()))) {
            if (auto folderView = view.try_as<IFolderView>()) {
                folderView->Items(SVGIO_SELECTION, IID_PPV_ARGS(selection.put()));
            }
        }
    }
    std::vector<path> paths;
    DWORD count = 0;
    if (!selection || FAILED(selection->GetCount(&count))) return paths;
    for (DWORD i = 0; i < count; ++i) {
        winrt::com_ptr<IShellItem> item;
        PWSTR name = nullptr;
        if (SUCCEEDED(selection->GetItemAt(i, item.put())) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &name))) {
            paths.emplace_back(name);
            CoTaskMemFree(name);
        } else {
            return {};  // Something that is not on disk.
        }
    }
    return paths;
}

// The selection, when every selected item is a symlink or junction; otherwise empty.
std::vector<path> SelectedLinks(IShellItemArray* items, IUnknown* site) {
    auto paths = SelectedPaths(items, site);
    for (const auto& p : paths) {
        if (!ReadLink(p)) return {};
    }
    return paths;
}

enum class LinkAction { OpenTarget, CopyTargetPath, ChangeTarget, ToRelative, ToAbsolute, ReplaceWithCopy };

const wchar_t* ActionTitle(LinkAction action) {
    switch (action) {
        case LinkAction::OpenTarget: return L"Open target location";
        case LinkAction::CopyTargetPath: return L"Copy target path";
        case LinkAction::ChangeTarget: return L"Change target…";
        case LinkAction::ToRelative: return L"Convert to relative symlink";
        case LinkAction::ToAbsolute: return L"Convert to absolute symlink";
        case LinkAction::ReplaceWithCopy: return L"Replace with a copy of the target";
    }
    return L"";
}

bool ActionEnabled(LinkAction action, const std::vector<path>& links) {
    if (links.empty()) return false;
    switch (action) {
        case LinkAction::OpenTarget:
        case LinkAction::ChangeTarget:
            return links.size() == 1;
        case LinkAction::CopyTargetPath:
            return true;
        case LinkAction::ToRelative:
        case LinkAction::ToAbsolute:
        case LinkAction::ReplaceWithCopy:
            for (const auto& link : links) {
                const auto info = ReadLink(link);
                if (!info) return false;
                if (action == LinkAction::ToAbsolute && info->kind != LinkKind::RelativeSymlink) return false;
                if (action == LinkAction::ToRelative &&
                    (info->kind != LinkKind::Symlink ||
                     !IsSupported(LinkKind::RelativeSymlink, {info->target}, link.parent_path()))) {
                    return false;
                }
                if (action == LinkAction::ReplaceWithCopy && !Exists(info->target)) return false;
            }
            return true;
    }
    return false;
}

void RunAction(LinkAction action, IShellItemArray* items, IUnknown* site) {
    const HWND owner = OwnerWindow(site);
    const auto links = SelectedLinks(items, site);
    if (links.empty()) return;
    switch (action) {
        case LinkAction::OpenTarget: OpenTargetLocation(owner, links.front()); break;
        case LinkAction::CopyTargetPath: CopyTargetPaths(owner, links); break;
        case LinkAction::ChangeTarget: ChangeTarget(owner, links.front(), HelperPath()); break;
        case LinkAction::ToRelative: ConvertLinks(owner, links, LinkKind::RelativeSymlink, HelperPath()); break;
        case LinkAction::ToAbsolute: ConvertLinks(owner, links, LinkKind::Symlink, HelperPath()); break;
        case LinkAction::ReplaceWithCopy: ReplaceWithCopies(owner, links); break;
    }
}

const wchar_t* KindIcon(LinkKind kind) {
    switch (kind) {
        case LinkKind::Symlink: return L"shell32.dll,-51380";
        case LinkKind::RelativeSymlink: return L"shell32.dll,-16801";
        case LinkKind::Junction: return L"shell32.dll,-4";
        case LinkKind::HardLink: return L"shell32.dll,-1";
    }
    return L"shell32.dll,-16769";
}

// Common IObjectWithSite plumbing.
template <typename Derived>
struct CommandBase : winrt::implements<Derived, IExplorerCommand, IObjectWithSite> {
    explicit CommandBase(std::shared_ptr<SiteHolder> shared) : shared_(std::move(shared)) {}

    IFACEMETHODIMP GetToolTip(IShellItemArray*, LPWSTR* tip) override {
        *tip = nullptr;
        return E_NOTIMPL;
    }
    IFACEMETHODIMP GetCanonicalName(GUID* name) override {
        *name = GUID_NULL;
        return E_NOTIMPL;
    }

    IFACEMETHODIMP SetSite(IUnknown* site) override {
        own_site_.copy_from(site);
        if (site) shared_->site.copy_from(site);
        return S_OK;
    }
    IFACEMETHODIMP GetSite(REFIID riid, void** result) override {
        *result = nullptr;
        auto site = Site();
        return site ? site->QueryInterface(riid, result) : E_FAIL;
    }

protected:
    IUnknown* Site() const { return own_site_ ? own_site_.get() : shared_->site.get(); }

    std::shared_ptr<SiteHolder> shared_;
    winrt::com_ptr<IUnknown> own_site_;
};

// Creates one kind of link. Top level ("Paste as symlink") or an item of the "Paste as link" submenu.
struct PasteCommand : CommandBase<PasteCommand> {
    PasteCommand(LinkKind kind, bool topLevel, std::shared_ptr<SiteHolder> shared)
        : CommandBase(std::move(shared)), kind_(kind), top_level_(topLevel) {}

    IFACEMETHODIMP GetTitle(IShellItemArray*, LPWSTR* name) override {
        return SHStrDupW(top_level_ ? L"Paste as symlink" : KindName(kind_).c_str(), name);
    }
    IFACEMETHODIMP GetIcon(IShellItemArray*, LPWSTR* icon) override { return SHStrDupW(KindIcon(kind_), icon); }
    IFACEMETHODIMP GetFlags(EXPCMDFLAGS* flags) override {
        *flags = ECF_DEFAULT;
        return S_OK;
    }
    IFACEMETHODIMP EnumSubCommands(IEnumExplorerCommand** commands) override {
        *commands = nullptr;
        return E_NOTIMPL;
    }

    IFACEMETHODIMP GetState(IShellItemArray* items, BOOL, EXPCMDSTATE* state) override try {
        *state = CanPaste(kind_, items, Site()) ? ECS_ENABLED : (top_level_ ? ECS_HIDDEN : ECS_DISABLED);
        return S_OK;
    } catch (...) {
        *state = ECS_HIDDEN;
        return S_OK;
    }

    IFACEMETHODIMP Invoke(IShellItemArray* items, IBindCtx*) override try {
        PasteLinks(kind_, items, Site());
        return S_OK;
    } catch (...) {
        return winrt::to_hresult();
    }

private:
    LinkKind kind_;
    bool top_level_;
};

struct CommandEnumerator : winrt::implements<CommandEnumerator, IEnumExplorerCommand> {
    CommandEnumerator(std::vector<winrt::com_ptr<IExplorerCommand>> commands, ULONG position = 0)
        : commands_(std::move(commands)), position_(position) {}

    IFACEMETHODIMP Next(ULONG count, IExplorerCommand** result, ULONG* fetched) override {
        ULONG done = 0;
        for (; done < count && position_ < commands_.size(); ++done, ++position_) {
            commands_[position_].copy_to(&result[done]);
        }
        if (fetched) *fetched = done;
        return done == count ? S_OK : S_FALSE;
    }
    IFACEMETHODIMP Skip(ULONG count) override {
        position_ = std::min<size_t>(position_ + count, commands_.size());
        return position_ < commands_.size() ? S_OK : S_FALSE;
    }
    IFACEMETHODIMP Reset() override {
        position_ = 0;
        return S_OK;
    }
    IFACEMETHODIMP Clone(IEnumExplorerCommand** result) override try {
        winrt::make<CommandEnumerator>(commands_, static_cast<ULONG>(position_)).copy_to(result);
        return S_OK;
    } catch (...) {
        return winrt::to_hresult();
    }

private:
    std::vector<winrt::com_ptr<IExplorerCommand>> commands_;
    size_t position_;
};

// "Paste as link" submenu: shown when at least one of its kinds applies.
constexpr LinkKind kSubmenuKinds[] = {LinkKind::RelativeSymlink, LinkKind::Junction, LinkKind::HardLink};

struct LinkMenuCommand : CommandBase<LinkMenuCommand> {
    using CommandBase::CommandBase;

    IFACEMETHODIMP GetTitle(IShellItemArray*, LPWSTR* name) override {
        return SHStrDupW(L"Paste as link", name);
    }
    IFACEMETHODIMP GetIcon(IShellItemArray*, LPWSTR* icon) override { return SHStrDupW(L"shell32.dll,-16769", icon); }
    IFACEMETHODIMP GetFlags(EXPCMDFLAGS* flags) override {
        *flags = ECF_HASSUBCOMMANDS;
        return S_OK;
    }
    IFACEMETHODIMP GetState(IShellItemArray* items, BOOL, EXPCMDSTATE* state) override try {
        bool any = false;
        for (auto kind : kSubmenuKinds) any = any || CanPaste(kind, items, Site());
        *state = any ? ECS_ENABLED : ECS_HIDDEN;
        return S_OK;
    } catch (...) {
        *state = ECS_HIDDEN;
        return S_OK;
    }
    IFACEMETHODIMP EnumSubCommands(IEnumExplorerCommand** result) override try {
        std::vector<winrt::com_ptr<IExplorerCommand>> commands;
        for (auto kind : kSubmenuKinds) {
            commands.push_back(winrt::make<PasteCommand>(kind, false, shared_).as<IExplorerCommand>());
        }
        winrt::make<CommandEnumerator>(std::move(commands)).copy_to(result);
        return S_OK;
    } catch (...) {
        return winrt::to_hresult();
    }
    IFACEMETHODIMP Invoke(IShellItemArray*, IBindCtx*) override { return E_NOTIMPL; }
};

// An action on selected links: "Open target location" at the top level, the rest in "Link options".
struct LinkActionCommand : CommandBase<LinkActionCommand> {
    LinkActionCommand(LinkAction action, bool topLevel, std::shared_ptr<SiteHolder> shared)
        : CommandBase(std::move(shared)), action_(action), top_level_(topLevel) {}

    IFACEMETHODIMP GetTitle(IShellItemArray*, LPWSTR* name) override { return SHStrDupW(ActionTitle(action_), name); }
    IFACEMETHODIMP GetIcon(IShellItemArray*, LPWSTR* icon) override {
        *icon = nullptr;
        return top_level_ ? SHStrDupW(L"shell32.dll,-4", icon) : E_NOTIMPL;
    }
    IFACEMETHODIMP GetFlags(EXPCMDFLAGS* flags) override {
        *flags = ECF_DEFAULT;
        return S_OK;
    }
    IFACEMETHODIMP EnumSubCommands(IEnumExplorerCommand** commands) override {
        *commands = nullptr;
        return E_NOTIMPL;
    }
    IFACEMETHODIMP GetState(IShellItemArray* items, BOOL, EXPCMDSTATE* state) override try {
        const bool enabled = ActionEnabled(action_, SelectedLinks(items, Site()));
        *state = enabled ? ECS_ENABLED : (top_level_ ? ECS_HIDDEN : ECS_DISABLED);
        return S_OK;
    } catch (...) {
        *state = ECS_HIDDEN;
        return S_OK;
    }
    IFACEMETHODIMP Invoke(IShellItemArray* items, IBindCtx*) override try {
        RunAction(action_, items, Site());
        return S_OK;
    } catch (...) {
        return winrt::to_hresult();
    }

private:
    LinkAction action_;
    bool top_level_;
};

// "Link options" submenu, shown when only links are selected.
struct LinkOptionsCommand : CommandBase<LinkOptionsCommand> {
    using CommandBase::CommandBase;

    IFACEMETHODIMP GetTitle(IShellItemArray*, LPWSTR* name) override { return SHStrDupW(L"Link options", name); }
    IFACEMETHODIMP GetIcon(IShellItemArray*, LPWSTR* icon) override { return SHStrDupW(L"shell32.dll,-16769", icon); }
    IFACEMETHODIMP GetFlags(EXPCMDFLAGS* flags) override {
        *flags = ECF_HASSUBCOMMANDS;
        return S_OK;
    }
    IFACEMETHODIMP GetState(IShellItemArray* items, BOOL, EXPCMDSTATE* state) override try {
        *state = SelectedLinks(items, Site()).empty() ? ECS_HIDDEN : ECS_ENABLED;
        return S_OK;
    } catch (...) {
        *state = ECS_HIDDEN;
        return S_OK;
    }
    IFACEMETHODIMP EnumSubCommands(IEnumExplorerCommand** result) override try {
        std::vector<winrt::com_ptr<IExplorerCommand>> commands;
        for (auto action : {LinkAction::CopyTargetPath, LinkAction::ChangeTarget, LinkAction::ToRelative,
                            LinkAction::ToAbsolute, LinkAction::ReplaceWithCopy}) {
            commands.push_back(winrt::make<LinkActionCommand>(action, false, shared_).as<IExplorerCommand>());
        }
        winrt::make<CommandEnumerator>(std::move(commands)).copy_to(result);
        return S_OK;
    } catch (...) {
        return winrt::to_hresult();
    }
    IFACEMETHODIMP Invoke(IShellItemArray*, IBindCtx*) override { return E_NOTIMPL; }
};

// "Move here and leave a symlink", shown after cutting files.
struct MoveAndLinkCommand : CommandBase<MoveAndLinkCommand> {
    using CommandBase::CommandBase;

    IFACEMETHODIMP GetTitle(IShellItemArray*, LPWSTR* name) override {
        return SHStrDupW(L"Move here and leave a symlink", name);
    }
    IFACEMETHODIMP GetIcon(IShellItemArray*, LPWSTR* icon) override { return SHStrDupW(KindIcon(LinkKind::Symlink), icon); }
    IFACEMETHODIMP GetFlags(EXPCMDFLAGS* flags) override {
        *flags = ECF_DEFAULT;
        return S_OK;
    }
    IFACEMETHODIMP EnumSubCommands(IEnumExplorerCommand** commands) override {
        *commands = nullptr;
        return E_NOTIMPL;
    }
    IFACEMETHODIMP GetState(IShellItemArray* items, BOOL, EXPCMDSTATE* state) override try {
        const bool visible = ClipboardIsCut() && CanPaste(LinkKind::Symlink, items, Site());
        *state = visible ? ECS_ENABLED : ECS_HIDDEN;
        return S_OK;
    } catch (...) {
        *state = ECS_HIDDEN;
        return S_OK;
    }
    IFACEMETHODIMP Invoke(IShellItemArray* items, IBindCtx*) override try {
        const HWND owner = OwnerWindow(Site());
        const auto destination = ResolveDestination(items, Site());
        if (!destination.directory) {
            ShowError(owner, destination.multipleSelected ? L"Select a single folder to move the items into."
                                                          : L"This location is not a folder on disk.");
            return S_OK;
        }
        const auto sources = ReadClipboardFiles();
        if (!sources.empty()) MoveAndLeaveLinks(owner, sources, *destination.directory, HelperPath());
        return S_OK;
    } catch (...) {
        return winrt::to_hresult();
    }
};

enum class CommandId { PasteSymlink, PasteLink, OpenTarget, LinkOptions, MoveAndLink };

struct Factory : winrt::implements<Factory, IClassFactory> {
    explicit Factory(CommandId id) : id_(id) {}

    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** result) override try {
        *result = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        auto shared = std::make_shared<SiteHolder>();
        switch (id_) {
            case CommandId::PasteSymlink:
                return winrt::make<PasteCommand>(LinkKind::Symlink, true, shared)->QueryInterface(riid, result);
            case CommandId::PasteLink:
                return winrt::make<LinkMenuCommand>(shared)->QueryInterface(riid, result);
            case CommandId::OpenTarget:
                return winrt::make<LinkActionCommand>(LinkAction::OpenTarget, true, shared)->QueryInterface(riid, result);
            case CommandId::LinkOptions:
                return winrt::make<LinkOptionsCommand>(shared)->QueryInterface(riid, result);
            case CommandId::MoveAndLink:
                return winrt::make<MoveAndLinkCommand>(shared)->QueryInterface(riid, result);
        }
        return CLASS_E_CLASSNOTAVAILABLE;
    } catch (...) {
        return winrt::to_hresult();
    }
    IFACEMETHODIMP LockServer(BOOL lock) override {
        if (lock) {
            ++winrt::get_module_lock();
        } else {
            --winrt::get_module_lock();
        }
        return S_OK;
    }

private:
    CommandId id_;
};

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* result) try {
    *result = nullptr;
    const std::pair<const GUID&, CommandId> classes[] = {
        {kPasteSymlinkClsid, CommandId::PasteSymlink}, {kPasteLinkClsid, CommandId::PasteLink},
        {kOpenTargetClsid, CommandId::OpenTarget},     {kLinkOptionsClsid, CommandId::LinkOptions},
        {kMoveAndLinkClsid, CommandId::MoveAndLink},
    };
    for (const auto& [id, command] : classes) {
        if (IsEqualCLSID(clsid, id)) return winrt::make<Factory>(command)->QueryInterface(riid, result);
    }
    return CLASS_E_CLASSNOTAVAILABLE;
} catch (...) {
    return winrt::to_hresult();
}

STDAPI DllCanUnloadNow() {
    return winrt::get_module_lock() ? S_FALSE : S_OK;
}
