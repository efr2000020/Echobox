# SPDX-FileCopyrightText: 2026 The Echobox Authors
# SPDX-License-Identifier: GPL-3.0-or-later

"""Session-screen: rough per-file recall / cricket-FP screen by comparing
Echobox's would-save decisions against BatDetect2 on the same raw session
recordings.

Not a certification, and NOT the same tool as ``tools/validator/``:

- ``tools/validator/`` — precise per-event validation of the detector
  against a curated, annotated single-clip corpus. Use it to justify a
  tunable change with numbers.
- ``tools/session_screen/`` (this package) — coarse per-file screen of a
  whole field session against a proxy-truth model. Use it to sanity-check
  a night's recordings and prioritise which clips a human should listen to.

See ``README.md`` and the two caveats the ``score`` subcommand prints at
the top of every report.
"""
