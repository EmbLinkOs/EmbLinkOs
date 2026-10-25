# EmbLinkOS integration

Upstream: https://github.com/emersion/mrsh
Revision: 4c81598721bc5eeb28f9faa818b3102d0471b7f6
License: MIT (see LICENSE).

The parser, AST, expansion engine and shell builtins originate upstream.
EmbLinkOS adapts process execution to explicit POSIX spawn, with its port layer
in ports/mrsh. Worker state and typed ASTs travel through an unlinked temporary
file descriptor. Native job-control and in-place exec remain unsupported.
The pristine import is committed separately from the native adaptations.
UPSTREAM.json records the pinned archive and original pathname helper hashes.
Local changes are tracked directly in this vendored tree; see
[POSIX_SHELL.md](../../docs/POSIX_SHELL.md) for the tested scope, supported
options and remaining upstream limits. The MIT notice is also installed under
/system/licenses in the OS image.
