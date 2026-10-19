"""Render completed production measurements without changing or discarding samples."""
from pathlib import Path
import json
root=Path(__file__).resolve().parents[2]
rows=json.loads((root/'artifacts/filtered-das/performance/summary.json').read_text())
assert len(rows)==36 and all(x['exit']==0 and x['service']['warm_count']==30 for x in rows)
def match(group,n,channels,tv,fd,phase='candidate',batch=64):
    return next(x for x in rows if x['name'].startswith(phase+'_') and all(x['spec'][k]==v for k,v in
        dict(group=group,n=n,channels=channels,tv=tv,fd=fd,batch=batch).items()))
text=['# FilteredDAS 联合性能与内存实测','',
      '同机串行生产服务/共享内存/GPU 合成输入，36 单元，每单元 1 cold + 30 warm 完整块，共 1116 块。',
      '所有 warm 样本参与 median/P95/max，无删样；P95 用 nearest-rank。原始每块记录、绝对命令/cwd/退出码、二进制 SHA256 和峰值计数见 evidence/performance/。',
      'service_ms 从 processRingPulse 入场至快照提交，roundtrip 另含 producer copy/轮询；原始 JSON 同时保留两者、吞吐和耗时。',
      '表中差值是两个串行单元 median 的差，包含主机波动，不能替代真实 T_arr/积压测量。',
      '', '## N5000/7500、4/8 通道、双波长、每通道每块 200 触发','',
      '|N|通道|时变低通|FilteredDAS|cold ms|median ms|P95 ms|max ms|相对同配置 eed98f0 median 差 ms|新滤波 median 增量 ms|A-lines/s|',
      '|---:|---:|---|---|---:|---:|---:|---:|---:|---:|---:|']
for n in [5000,7500]:
    for ch in [4,8]:
        for tv in ['off','on']:
            old=match('load',n,ch,tv,'off','baseline')['service']['median_ms']
            off=match('load',n,ch,tv,'off')['service']['median_ms']
            for fd in ['off','on']:
                r=match('load',n,ch,tv,fd);s=r['service']
                text.append(f"|{n}|{ch}|{tv}|{fd}|{r['cold_service_ms']:.3f}|{s['median_ms']:.3f}|{s['p95_ms']:.3f}|{s['max_ms']:.3f}|{s['median_ms']-old:.3f}|{s['median_ms']-off:.3f}|{s['lines_per_second']:.1f}|")
text+=['','## 当前实际用户几何','',
       'N7500、2 通道、block=10、8000 A-lines/圈、36 mm FOV、0.01 mm 网格；半径 6.48 mm，WL1/WL2 DelayCut=448/464（nt=7053/7037），拼接模式、5°羽化，7.5 mm 双声速边界、1490/1540 m/s，DBR mask=375。',
       '保持原圈长及 source-final 语义，仅在第 31 个完整块后正常停止采样。未把未完成的圈标记为重建完成。','',
       '|时变低通|FilteredDAS|eed98f0 median ms|cold ms|median ms|P95 ms|max ms|新滤波 median 增量 ms|峰值 WS MiB|',
       '|---|---|---:|---:|---:|---:|---:|---:|---:|']
for tv in ['off','on']:
    old=match('actual',7500,2,tv,'off','baseline')['service']['median_ms'];off=match('actual',7500,2,tv,'off')['service']['median_ms']
    for fd in ['off','on']:
        r=match('actual',7500,2,tv,fd);s=r['service'];text.append(f"|{tv}|{fd}|{old:.3f}|{r['cold_service_ms']:.3f}|{s['median_ms']:.3f}|{s['p95_ms']:.3f}|{s['max_ms']:.3f}|{s['median_ms']-off:.3f}|{r['peak_bytes']['working_set']/1048576:.2f}|")
text+=['','## 较高负载与批大小','',
       'high 同时启用 STFT WindowLength=256/FFTLength=256/Overlap=.75 和 FilteredDAS PadFactor=8/BatchColumns=64，其余业务默认不变。','',
       '|N|通道|cold ms|median ms|P95 ms|max ms|新滤波准备 ms|新滤波缓存/工作区 MiB|峰值 WS MiB|',
       '|---:|---:|---:|---:|---:|---:|---:|---:|---:|']
for n in [5000,7500]:
    for ch in [4,8]:
        r=match('high',n,ch,'high','high');s=r['service'];m=r['filtered_metrics']
        text.append(f"|{n}|{ch}|{r['cold_service_ms']:.3f}|{s['median_ms']:.3f}|{s['p95_ms']:.3f}|{s['max_ms']:.3f}|{m['design_us']/1000:.3f}|{m['cached_bytes']/1048576:.2f}|{r['peak_bytes']['working_set']/1048576:.2f}|")
text+=['','N7500、8 通道、时变低通 OFF、FilteredDAS ON、PadFactor=4：','',
       '|BatchColumns|真实最大批列数|median ms|P95 ms|max ms|准备 ms|工作区 MiB|缓存+工作区 MiB|峰值 WS MiB|',
       '|---:|---:|---:|---:|---:|---:|---:|---:|---:|']
