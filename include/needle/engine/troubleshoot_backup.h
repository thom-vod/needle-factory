#pragma once

#include <string>
#include <vector>

#include "needle/model/result.h"

namespace needle {

// Backup-branch isolation primitive for troubleshooter sessions that
// mutate project state (Tweak and Full tiers). Diagnose does not use
// this — it is read-only.
//
// capture(): creates `auto/troubleshoot/backup/<run-id>-<session-id>`
// at the current HEAD and records the base SHA + the list of
// pre-existing untracked files under the session directory. These
// records let rollback() return tracked files to their pre-agent
// state and report untracked drift without clobbering the operator's
// own untracked files.

struct BackupInfo {
    std::string branch;       // ref name, empty when no backup created
    std::string base_sha;     // pre-agent HEAD, empty when no backup created
};

struct RollbackReport {
    std::string branch;
    std::string base_sha;
    std::vector<std::string> untracked_drift; // files present now, absent at capture
};

class TroubleshootBackup {
public:
    // Create the backup branch and write `backup-base.txt` and
    // `pre-untracked.txt` under session_dir. Returns the resulting
    // BackupInfo or a Result-failure error message.
    static Result<BackupInfo> capture(const std::string& project_dir,
                                      const std::string& run_id,
                                      const std::string& session_id,
                                      const std::string& session_dir);

    // Restore tracked files to backup-base, delete the backup branch,
    // and report untracked-drift files (does not delete them — that
    // is the operator's call).
    static Result<RollbackReport> rollback(const std::string& project_dir,
                                           const std::string& session_dir);
};

} // namespace needle
