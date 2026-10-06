#pragma once
// Non-root guidance shared by every privileged screen: a one-shot modal on
// first entry plus a persistent banner (sudo hint + elevate restart button).
// The button label / wording is platform-specific (Administrator vs root)
// via IPlatform::elevateActionMsgId()/elevateDeclinedMsgId().
namespace bootroll {

class App;

namespace elevate {

// Persistent non-root banner at the top of a privileged page. No-op when
// elevated. Shows the declined-feedback line after a failed restart attempt.
void drawBanner(App& app);

// One-shot ImGui modal when the user first enters a privileged page without
// elevation (once per page per process). No-op when elevated.
void maybeShowModal(App& app);

} // namespace elevate
} // namespace bootroll
