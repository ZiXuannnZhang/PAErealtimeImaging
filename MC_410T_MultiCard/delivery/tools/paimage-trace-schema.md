# PAimage receiver diagnostics schema 3

Each little-endian record is 64 bytes, Python struct `<QQQQIIIHHHHHhBB4sH`.
Fields: sequence, monotonicNs, measurementSession, correlation, threadId,
sourceIPv4 (Windows native representation of network-order bytes), value,
localPort, sourcePort, datagramLength, triggerSeq, packetSeq, cardId, stage,
reason, rawHeader[4], schemaVersion. No payload is stored.

Stage 1 is successful recvfrom before classification. It is application ingress,
not NIC ingress. Correlation is the raw ingress ID. Stage 2 is source decision:
0 accepted, 1 short, 2 disabled, 3 recent trigger, 4 duplicate, 5 offset outside,
6 complete, 7 trigger switch, 8 timeout, 9 startup idle clear, 10 startup overflow,
11 startup confirmed, 12 sync expired, 13 Stop active truncation observation,
14 startup buffered, 15 card output, 16 sync output, 17 invalid card,
18 startup overflow discard, 19 Stop buffered discard, 20 listener active discard,
21 listener buffered discard; 22 listener pending sync discard; 23 START pending sync
discard; 24 START active discard; 25 startup reset active discard; 26 startup reset
pending sync discard. Object events correlate to the object's first raw ID.

Stage 3: feedback classification (1 ready/18 bytes, 2 ACK/60 bytes, 0 unknown).
Stage 4: historical replay adapter boundary; never proves GUI host delivery.
Stage 5: output queue result (0 CardQueued, 1 CardDisabled, 2 CardFull,
3 CardConsumed, 4 SyncQueued, 5 SyncEvicted, 6 SyncConsumed, 7 SyncStale,
8 CallbackFailed, 9 ListenerDiscard, 10 SessionDiscard, 11 SavingDiscard).
Stage 6: control send, reason is command byte 4; packet stores command byte 57
(START=1, STOP=0 when reason=3); threadId is the sending thread. Value is socket error, length
is returned byte count (0 for failed send). Stage 7: real host boundary:
reason 0 save result (value 0 not requested, 1 disabled, 2 legacy queued,
3 legacy queue full, 4 legacy queue failure, 5 synchronous consumer returned true,
6 synchronous consumer returned false); reason 1 display returned, 2 Ring returned,
3 publisher returned (value is boolean); 4 exception; 5 stale after conversion.
Callback return does not establish durable disk write or completed reconstruction.

`run-config.json` identifies each immutable listener configuration. Packet counts
and source addresses must not be mixed across these directories. First arrival
anchors relative packet slots; physical slot zero is unknown. Whole unseen
triggers have no physical ground truth in these traces.

Default queue allocation is 16 MiB; segments are 64 MiB; each run has a 1 GiB
budget. Missing summary, sequence gaps, malformed records, write failure or queue
loss make the trace incomplete. Export captures a fixed record fence and time
window without stopping reception, then waits for a background flush. Clipping
is explicit and remains incomplete for whole-run analysis; absent pre-window
object anchors must not be inferred. `paimage/manifest.json` lists coverage,
run identity and SHA256 of exported files.

Usage: `python paimage_trace_analyze.py TRACE_DIR --packets 28 --samples 5000 --output analysis.json`
Use 70 and 12500 for 50 microseconds. Production endpoint metadata overrides the
replay port default. Each source run is analyzed separately.

Schema 3 keeps schema-1/2 ingress records unchanged and adds an independent
64-byte timing stream with Python struct `<QQQQQIIIHhHHI>`. Timing kinds are:
1 receiver thread lifetime, 2 select, 3 socket drain, 4 loop gap, 5 recvfrom,
6 raw trace push, 7 ingress statistics callback, 8 core mutex wait, 9 core ingest,
10 core poll, 11 control-thread core mutex wait, 12 control-thread mutex hold,
13 card enqueue, 14 sync enqueue, 15 card worker, 16 sync worker. Detailed files
retain data-bearing select/drain records and intervals at least 0.5 ms; every
interval contributes to fixed aggregates and 0.5/2/10/100 ms histogram buckets.

`timing-summary.json` records queue/budget loss and paired QPC/UTC anchors.
Wall elapsed records show elapsed time only; use WPR scheduling evidence before
classifying a long interval as CPU preemption. The timing queue is 16 MiB and disk budget is 256 MiB. This quick candidate targets listener runs up to 120 seconds. Observed detailed event rate reached about 21,182 records/second; exhaustion is explicit. QPC/UTC to steady-clock alignment error is not yet verified.

`--rounds-json` accepts a JSON object with a `rounds` array. Each item may
set `trialId`, `roundId`, `extraTrigger`, `effectiveCount` and `basis`.
Only `user-confirmed` or `protocol-confirmed` round starts plus a complete
trace allow the analyzer to verify the 4000-trigger conservation check. An
operator marker time is never treated as a physical trigger edge. ZIP input is
supported; use `--run-id` when a ZIP contains multiple runs.