for batch in [1,7,64]:
    r=match('load' if batch==64 else 'batch',7500,8,'off','on',batch=batch);s=r['service'];m=r['filtered_metrics']
    assert m['max_batch']==batch
    text.append(f"|{batch}|{int(m['max_batch'])}|{s['median_ms']:.3f}|{s['p95_ms']:.3f}|{s['max_ms']:.3f}|{m['design_us']/1000:.3f}|{m['workspace_bytes']/1048576:.2f}|{m['cached_bytes']/1048576:.2f}|{r['peak_bytes']['working_set']/1048576:.2f}|")
text+=['',f"全矩阵读取的进程最高 PeakWorkingSetSize={max(r['peak_bytes']['working_set'] for r in rows)} bytes；最高 PeakPagefileUsage={max(r['peak_bytes']['pagefile'] for r in rows)} bytes。",
       '采样每 0.1 秒读取 Windows 累计峰值；计数为服务进程主机内存，未将其冒充 GPU VRAM。',
       '默认两个实际 nt 的 FilteredDAS 动态缓存/工作区 35 MiB，其中工作区 32 MiB；PadFactor=8 时 70 MiB/64 MiB。关闭为 0；实际 block=10 的最大批列数为 5，BatchColumns=64 是上限。',
       '资源上限为每配置 nfft<=262144、工作区<=256 MiB、全部长度计划<=128 MiB；原子准备时旧新配置最多两份，滤波核动态存储上限 768 MiB。',
       '没有丢线、降采样、降低补零或自动幅值匹配。实机视觉、真实采集链、T_arr 与长期积压仍为 UNVERIFIED。','']
report=root/'CODEX_REPORTS/ring-filtered-das-20261019'
failed=root/'artifacts/filtered-das/performance/failed-attempts/attempt-01/summary.json'
assert failed.exists()
failures=json.loads(failed.read_text());assert len(failures)==1 and failures[0]['exit']==2 and failures[0]['blocks']==11
text+=['## 未完成的超时尝试','',
       'N7500/C8/两项开启的一次完整尝试在第 12 块超过工具原有 20 秒等待，退出码 2；此前 11 块记录完整保留。',
       '退出后现场读取为 notifications=12、consumed=12、lastProcessUs=22003342（22.003 秒），服务主线程已回到 Qt 事件循环。',
       '不据此归因于某个系统组件；该尝试不能计为通过，且明确证明本次存在超过 20 秒的尾延迟。',
       '失败日志、进程峰值、原命令、退出码和 GDB 现场在 evidence/performance/failed-attempts/attempt-01。',
       '同配置第二次尝试也在 20 秒等待下超时，仅返回首块 13.373 秒，见 attempt-02；记录器无 warm 求 median 异常已修正，未补造未持久化的峰值。',
       '随后只在测量工具增加 --frame-timeout-ms（默认仍 20000），剩余单元及四个复测显式用 120000 捕获完整长尾。',
       '原始 20 秒超时仍为失败；失败单元重新完整采样 31 块。DSP 服务、计时算式和生产超时策略不变。',
       '两版工具源码/二进制和唯一差异见 evidence/performance/tool-versions，各单元命令与 hash 保留。','']
repeat=root/'artifacts/filtered-das/performance/repeat/summary.json'
if repeat.exists():
    repeats=json.loads(repeat.read_text());assert len(repeats)==4
    text+=['## 波动单元的独立复测','',
           '首次数据完整保留在上表；按有限范围修正前观测预先确定的 OFF/ON 配对单元、高尾延迟单元和默认大批单元各复测 1 cold + 30 warm 完整块。不根据本轮结果重新选样，未删除或替换首次样本。',
           '先前与本轮原始记录都保留，运行时波动的系统级根因未归因；跨单元 median 差不能视作纯算法增量。两轮均不能证明真实 T_arr 预算通过。','',
           '|单元|首次 median/P95/max ms|复测 median/P95/max ms|复测峰值 WS MiB|',
           '|---|---:|---:|---:|']
    for r in repeats:
        old=next(x for x in rows if x['name']==r['name']);s=r['service'];o=old['service']
        assert r['exit']==0 and s['warm_count']==30
        text.append(f"|{r['name']}|{o['median_ms']:.3f}/{o['p95_ms']:.3f}/{o['max_ms']:.3f}|{s['median_ms']:.3f}/{s['p95_ms']:.3f}/{s['max_ms']:.3f}|{r['peak_bytes']['working_set']/1048576:.2f}|")
    text+=['','主矩阵 36 单元/1116 块/1080 warm；独立复测 4 单元/124 块/120 warm。所有原始记录与二进制身份均随联合包保留。','']
(report/'performance.md').write_text('\n'.join(text),encoding='utf-8')
print('wrote',report/'performance.md')
