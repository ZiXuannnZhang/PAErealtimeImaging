# Local source backup before 2026-10-13 cleanup

This archival branch preserves local-only source/doc blobs before cleanup; none of these files is adopted into canonical main.
working-tree/ keeps original repository-relative paths. stashes/<SHA>/ keeps exact previously unpushed stash blobs.
older-pending-source/ preserves unique source already present in the older pending-delete area without modifying that area.
The accompanying audits record original paths, blob identities and provenance. Generated Qt/CMake files are regenerable build outputs, not unique authored source.
Original copies remain local until remote publication is verified. Raw acquisition/calibration data and runtime dependencies are handled separately and are not uploaded by this backup.
