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
Feedback/ACK packets are control evidence, not sampling data.
