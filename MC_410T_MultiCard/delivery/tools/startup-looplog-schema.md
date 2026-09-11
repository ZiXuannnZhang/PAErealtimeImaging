# Startup ingress loop log schema 1

Each little-endian record is 80 bytes, Python struct
"<QQQQIHHIIIIQQQ" (sequence, timeNs, spanNs, session, threadId, kind, flags,
value0..value3, payloadA..payloadC). Records are appended by the receive
thread and low-rate monitoring producers into a preallocated bounded queue;
no JSON, file I/O or system calls run on the receive path. The background
writer emits looplog-N.bin segments plus looplog-summary.json. Segments rotate
at 1 second or 8 MiB, retain up to 5 seconds, and protect up to eight merged
freeze windows (five seconds before/after BurstMark, RecvFailure, or Stalled).
The summary separates `retentionEvicted`, `queueDropped`, `freezeRejected`,
`budgetExhausted`, `ioWriteFailed`, `exportTruncated`, and `writerUnwritten`;
`loopLogIncomplete` is true whenever any coverage or durability qualifier is
present. Total retained budget is 256 MiB; exhaustion is explicit and never
back-pressures the receiver. Old segments are removed only within the current
run and only when not frozen or held by an export cut.

Kinds and field meanings:

| kind | meaning | timeNs / spanNs | fields |
|---|---|---|---|
| 1 Loop | one receive-loop iteration, written at loop end | loopStart / loopStart-previousLoopStart | value0 loopId, value1 select return (uint32 of int), value2 drain attempts, value3 successful recvfrom count, payloadA select span, payloadB poll span (mutex wait + poll), payloadC ready-port mask (bit0 feedback, bit1+i card i), flags bit0 select timeout, bit1 select error |
| 2 Drain | one socket drain | drainStart / drain duration | value0 card+1 (0=feedback), value1 local port, value2 attempts, value3 successes, payloadA bytes, payloadB drainId, payloadC last ingress ID, flags exit reason (1 would-block, 2 socket error) |
| 3 RecvFailure | failed recvfrom | recvStart / call duration | value0 local port, value1 WSAGetLastError, payloadB drainId, flags 1 would-block, 2 socket error |
| 4 SocketSetup | socket create/nonblocking/rcvbuf/bind before thread start | setup time / 0 | value0 port, value1 handle low 32 bits, value2 SO_RCVBUF result, value3 first error code, flags bit0 feedback, bit1 create failed, bit2 rcvbuf set failed, bit3 nonblocking failed, bit4 rcvbuf get failed, bit5 bind failed |
| 5 ThreadStart | receiver thread entered its loop | threadStart / 0 | value0 SetThreadPriority result, value1 actual priority |
| 6 BurstMark | first sampling datagram after >=2 s without any | burst time / idle span | value0/value1 freeze epoch (lo/hi), payloadA previous sample time, payloadB burst threshold time |
| 7 Stalled | 100 ms monitor saw no loop progress for >20 ms | detect time / stalled span | payloadA last loop time. Sampling cannot observe every 20 ms stall and never proves CPU preemption alone |
| 8 ControlMark | prepareStart/completeStart/prepareStop/completeStop | enter time / duration | value0 phase 1..4, value1 success, payloadB session, flags bit0 enabled after, bit1 confirmed after |

Successful recvfrom calls correlate with the raw ingress trace by ingress ID:
the timing stream Recvfrom record and the TraceRecord share that ID. Failed
recvfrom timing records carry the drain ID with flags bit 1 set instead.

Burst marking only marks receive-burst windows; it never changes session
handling, assembly cleanup or physical round attribution. ACK/ready feedback
is not sampling data.

Ten seconds after a burst, the UI thread writes `burst-<epoch>-<runId>.json`
to the configured system-capture channel. When the independent capture script
has written a still-valid `active-trial.json`, its per-round `trialId` is used;
otherwise the command-line trial ID is used. The notification records
`trialIdSource` and `commandLineTrialId`. The receive thread never reads either
file and never waits for this exchange.
