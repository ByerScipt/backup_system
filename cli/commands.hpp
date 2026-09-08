#pragma once
struct Arguments;
int localBackup(const Arguments& args);
int localRestore(const Arguments& args);
int inspectArchive(const Arguments& args);
int registerUser(const Arguments& args);
int remoteBackup(const Arguments& args);
int remoteList(const Arguments& args);
int remoteRestore(const Arguments& args);
