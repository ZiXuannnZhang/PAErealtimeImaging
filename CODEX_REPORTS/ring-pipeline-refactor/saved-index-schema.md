# 保存索引契约

## 文件

每个 card/channel 保存组生成：

- `CardN_ChA_<suffix>.dat`
- `CardN_ChB_<suffix>.dat`
- `CardN_ChA_<suffix>.index.jsonl`
- `CardN_ChA_<suffix>.manifest.json`

写文件只追加，不使用同名 reopen/truncate。A/B 文件的同一批次必须同时成功，随后 index 追加 commit 行。

## `index.jsonl`

`data` 行至少包含：`kind`、`batchId`、`acceptSequence`、`session`、`wireTrigger`、`expandedTrigger`、`card`、`sampleCount`、`aOffset`、`aBytes`、`bOffset`、`bBytes`、`qualityUnknown`、`assemblyComplete`、`packetCoverageComplete`、`missingReason`。

每批 data 行之后追加一行 `commit`：`batchId`、`rows`、批次 A/B 起始 offset 和字节数。缺少 commit 的 data 尾部属于未提交尾部，不能计入 `writtenCount/savedCount`。

## manifest 与计数

manifest 记录 `schemaVersion`、card、session、source IPv4、file sequence、`acceptedTriggers`、`writtenTriggers`、A/B/index 路径和 write error。`acceptedCount` 代表保存 worker 已接收，`writtenCount`/`savedCount` 只在 A/B 写完、index data 行及 commit 都成功后递增。

## 检查器

```powershell
python MC_410T_MultiCard/delivery/tools/check_saved_index.py <目录或*.index.jsonl>
```

`check_saved_index.py` 只读检查：JSONL、data/commit 配对、批次行数、触发身份重复、A/B 长度一致、offset 不越界和 expanded trigger 缺口。发现错误返回 exit code 1；不会修复、截断或重写数据。
