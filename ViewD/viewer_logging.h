// PyrA Project, Git ap795/PyrA
// Copyright (c) 2026 Alexandre Papine
// SPDX-License-Identifier: MIT

#pragma once

#include <QString>

namespace pyra::test {

// Process-lifetime logging; call once before constructing QApplication.
bool start_viewer_log ( const QString &directory, QString *path, QString *error );

} // namespace pyra::test
