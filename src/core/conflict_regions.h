#pragma once

#include "core/types.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace gk {

/// One conflict region in a file git left marked up. The three side vectors hold
/// whole lines including their line endings; an absent side (a deletion, or a
/// side that was never in the index) is empty.
struct ConflictRegion {
    std::vector<std::string> ours;
    std::vector<std::string> base;
    std::vector<std::string> theirs;
    bool hasBase = false;  ///< the `|||||||` section was present (diff3 style)
    bool balanced = true;  ///< all four markers were found
    std::size_t firstLine = 0;
    std::size_t lineCount = 0;
    /// The block exactly as it was, markers included. An unresolved region is put back
    /// byte for byte from here rather than reconstructed from the sides.
    std::vector<std::string> raw;
};

/// A run of the marked file: either plain text or one conflict region, in order.
struct ConflictPiece {
    bool isRegion = false;
    std::vector<std::string> lines; ///< plain text when `!isRegion`
    std::size_t region = 0;         ///< index into ConflictDocument::regions
};

struct ConflictDocument {
    std::vector<ConflictPiece> pieces;
    std::vector<ConflictRegion> regions;
    /// True when every region closed its markers. A file with an unbalanced block
    /// is already broken and is handed back untouched.
    bool balanced() const;
};

enum class ConflictMarker { None, Ours, Base, Split, Theirs };

/// git's default marker length, and the shortest run a marker can be.
constexpr std::size_t kDefaultConflictMarkerSize = 7;

/// The length of the marker run when `line` opens a conflict block (`<<<<<<<` and
/// longer, then a space or the end of the line), or 0. The opening marker sets the
/// length the rest of the block's markers must have.
std::size_t conflictOpeningSize(std::string_view line);

/// Which marker `line` is, by git's rule: exactly `size` marker characters, then a
/// space or the end of the line. A longer run — `================` under a heading —
/// is text, not a separator.
ConflictMarker conflictMarkerOf(std::string_view line,
                                std::size_t size = kDefaultConflictMarkerSize);

/// A marker size no line of `texts` can be mistaken for: one longer than the longest
/// run of marker characters any line starts with, and never below git's default.
std::size_t safeConflictMarkerSize(const std::vector<std::string_view>& texts);

/// Splits diff3-marked text into plain runs and conflict regions, keeping the
/// bytes of each line exactly as they were.
///
/// This only says what the regions are; the automatic sweep (`conflict_auto`) decides
/// which need nobody, so the editor can offer them one at a time without a second opinion
/// about how to settle them.
ConflictDocument parseConflictDocument(const std::string& marked);

/// Where each region sits in our side of the file, as line numbers a reader can go to:
/// the plain runs and the regions' our-side lines, counted in order.
std::vector<LineSpan> oursLineSpans(const ConflictDocument& document);

/// What a region's answer is. `Lines` is the per-line one: some lines of each
/// side are taken and the rest dropped, which a single whole-side choice cannot
/// express.
enum RegionChoice {
    RegionUnanswered = 0,
    RegionOurs = 1,
    RegionBase = 2,
    RegionTheirs = 3,
    RegionBoth = 4,
    RegionBothReverse = 5,
    RegionLines = 6,
};

/// Which lines of one region the reader has taken, side by side.
///
/// The flag at index `i` says whether line `i` of that side goes into the result.
/// Untaken lines are dropped, so a region can keep part of ours and part of
/// theirs — the per-line answer the whole-side chips cannot give.
struct RegionLineTake {
    std::vector<char> ours;
    std::vector<char> theirs;
    /// Which side's taken lines come first. A region where one side's line is a
    /// continuation of the other's is the case the reader cares about, and the
    /// order is theirs to pick.
    bool oursFirst = true;

    void resize(const ConflictRegion& region);
    bool anyTaken() const;
    bool isTaken(bool oursSide, std::size_t line) const;
    void toggle(bool oursSide, std::size_t line);
    void takeAll(bool oursSide, bool take);
};

/// The lines a choice puts into the result, in order.
///
/// For `RegionLines` this is the taken lines of each side; for a whole side it is
/// that side's lines; for `RegionBoth`/`RegionBothReverse` it is both sides with
/// the order reversed rather than the text. `RegionUnanswered` yields nothing:
/// the caller keeps the original marked bytes instead.
std::vector<std::string> regionChosenLines(const ConflictRegion& region, int choice,
                                           const RegionLineTake& lines);

/// True when the choice settles the region. An unbalanced block never is, and a
/// per-line answer with nothing taken leaves the region open.
bool regionResolved(const ConflictRegion& region, int choice, const RegionLineTake& lines);

} // namespace gk
