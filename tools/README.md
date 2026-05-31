# tools/

Development-only helpers. **Nothing here is required to build, install, or run
Echobox on the Raspberry Pi** — end users can ignore this directory.

| Subdirectory     | What it is                                                                 |
| ---------------- | -------------------------------------------------------------------------- |
| [`validator/`](validator/) | Python GUI + CLI for offline detector tuning and ALSA loopback emulation (formerly `fake-mic`). |

## Why these live in the same repo

The validator drives the production detector directly through a C ABI
(`tools/validator/native/`) — there is no hand-maintained Python port to drift
out of sync. Keeping the shim, the algorithm plugins, and the validator
front-ends in one repository means any C++ change that affects the detection
contract lands in the same commit and PR as the validator update that
exercises it.

## Build scope

The top-level CMake project does **not** descend into `tools/` for production
builds; adding files here never affects what ships to the Pi. The validator's
own native lib (`tools/validator/native/CMakeLists.txt`) is only added when
the project is configured with `-DECHOBOX_DYNAMIC_PLUGINS=ON` (the mode
`build_dev.sh` uses). The Python tools run from a local virtualenv (e.g.
`pip install -r tools/validator/requirements.txt`).
