#pragma once
#include <string>

namespace dump {
void Names(const std::wstring& dir);    // names.txt: every FName
void Objects(const std::wstring& dir);  // objects.txt (all objects) and classes.txt (classes, structs, enums)
void KnownOffsets();                    // logs whether other OT6.5 projects' hardcoded addresses match this exe
void MissionClasses();                  // logs every loaded class in the mission-related families, with its parent
}  // namespace dump
