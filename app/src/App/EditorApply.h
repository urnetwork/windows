// Whether a settings editor's Update may write (owner report 2026-10-16): the
// DNS resolver editor and the transport policy editor open on a reading of the
// settings, and an Update writes the whole value. Opened over no reading, a
// form on blank or default values would overwrite the real resolver or policy.
//
// So an editor carries the reading it opened on, and the writer reads the
// settings again, fresh, under the lock it writes under. It writes only when
// that reading is known and equals the one the editor opened on; otherwise it
// writes nothing, and the views take the fresh reading, so the editor opens on
// it next time.
//
// A DNS reading is known only when it holds a value: a live device always
// holds resolver settings (DeviceLocal starts from the connect package's
// default upgrade mux settings), so none means no session or a read with
// nothing known, and the DNS editor takes no Update over it at all. A
// transport reading of none can be true, the default policy in force, so it
// counts when the writer says it is the device's or the stored mirror's.
//
// Pure, no Windows dependencies; tools/editor-apply-tests.cpp is the spec.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <optional>

namespace urnw::editorapply {

enum class Step {
  Write,   // the settings are what the editor opened on: write the draft
  Reseed,  // they changed or are unknown: write nothing, take the fresh reading
};

// `opened` is the reading the editor opened on, `current` the writer's fresh
// one, which `currentKnown` says is the settings' own and not a fallback for a
// read that failed; `equal` compares two values.
template <class T, class Equal>
constexpr Step Decide(const std::optional<T>& opened, const std::optional<T>& current,
                      bool currentKnown, Equal equal) {
  if (!currentKnown) return Step::Reseed;
  if (opened.has_value() != current.has_value()) return Step::Reseed;
  if (opened && !equal(*opened, *current)) return Step::Reseed;
  return Step::Write;
}

// Whether the DNS editor may take an Update over `reading`, and whether a
// fresh DNS reading is known.
template <class T>
constexpr bool DnsReadingKnown(const std::optional<T>& reading) {
  return reading.has_value();
}

}  // namespace urnw::editorapply
