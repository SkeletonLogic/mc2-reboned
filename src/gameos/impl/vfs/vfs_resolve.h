//===========================================================================//
// Shared GameOS path-resolution policy. Every VFS backend receives the same
// normalized, case-resolved paths and enumeration filtering.
//===========================================================================//

#pragma once

#include <string>
#include <vector>

std::string VfsNormalizePath(const char* path);
std::string VfsResolvePath(const char* path);
void VfsFindEntries(const char* pathPattern, bool wantDirectories,
                    std::vector<std::string>& entries);
bool VfsResolvedIsDirectory(const std::string& resolved);
void VfsInvalidatePathCaches();
void VfsNoteWholeFileSingleOpen();
void VfsDumpStats();
