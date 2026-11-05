# FBP synthetic performance measurements

Serialized submission; first block excluded. P95 is nearest-rank. All warm samples retained.
250 ms is the compute budget for 10 triggers at a combined 40 Hz. This is not sustained-ingress acceptance.

|Build|Channels|STFT|Shift|Warm n|Service mean / median / P95 / max ms|Roundtrip mean / median / P95 / max ms|Roundtrip >250 ms|Preprocess mean ms|Accumulate mean ms|
|---|---:|---|---:|---:|---|---|---:|---:|---:|
|baseline|2|on|0.375|20|126.452 / 132.023 / 152.938 / 153.945|138.713 / 142.041 / 165.569 / 167.727|0|—|—|
|candidate|2|on|0.375|20|68.704 / 71.562 / 82.239 / 86.453|78.876 / 81.952 / 95.374 / 97.953|0|28.837|19.946|
