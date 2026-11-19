// SymlinkCreator.exe: helper without a console window.
//
//   --apply <request file>              Create the links listed in a request file (used elevated by the DLL).
//   --link <kind> <folder> <source>...  Create links from the command line. Kinds: symlink, relative, junction, hardlink.
//   --info <link>...                    Show the type and target of links.
//   --register                          Register the context menu for the current user.
//   --unregister                        Remove it.
//   --install-cert <file.cer>           Trust the package certificate (LocalMachine\TrustedPeople, needs admin).
//   --remove-cert <file.cer>            Remove it again.

#include <unknwn.h>
#include <windows.h>
#include <wincrypt.h>

#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Management.Deployment.h>

#include <format>
#include <fstream>
#include <iterator>

#include "../common/ids.hpp"
#include "../common/links.hpp"

using namespace symlink_creator;
namespace deployment = winrt::Windows::Management::Deployment;

namespace {

constexpr wchar_t kUsage[] =
    L"Usage:\n"
    L"  SymlinkCreator.exe --link <symlink|relative|junction|hardlink> <folder> <source>...\n"
    L"  SymlinkCreator.exe --info <link>...\n"
    L"  SymlinkCreator.exe --register\n"
    L"  SymlinkCreator.exe --unregister\n"
    L"  SymlinkCreator.exe --install-cert <file.cer>\n"
    L"  SymlinkCreator.exe --remove-cert <file.cer>\n";

bool g_console = false;

// Writes to the console that started us, if any; otherwise shows a message box for errors.
void Report(const std::wstring& text, bool error) {
    // Redirected output (file or pipe): write UTF-8.
    HANDLE redirected = GetStdHandle(STD_OUTPUT_HANDLE);
    const DWORD type = redirected && redirected != INVALID_HANDLE_VALUE ? GetFileType(redirected) : FILE_TYPE_UNKNOWN;
    if (type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE) {
        const std::wstring line = text + L"\r\n";
        const int size = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), nullptr, 0, nullptr, nullptr);
        std::string utf8(size, '\0');
        WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), utf8.data(), size, nullptr, nullptr);
        DWORD written = 0;
        WriteFile(redirected, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
        return;
    }
    if (g_console) {
        HANDLE out = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (out != INVALID_HANDLE_VALUE) {
            const std::wstring line = text + L"\n";
            DWORD written = 0;
            WriteConsoleW(out, line.c_str(), static_cast<DWORD>(line.size()), &written, nullptr);
            CloseHandle(out);
            return;
        }
    }
    if (error) MessageBoxW(nullptr, text.c_str(), kAppName, MB_ICONERROR | MB_OK);
}

path ExeDirectory() {
    std::wstring module(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
        if (length < module.size()) {
            module.resize(length);
            break;
        }
        module.resize(module.size() * 2);
    }
    return path(module).parent_path();
}

