#include "core/types.h"

namespace gk {

const char* toString(RefKind kind) {
    switch (kind) {
    case RefKind::LocalBranch: return "local";
    case RefKind::RemoteBranch: return "remote";
    case RefKind::Tag: return "tag";
    case RefKind::Other: return "other";
    }
    return "other";
}

const char* toString(ChangeKind kind) {
    switch (kind) {
    case ChangeKind::Added: return "added";
    case ChangeKind::Modified: return "modified";
    case ChangeKind::Deleted: return "deleted";
    case ChangeKind::Renamed: return "renamed";
    case ChangeKind::TypeChange: return "typechange";
    case ChangeKind::Untracked: return "untracked";
    case ChangeKind::Conflicted: return "conflicted";
    }
    return "modified";
}

const char* toString(RepoOperation op) {
    switch (op) {
    case RepoOperation::None: return "none";
    case RepoOperation::Merge: return "merge";
    case RepoOperation::Rebase: return "rebase";
    case RepoOperation::CherryPick: return "cherry-pick";
    case RepoOperation::Revert: return "revert";
    }
    return "none";
}

const char* toString(MergeStatus status) {
    switch (status) {
    case MergeStatus::UpToDate: return "up-to-date";
    case MergeStatus::FastForward: return "fast-forward";
    case MergeStatus::Clean: return "clean";
    case MergeStatus::Conflicted: return "conflicted";
    }
    return "unknown";
}

const char* toString(ConflictKind kind) {
    switch (kind) {
    case ConflictKind::BothModified: return "both modified";
    case ConflictKind::BothAdded: return "both added";
    case ConflictKind::DeletedByUs: return "deleted by us";
    case ConflictKind::DeletedByThem: return "deleted by them";
    case ConflictKind::BothDeleted: return "both deleted";
    case ConflictKind::AddedByUs: return "added by us";
    case ConflictKind::AddedByThem: return "added by them";
    case ConflictKind::Unknown: return "conflict";
    }
    return "conflict";
}

const char* toString(RebaseStatus status) {
    switch (status) {
    case RebaseStatus::UpToDate: return "up-to-date";
    case RebaseStatus::Completed: return "completed";
    case RebaseStatus::Conflicted: return "conflicted";
    }
    return "unknown";
}

const char* toString(SubmoduleState state) {
    switch (state) {
    case SubmoduleState::InSync: return "in-sync";
    case SubmoduleState::NotInitialized: return "not-initialized";
    case SubmoduleState::OutOfSync: return "out-of-sync";
    case SubmoduleState::Conflict: return "conflict";
    }
    return "unknown";
}

} // namespace gk
