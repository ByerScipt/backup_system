#pragma once
#include "backup/core.hpp"
#include <string>
struct Arguments;
void progressReport(const backup::ProgressEvent& event);
std::string temporaryArchive();
int localBackup(const Arguments& args);
int localRestore(const Arguments& args);
int inspectArchive(const Arguments& args);
int registerUser(const Arguments& args);
int remoteBackup(const Arguments& args);
int remoteList(const Arguments& args);
int remoteRestore(const Arguments& args);