bool Exists(const path& p) {
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// ---- Links ---------------------------------------------------------------------------------

int Apply(const path& requestFile) {
    const auto requests = ReadRequestFile(requestFile);
    DeleteFileW(requestFile.c_str());
    if (requests.empty()) return 1;
    const auto results = CreateLinksAndReport(requests, nullptr, std::nullopt);
    for (const auto& result : results) {
        if (result.error) return 1;
    }
    return 0;
}

int Link(const std::vector<std::wstring>& args) {
    if (args.size() < 4) {
        Report(kUsage, true);
        return 2;
    }
    const auto kind = ParseKind(args[1]);
    if (!kind) {
        Report(L"Unknown link type: " + args[1], true);
        return 2;
    }
    const path directory = std::filesystem::absolute(args[2]);
    int failures = 0;
    for (size_t i = 3; i < args.size(); ++i) {
        const auto result = CreateLink(*kind, std::filesystem::absolute(args[i]), directory);
        if (result.error == ERROR_SUCCESS) {
            Report(std::format(L"{} -> {}", result.link.native(), result.source.native()), false);
        } else {
            ++failures;
            Report(std::format(L"FAILED {}: {}", result.source.native(), result.message), true);
        }
    }
    return failures ? 1 : 0;
}

int Info(const std::vector<std::wstring>& args) {
    int failures = 0;
    for (size_t i = 1; i < args.size(); ++i) {
        const path link = std::filesystem::absolute(args[i]);
        if (const auto info = ReadLink(link)) {
            Report(std::format(L"{}: {} -> {} ({}{})", link.native(), KindName(info->kind), info->target.native(),
                               info->rawTarget.native(), Exists(info->target) ? L"" : L", missing"),
                   false);
        } else {
            ++failures;
            Report(link.native() + L": not a symlink or junction", false);
        }
    }
    return failures ? 1 : 0;
}

// ---- Windows 11 context menu (sparse package) ----------------------------------------------

std::wstring DeploymentError(const deployment::DeploymentResult& result) {
    return std::format(L"{} (0x{:08X})", std::wstring(result.ErrorText()),
                       static_cast<uint32_t>(result.ExtendedErrorCode().value));
}

void RemovePackages(deployment::PackageManager& manager) {
    for (const auto& package : manager.FindPackagesForUser(L"", kPackageName, kPackagePublisher)) {
        manager.RemovePackageAsync(package.Id().FullName()).get();
    }
}

int Register() {
    const path directory = ExeDirectory();
    const path dll = directory / L"SymlinkCreator.dll";
    if (!Exists(dll)) {
        Report(L"SymlinkCreator.dll was not found next to SymlinkCreator.exe.", true);
        return 1;
    }

    try {
        deployment::PackageManager manager;
        const winrt::Windows::Foundation::Uri externalLocation(directory.native());
        const path msix = directory / L"SymlinkCreator.msix";
        const path manifest = directory / L"AppxManifest.xml";
        deployment::DeploymentResult result{nullptr};
        if (Exists(msix)) {
            // Signed package: works without Developer Mode once its certificate is trusted.
            RemovePackages(manager);
            deployment::AddPackageOptions options;
            options.ExternalLocationUri(externalLocation);
            result = manager.AddPackageByUriAsync(winrt::Windows::Foundation::Uri(msix.native()), options).get();
        } else if (Exists(manifest)) {
            // Development build: loose manifest, needs Developer Mode.
            deployment::RegisterPackageOptions options;
            options.ExternalLocationUri(externalLocation);
            options.DeveloperMode(true);
            result = manager.RegisterPackageByUriAsync(winrt::Windows::Foundation::Uri(manifest.native()), options).get();
        } else {
            Report(L"Neither SymlinkCreator.msix nor AppxManifest.xml was found next to SymlinkCreator.exe.", true);
            return 1;
        }
        if (FAILED(result.ExtendedErrorCode())) {
            Report(L"Could not register the Windows 11 context menu: " + DeploymentError(result), true);
            return 1;
        }
    } catch (const winrt::hresult_error& e) {
        Report(std::format(L"Could not register the Windows 11 context menu: {} (0x{:08X})", std::wstring(e.message()),
                           static_cast<uint32_t>(e.code().value)),
               true);
        return 1;
    }

    Report(L"SymlinkCreator context menu registered.", false);
    return 0;
}

int Unregister() {
    try {
        deployment::PackageManager manager;
        RemovePackages(manager);
    } catch (const winrt::hresult_error& e) {
        Report(L"Could not remove the Windows 11 context menu: " + std::wstring(e.message()), true);
        return 1;
    }
    Report(L"SymlinkCreator context menu removed.", false);
    return 0;
}

// ---- Certificate ---------------------------------------------------------------------------

int Certificate(const path& file, bool install) {
    std::ifstream stream(file, std::ios::binary);
    const std::vector<BYTE> data((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    if (data.empty()) {
        Report(L"Could not read the certificate " + file.native(), true);
        return 1;
    }
    PCCERT_CONTEXT certificate = CertCreateCertificateContext(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, data.data(),
                                                              static_cast<DWORD>(data.size()));
    if (!certificate) {
        Report(L"Invalid certificate: " + ErrorText(GetLastError()), true);
        return 1;
    }
    // TrustedPeople is enough for MSIX signatures and, unlike Root, does not trust the key for anything else.
    HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_LOCAL_MACHINE, L"TrustedPeople");
    DWORD error = ERROR_SUCCESS;
    if (!store) {
        error = GetLastError();
    } else if (install) {
        if (!CertAddCertificateContextToStore(store, certificate, CERT_STORE_ADD_REPLACE_EXISTING, nullptr)) {
            error = GetLastError();
        }
    } else if (PCCERT_CONTEXT found =
                   CertFindCertificateInStore(store, X509_ASN_ENCODING, 0, CERT_FIND_EXISTING, certificate, nullptr)) {
        if (!CertDeleteCertificateFromStore(found)) error = GetLastError();  // Frees `found`.
    }
    if (store) CertCloseStore(store, 0);
    CertFreeCertificateContext(certificate);
    if (error) {
        Report(L"Could not update the trusted certificates: " + ErrorText(error), true);
        return 1;
    }
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    g_console = AttachConsole(ATTACH_PARENT_PROCESS) != FALSE;

    int count = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &count);
    std::vector<std::wstring> args(argv + 1, argv + count);
    LocalFree(argv);

    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        const std::wstring command = args.empty() ? L"" : args[0];
        if (command == L"--apply" && args.size() == 2) return Apply(args[1]);
        if (command == L"--link") return Link(args);
        if (command == L"--info") return Info(args);
        if (command == L"--register") return Register();
        if (command == L"--unregister") return Unregister();
        if (command == L"--install-cert" && args.size() == 2) return Certificate(args[1], true);
        if (command == L"--remove-cert" && args.size() == 2) return Certificate(args[1], false);
    } catch (const winrt::hresult_error& e) {
        Report(std::wstring(e.message()), true);
        return 1;
    } catch (const std::exception& e) {
        Report(winrt::to_hstring(e.what()).c_str(), true);
        return 1;
    }
    Report(kUsage, true);
    return 2;
}
