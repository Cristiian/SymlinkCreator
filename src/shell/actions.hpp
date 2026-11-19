// Context menu actions that need the shell (dialogs, copy/move engine). Used by shell.cpp.
#pragma once

#include "../common/links.hpp"

namespace symlink_creator {

// Opens the folder that holds the target of `link`, with the target selected.
void OpenTargetLocation(HWND owner, const path& link);

// Puts the absolute targets of `links` on the clipboard, one per line.
void CopyTargetPaths(HWND owner, const std::vector<path>& links);

// Lets the user pick a new target for `link` and recreates it with the same link type.
void ChangeTarget(HWND owner, const path& link, const std::optional<path>& helper);

// Recreates each link as `kind` (absolute or relative symlink) with the same target.
void ConvertLinks(HWND owner, const std::vector<path>& links, LinkKind kind, const std::optional<path>& helper);

// Replaces each link with a real copy of its target.
void ReplaceWithCopies(HWND owner, const std::vector<path>& links);

// Moves `sources` into `destination` and leaves an absolute symlink at each original path.
void MoveAndLeaveLinks(HWND owner, const std::vector<path>& sources, const path& destination,
                       const std::optional<path>& helper);

}  // namespace symlink_creator
