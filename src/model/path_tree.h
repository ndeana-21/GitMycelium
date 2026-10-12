#pragma once

#include <QString>
#include <QStringList>

#include <vector>

namespace gk {

struct PathFolder {
    QString label;          ///< shown to the user; merged chains are already joined
    QString prefix;         ///< the whole prefix, with a trailing slash
    std::vector<int> items; ///< indices into whatever list the caller passed in
    std::vector<PathFolder> folders;

    bool empty() const { return items.empty() && folders.empty(); }
};

/// Folds a list of slashed names into folders.
///
/// `names` and `items` are parallel: `items.at(i)` belongs to `names.at(i)` and
/// is whatever index the caller wants back. The order inside a folder is the
/// order of the input, so the caller decides it by sorting first.
///
/// A chain of folders that each hold nothing but the next folder becomes a single
/// folder: `origin` + `feature` is one node called `origin/feature`. Without that,
/// a branch called `origin/feature/wip` costs three levels in the tree and the
/// folders stop earning the space they take.
///
/// One implementation for every list that groups by path — the branch panel and
/// the commit's file list both do, and two copies of these rules drift apart.
PathFolder buildPathTree(const QStringList& names, const std::vector<int>& items);

} // namespace gk
