#pragma once

class QApplication;

namespace gk {

/// Reads the saved theme mode and zoom from `QSettings` into the `theme` module.
/// The window and `main()` share it so a fresh start and a preference change
/// cannot read the profile two different ways.
void loadSavedAppearance();

/// Applies the active theme and zoom to the whole application in one place:
/// palette, base font and stylesheet together. `main()` calls it once at start
/// and the window calls it again after a preference changes, so the two cannot
/// drift apart about what "the look" is. The style itself (Fusion) is a
/// `main()` decision and stays there.
void applyApplicationTheme(QApplication* app);

} // namespace gk
