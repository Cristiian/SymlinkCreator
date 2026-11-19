// COM class IDs of the context menu commands. Keep in sync with package/AppxManifest.xml.
#pragma once

#include <guiddef.h>

namespace symlink_creator {

// "Paste as symlink". {6FBEED93-1EFD-4B46-9CA7-EA8E262AA432}
inline constexpr GUID kPasteSymlinkClsid{0x6FBEED93, 0x1EFD, 0x4B46, {0x9C, 0xA7, 0xEA, 0x8E, 0x26, 0x2A, 0xA4, 0x32}};
// "Paste as link" submenu. {A420FCAD-87A0-462A-B546-EAEFADD3D8E1}
inline constexpr GUID kPasteLinkClsid{0xA420FCAD, 0x87A0, 0x462A, {0xB5, 0x46, 0xEA, 0xEF, 0xAD, 0xD3, 0xD8, 0xE1}};

// "Open target location" on a selected link. {203DB7B1-B02D-4D5B-97DF-8F7007D4571D}
inline constexpr GUID kOpenTargetClsid{0x203DB7B1, 0xB02D, 0x4D5B, {0x97, 0xDF, 0x8F, 0x70, 0x07, 0xD4, 0x57, 0x1D}};
// "Link options" submenu on selected links. {19947614-650F-4A69-AFE7-8A0641904447}
inline constexpr GUID kLinkOptionsClsid{0x19947614, 0x650F, 0x4A69, {0xAF, 0xE7, 0x8A, 0x06, 0x41, 0x90, 0x44, 0x47}};
// "Move here and leave a symlink" after cutting files. {8499F34C-A9D4-4F35-AA68-2A1CCD54C632}
inline constexpr GUID kMoveAndLinkClsid{0x8499F34C, 0xA9D4, 0x4F35, {0xAA, 0x68, 0x2A, 0x1C, 0xCD, 0x54, 0xC6, 0x32}};

// Package identity. Publisher must match the subject of the signing certificate.
inline constexpr wchar_t kPackageName[] = L"SymlinkCreator";
inline constexpr wchar_t kPackagePublisher[] = L"CN=SymlinkCreator";

}  // namespace symlink_creator
