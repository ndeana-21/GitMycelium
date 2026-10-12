#include "core/rebase_plan.h"

#include <algorithm>
#include <cctype>

namespace gk {
namespace {

std::string trimmed(const std::string& text) {
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t");
    return text.substr(first, last - first + 1);
}

std::vector<std::string> split(const std::string& text, char separator) {
    std::vector<std::string> parts;
    std::string current;
    for (char c : text) {
        if (c == separator) {
            parts.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    parts.push_back(current);
    return parts;
}

} // namespace

const char* rewriteActionName(RewriteAction action) {
    switch (action) {
    case RewriteAction::Keep: return "pick";
    case RewriteAction::Drop: return "drop";
    case RewriteAction::Squash: return "squash";
    case RewriteAction::Fixup: return "fixup";
    case RewriteAction::Reword: return "reword";
    case RewriteAction::Edit: return "edit";
    }
    return "pick";
}

bool rewriteActionFromName(const std::string& name, RewriteAction* action) {
    if (name == "pick" || name == "p" || name == "keep") {
        *action = RewriteAction::Keep;
    } else if (name == "drop" || name == "d") {
        *action = RewriteAction::Drop;
    } else if (name == "squash" || name == "s") {
        *action = RewriteAction::Squash;
    } else if (name == "fixup" || name == "f") {
        *action = RewriteAction::Fixup;
    } else if (name == "reword" || name == "r") {
        *action = RewriteAction::Reword;
    } else if (name == "edit" || name == "e") {
        *action = RewriteAction::Edit;
    } else {
        return false;
    }
    return true;
}

std::vector<RebasePlanRow> defaultRebasePlan(const std::vector<RebaseCandidate>& candidates) {
    std::vector<RebasePlanRow> rows;
    rows.reserve(candidates.size());
    for (const RebaseCandidate& candidate : candidates) {
        RebasePlanRow row;
        row.id = candidate.id;
        row.subject = candidate.subject;
        row.action = RewriteAction::Keep;
        rows.push_back(std::move(row));
    }
    return rows;
}

std::string rebasePlanRefusal(const std::vector<RebasePlanRow>& rows) {
    if (rows.empty()) {
        return "an interactive rebase needs at least one commit";
    }
    if (rows.front().action == RewriteAction::Squash ||
        rows.front().action == RewriteAction::Fixup) {
        return "the first commit cannot be squashed or fixed up — there is nothing below it";
    }
    const bool anySurvives =
        std::any_of(rows.begin(), rows.end(), [](const RebasePlanRow& row) {
            return row.action != RewriteAction::Drop;
        });
    if (!anySurvives) {
        return "every commit is dropped — the rebase would leave the branch empty";
    }
    for (const RebasePlanRow& row : rows) {
        if (row.action == RewriteAction::Reword && trimmed(row.message).empty()) {
            return "a reword needs a message before the rebase can run";
        }
    }
    return {};
}

std::vector<RewriteStep> rebasePlanSteps(const std::vector<RebasePlanRow>& rows) {
    std::vector<RewriteStep> steps;
    steps.reserve(rows.size());
    for (const RebasePlanRow& row : rows) {
        RewriteStep step{};
        step.commit = row.id;
        step.action = row.action;
        if (row.action == RewriteAction::Reword) {
            step.message = row.message;
        }
        steps.push_back(std::move(step));
    }
    return steps;
}

std::vector<RebasePlanRow> parseRebaseSpec(const std::string& spec,
                                           const std::vector<RebaseCandidate>& candidates,
                                           std::string* errorOut) {
    const auto refuse = [errorOut](const std::string& why) {
        if (errorOut) {
            *errorOut = why;
        }
        return std::vector<RebasePlanRow>{};
    };
    if (candidates.empty()) {
        return refuse("the range holds no commits");
    }
    if (trimmed(spec).empty()) {
        return refuse("no plan was given");
    }

    std::vector<bool> used(candidates.size(), false);
    std::vector<RebasePlanRow> rows;
    for (const std::string& rawToken : split(spec, ',')) {
        const std::string token = trimmed(rawToken);
        if (token.empty()) {
            continue;
        }

        RewriteAction action = RewriteAction::Keep;
        std::string rest = token;
        const std::size_t colon = token.find(':');
        if (colon != std::string::npos) {
            const std::string word = trimmed(token.substr(0, colon));
            if (!rewriteActionFromName(word, &action)) {
                return refuse("unknown action '" + word + "'");
            }
            rest = token.substr(colon + 1);
        }

        std::string sha = rest;
        std::string message;
        const std::size_t equals = rest.find('=');
        if (equals != std::string::npos) {
            sha = rest.substr(0, equals);
            message = rest.substr(equals + 1);
        }
        sha = trimmed(sha);

        int match = -1;
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            if (candidates[i].id.rfind(sha, 0) == 0) {
                if (match >= 0) {
                    return refuse("'" + sha + "' is an ambiguous prefix");
                }
                match = static_cast<int>(i);
            }
        }
        if (match < 0) {
            return refuse("'" + sha + "' names no commit in the range");
        }
        if (used[static_cast<std::size_t>(match)]) {
            return refuse("'" + sha + "' is named more than once");
        }
        used[static_cast<std::size_t>(match)] = true;

        RebasePlanRow row;
        row.id = candidates[static_cast<std::size_t>(match)].id;
        row.subject = candidates[static_cast<std::size_t>(match)].subject;
        row.action = action;
        row.message = message;
        rows.push_back(std::move(row));
    }

    if (rows.size() != candidates.size()) {
        return refuse("every commit in the range must appear exactly once (" +
                      std::to_string(rows.size()) + " of " +
                      std::to_string(candidates.size()) + " named)");
    }
    return rows;
}

std::string rebasePlanText(const std::vector<RebasePlanRow>& rows) {
    std::string out;
    for (const RebasePlanRow& row : rows) {
        out += rewriteActionName(row.action);
        out += ' ';
        out += row.id;
        out += ' ';
        out += row.subject;
        out += '\n';
    }
    return out;
}

} // namespace gk
