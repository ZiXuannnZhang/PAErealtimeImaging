# Startup ingress loop log schema 2

Each little-endian record remains 80 bytes, with the legacy Python struct
`<QQQQIHHIIIIQQQ` (sequence, timeNs, spanNs, session, threadId, kind, flags,
value0..value3, payloadA..payloadC). The receive thread only pushes fixed-size
records to a bounded queue. It does not perform JSON, file I/O, or system calls.

The background writer keeps a 64 MiB rolling recent window. It writes a frozen
`looplog-window-<id>.bin` only for a marked burst, covering five seconds before
and five seconds after the marker. Overlapping markers are merged and their
IDs/epochs are listed in `looplog-summary.json`. A burst before listener-start
is retained as `beforeListenerStart`; a window that reaches the rolling-memory
or post-window boundary records explicit truncation fields.
When detailed diagnostics pause, the writer drains reservations accepted before
the pause and freezes any open window. `coverageEndNs` records the end of
collection in the same monotonic clock as the records. A pause at or after the
planned end leaves that window's tail complete.

There is no continuous receive-loop file when no burst occurs. The summary is
still written and contains low-rate lifecycle/control/aggregate diagnostics.
The frozen-record budget is 256 MiB and is never overwritten. All byte budgets
are rounded down to complete 80-byte records; a non-zero tail is reported as
`tailBytesOmitted`.

The summary contains at least:

* `schemaVersion`, `recordBytes`, `recordsIssued`, `recordsWritten`;
* `burstMarkEpochs`, `burstWindowIds`, `windowCount`, `windowRecordCounts`;
* `preWindowTruncated`, `postWindowTruncated`, `windowTruncated`;
* `beforeListenerStart`, `retentionEvicted`, `tailBytesOmitted`;
* `queueDropped`, `writerUnwritten`, `budgetOmitted`, `pending`;
* `loopLogIncomplete`, `writeFailed`, `budgetExhausted`, and QPC/UTC anchors.

The four loss classes are intentionally separate:

* `queueDropped`: producer could not enter the bounded queue;
* `writerUnwritten`: records remained when the bounded writer stopped;
* `budgetOmitted`: complete records could not fit in the frozen 256 MiB budget;
* `retentionEvicted`: records fell out of the 64 MiB rolling window before a
  burst marker could freeze them.

The export captures the issued sequence and queue-drop count together.
A frozen window belongs to the export when its first `triggerSequence`
is within that boundary. Later burst markers do not extend the reported
window for that export. `queueDroppedAtOpen` is the cumulative drop count
at the first marker and does not localize a loss to that window's prehistory.
Compare the number of records read with `recordsWritten` for this rolling
mode. Records outside retained windows are intentionally absent.

In ZIPs, `exportWindows` maps each frozen window's ID and `sourceFile` to
its `exportFile`, when any records were included. `inRequestedWindow=false`
identifies a source window entirely outside the requested time range. Its
status is retained in the inventory. A null value means the time relationship
could not be established. `timeBoundsStatus` distinguishes known, missing
(`unknown`) and invalid bounds. Records inside the click sequence are checked
against known source bounds before time clipping. A contradictory in-scope
record makes the export incomplete. `open_window_pending` indicates the writer
had not published a triggered window when the exporter read the inventory.
Time clipping, click-sequence clipping and source window truncation have
separate fields.

A production run whose configured start is after `fixedEndMs` is outside the
request. The run has `runTimeRangeStatus=unknown` when the configuration does
not establish a trusted bound. `queueDropScope` describes the source count as
`run_through_cut` or `source_run`. `queueDroppedInRequestedWindow=null`
means the source count cannot locate drops within the requested wall interval.

`paimage/manifest.json` describes the optional attachment with
`attachmentStatus=available`, `partial` or `unavailable`. If registration or
the root lease fails, the formal ZIP includes the unavailable manifest.
Unreadable registered runs are listed in `skippedRuns` and readable runs
remain in the archive. `attachmentIssues` carries the source error code and
message. `files` lists only members successfully written. A ZIP writing or
publication failure still fails the export.

The analyzer derives each burst's time bounds from its own frozen file or
the mapped ZIP member. The reported five-second coverage describes observed
time bounds. Coverage is `null` when the analyzer cannot establish file
ownership or when truncation leaves coverage uncertain.

ZIP members are checked against the exported count, available file sizes
and digests, and the request and source bounds where known. Both offline
tools use the same LoopLog parser. `records` and `files[].records` report
physical copies, with `physicalRecordsRead` including rejected fixed-size
slots. Run-level event statistics use `logicalRecords` and count identical
copies of the same sequence once per run. `sequenceConflicts` identifies
the files that disagree about a sequence. The parser retains each file's
verification result and excludes that sequence from run-level statistics.

`counterExhausted` reports that the bounded admission counters reached
their limit. New submissions stop at that point and the summary reports
incomplete coverage.

The old schema-1 summary and `looplog-*.bin` files remain readable by the
analyzers. Schema 2 adds fields; it does not change the 80-byte record layout.

Kinds and field meanings retain the existing contract:

| kind | meaning | timeNs / spanNs | fields |
|---|---|---|---|
| 1 Loop | one receive-loop iteration | loopStart / loopStart-previousLoopStart | value0 loopId, value1 select return, value2 drain attempts, value3 successful recvfrom count, payloadA select span, payloadB poll span, payloadC ready-port mask |
| 2 Drain | one socket drain | drainStart / drain duration | value0 card+1 (0=feedback), value1 local port, value2 attempts, value3 successes, payloadA bytes, payloadB drainId, payloadC last ingress ID |
| 3 RecvFailure | failed recvfrom | recvStart / call duration | value0 local port, value1 WSA error, payloadA drainId, flags exit reason |
| 4 SocketSetup | socket setup observation | setup time / 0 | value0 port, value1 handle, value2 SO_RCVBUF, value3 first error, flags setup stages |
| 5 ThreadStart | receiver thread entered loop | threadStart / 0 | value0 priority result, value1 actual priority |
| 6 BurstMark | first sampling datagram after the idle threshold | burst time / idle span | value0/value1 freeze epoch, payloadA previous sample, payloadB threshold |
| 7 Stalled | monitor saw no loop progress | detect time / stalled span | payloadA last loop time |
| 8 ControlMark | prepare/complete start/stop | enter time / duration | value0 phase 1..4, value1 success, payloadB session |

Successful `recvfrom` calls correlate with the raw ingress trace by ingress
ID. Failed calls have no ingress record and use the drain ID in timing output.
Drain flags: 1 = would block (queue observed empty), 2 = socket error,
3 = packet budget yielded (not an error or drop), 4 = stop requested.
The same values apply to Drain timing records; RecvFailure only uses 1 or 2.
Feedback/ACK packets are control evidence, not sampling data.
