# SPDX-FileCopyrightText: 2026 The Echobox Authors
# SPDX-License-Identifier: GPL-3.0-or-later

"""``python -m validator`` → launch the GUI."""
import sys
from .gui import main

if __name__ == "__main__":
    sys.exit(main())
