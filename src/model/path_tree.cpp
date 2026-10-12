#include "model/path_tree.h"

#include <algorithm>

namespace gk {
namespace {

PathFolder* findOrAddFolder(const QString& prefix, const QString& label, PathFolder* parent) {
    for (PathFolder& folder : parent->folders) {
        if (folder.prefix == prefix) {
            return &folder;
        }
    }
    PathFolder child;
    child.label = label;
    child.prefix = prefix;
    parent->folders.push_back(std::move(child));
    return &parent->folders.back();
}

void mergeChains(PathFolder& folder) {
    for (PathFolder& child : folder.folders) {
        mergeChains(child);
    }
    // Repeated because merging can expose another single-child folder below.
    for (PathFolder& child : folder.folders) {
        while (child.items.empty() && child.folders.size() == 1) {
            PathFolder only = std::move(child.folders.front());
            child.label = child.label + QLatin1Char('/') + only.label;
            child.prefix = only.prefix;
            child.items = std::move(only.items);
            child.folders = std::move(only.folders);
            mergeChains(child);
        }
    }
}

void sortFolders(PathFolder& folder) {
    std::sort(folder.folders.begin(), folder.folders.end(),
              [](const PathFolder& a, const PathFolder& b) {
                  return QString::compare(a.label, b.label, Qt::CaseInsensitive) < 0;
              });
    for (PathFolder& child : folder.folders) {
        sortFolders(child);
    }
}

} // namespace

PathFolder buildPathTree(const QStringList& names, const std::vector<int>& items) {
    PathFolder root;
    const qsizetype count = std::min<qsizetype>(names.size(), static_cast<qsizetype>(items.size()));
    // Parallel lists: pairing fewer than we have would drop items silently.
    Q_ASSERT(names.size() == static_cast<qsizetype>(items.size()));
    for (qsizetype i = 0; i < count; ++i) {
        const QStringList parts = names.at(i).split(QLatin1Char('/'), Qt::SkipEmptyParts);
        PathFolder* current = &root;
        QString path;
        for (int part = 0; part + 1 < parts.size(); ++part) {
            path += parts.at(part) + QLatin1Char('/');
            current = findOrAddFolder(path, parts.at(part), current);
        }
        current->items.push_back(items.at(static_cast<std::size_t>(i)));
    }
    mergeChains(root);
    sortFolders(root);
    return root;
}

} // namespace gk
