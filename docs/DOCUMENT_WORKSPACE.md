# Documents and recoverable workspaces

Files can create a text document in the current folder and open it in Editor.
Editor and Note++ use the same session Open/Save service, start in the user's
home or the current document's directory, and offer Home, Documents, Downloads
and Desktop. The panel can create folders. Save requires a filename component;
existing files require an explicit Replace answer. Cancelling leaves the app's
path and dirty state intact. Browser sources are outside this milestone.

## Saving a document

`user/lib/document.c` is shared by both apps through libembk.so. It creates an
exclusive sibling temporary file, handles interrupted and short writes, checks
fsync and close, renames over the destination, then fsyncs the containing
folder. It never truncates the original to begin a save. A failure before the
rename removes only the temporary file. A failure after rename is reported as
a failure, and the app retains dirty state because durability was not proven.
The fd fsync path now calls the filesystem sync operation even for a clean
file or a directory. Short backend writeback leaves pages dirty and returns an
error instead of marking the incomplete write clean. `test document sync`
exercises both boundaries with a private mount and descriptor fixture.

Replacement follows EMBKFS v2.3 §1: the directory name changes in a single
B-tree commit. EMBKFS's commit barriers remain the storage durability boundary;
this does not establish physical power-loss behavior for arbitrary hardware.

A stamp records existence, byte count and a 64-bit content hash. Save checks it
before writing and again before replacement. Ordinary external edits cause an
explicit conflict, preserving the editor buffer and the other writer's file;
Save As can choose another destination. This is advisory conflict detection,
not an atomic filesystem compare-and-swap. A writer racing the final check and
rename remains a gap. Separate tabs cannot Save As onto a file already open in
the same Note++ instance. Symlinks and nonregular destinations are refused.

Opening checks regular-file type, embedded NUL and the full size before
replacing an existing buffer. Oversized and binary documents are refused,
never silently truncated and subsequently saved. Current text capacity is
256 KiB per document; Note++ supports eight documents. The model no longer
interprets an arbitrary read error as a new empty file.

## Closing an application

The EmApp `em_set_close_hook` callback covers Escape, toolkit controls, GUI+W,
compositor close requests and explicit `em_app_request_exit`. Returning zero
defers the exit and requests another frame. The app asks Save / Discard / Cancel;
a failed save or cancelled picker keeps the prompt and document open.
Note++ checks every dirty tab before quitting. Cancelling a multi-document quit
restores the dirty status of documents whose discard had been selected earlier.

Native syscall 145, `win_close_ack(window)`, acknowledges a caller-owned window
and cancels its compositor close grace timer. It requires GPU capability, checks
the full ID width and cannot acknowledge another process's window. A hook-based
runtime acknowledges a compositor request as soon as it receives it, including
polls while idle, so the user may spend time choosing a save location. Apps
without a hook retain the existing unresponsive-app timeout. Force termination
and session logout are outside ordinary close negotiation.

## Recovery and reopening

Changed buffers are checkpointed at approximately two-second intervals into
`$HOME/.editor-drafts` or `$HOME/.notepp-drafts`. Checkpoints use the same synced
replacement routine; they do not overwrite the original document. A versioned,
little-endian `EMDRAFT1` record stores path, text length, cursor, original stamp
and a checksum covering the metadata and text. Invalid records are retained,
not loaded into a buffer. No executable data is read from a draft.

Each process owns distinct PID/random-nonce/slot names. Startup snapshots the
directory listing before changing names, skips drafts belonging to other live
instances, and claims abandoned files with rename-noreplace. It syncs a new
owned copy before deleting the abandoned record. Excess drafts are retained
when all eight tabs are occupied. A PID reused after reboot by another live
instance can postpone recovery; no draft is deleted for that reason. Recovery
is automatic, with a visible status message. Undo history is not recovered.
An explicit file open in Editor takes precedence; standalone launch restores
one draft. Note++ restores abandoned dirty documents before saved session paths.

Saving or explicitly discarding a document removes its draft. Note++ records
saved paths and cursors using atomic replacement, and reopens them after a clean
quit or reboot. Recovery is bounded by the most recent successful checkpoint:
work typed after it, filesystem/device failures, and abrupt loss during a
checkpoint are not promised to survive. Session logout still terminates apps
without asking to save; there is no desktop-wide document/power coordinator yet.

The picker still returns paths, so callers need namespace authority over the
selected location. Transferable open-file capabilities remain future work.
Editor now has a home-scoped namespace and explicit filesystem/GPU capabilities;
Files supplies HOME when launching it with EDIT_FILE.

## Verification

`make document-test service-test doc-model-test` checks failures and model
boundaries on the host. The document test injects interrupted/short/zero writes,
ENOSPC, file-sync failure, rename failure and parent-sync failure, asserting
which exact bytes remain. Service tests reject malformed lengths, unterminated
and relative replies, and output-buffer truncation. The existing editing and
UI suites remain applicable.

`test devices files` runs the shared document implementation in ring 3 with a
home-scoped namespace. It checks replacement, external-edit conflicts, binary
refusal and checksummed draft round trips. `test devices` also checks the new
close acknowledgement's capability and ownership boundaries.

`python3 tools/files_document_smoke.py` checks Files creation, HOME/EDIT_FILE
handoff, saving in Editor and duplicate-name refusal in a real user account.

`python3 tools/workspace_smoke.py` drives the real authenticated desktop with
QMP input and retains original captures. GDB inspection reads window ownership
and app buffers; it does not change application state. Build at 1280×800 first.

## Ordered keyboard modifier snapshots

Native syscall 146, `key_poll_input`, consumes the same character queue as the
legacy byte poll and returns its byte plus the modifiers captured when it was
produced. The runtime exposes that snapshot to key hooks through
`em_key_modifiers`. The live `key_mods` operation remains unchanged.

This matters because the legacy navigation encoding deliberately overlaps C0:
Up and Ctrl+S are both 0x13, Page Up and Ctrl+N are both 0x0e. Reading the current
modifier state after polling a delayed character cannot recover which occurred.
A parallel key-event queue cannot repair the ordering of that character stream.
Keeping metadata beside each byte in its existing queue resolves the ambiguity
without changing byte-stream consumers or using another lossy encoding.

Both editors use the snapshot for their document shortcuts. The syscall applies
the same focused-process/session input gate as legacy key_poll. The native
`test input snapshot` preserves live queue contents, checks distinct captured
modifiers for Ctrl+S and Up after release, then restores the original queue.
The desktop acceptance also checks that a plain Up does not open Save.
