These fnmatch/glob implementations are from newlib 4.5.0.20241231,
newlib/libc/posix/{fnmatch,glob}.c. Their original BSD notices are retained in
both files. The private collate.h selects C-locale byte ordering; other collation
locales are not implemented. They are linked only into the shell port because
the bare-metal newlib archive omits these POSIX objects. Native glob is built at
EL/IX level 1, without user-database tilde lookup; the shell performs its own
HOME-based tilde expansion. Host validation uses the same imported matchers.
