# EmbLinkOS integration

Upstream: https://github.com/emersion/mrsh
Revision: 4c81598721bc5eeb28f9faa818b3102d0471b7f6
License: MIT (see LICENSE).

The parser, AST, expansion engine and shell builtins originate upstream.
EmbLinkOS adapts process execution to explicit POSIX spawn, with its port layer
in ports/mrsh. Worker state and typed ASTs travel through an unlinked temporary
file descriptor. Native job-control and in-place exec remain unsupported.
Local changes are tracked directly in this vendored tree; see the repository
POSIX shell documentation for the tested scope and remaining upstream limits.
