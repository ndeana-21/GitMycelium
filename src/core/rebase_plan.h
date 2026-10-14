#pragma once

#include "core/types.h"

#include <string>
#include <vector>

namespace gk {

/// One row of an interactive rebase plan: a commit, what should happen to it, and — for
/// a reword — the message that replaces the original.
///
/// The order of the rows is the order git will replay them, oldest first, which is the
/// order git's own todo lists them. The rows decide that order; nothing here sorts.
struct RebasePlanRow {
    Oid id;
    std::string subject;
    RewriteAction action = RewriteAction::Keep;
    std::string message; ///< used by Reword only
};

std::vector<RebasePlanRow> defaultRebasePlan(const std::vector<RebaseCandidate>& candidates);

/// The one-word name of a rewrite action (`pick`, `reword`, `edit`, `squash`, `fixup`,
/// `drop`), and its inverse. Shared so the editor, the plan text and the spec parser all
/// spell the actions the same way.
const char* rewriteActionName(RewriteAction action);
bool rewriteActionFromName(const std::string& name, RewriteAction* action);

/// Why the plan cannot be run, as a sentence for the reader — or empty when it can.
///
/// The rules are git's own, stated before it has to: the first step cannot fold into
/// nothing, at least one commit must survive, and a reword has to say what to say.
std::string rebasePlanRefusal(const std::vector<RebasePlanRow>& rows);

/// The rows as rewrite steps, in order. The set is what the plan names; the caller
/// compares it against the range before running (see Repository::rewriteHistory).
std::vector<RewriteStep> rebasePlanSteps(const std::vector<RebasePlanRow>& rows);

/// Parses a compact plan against the candidates: comma-separated `action:sha[=message]`
/// entries in the final order, where `sha` may be an abbreviated, unambiguous prefix of a
/// candidate's id. Every candidate must appear exactly once. On anything unclear the rows
/// are empty and `*errorOut` says why.
std::vector<RebasePlanRow> parseRebaseSpec(const std::string& spec,
                                           const std::vector<RebaseCandidate>& candidates,
                                           std::string* errorOut);

std::string rebasePlanText(const std::vector<RebasePlanRow>& rows);

} // namespace gk
