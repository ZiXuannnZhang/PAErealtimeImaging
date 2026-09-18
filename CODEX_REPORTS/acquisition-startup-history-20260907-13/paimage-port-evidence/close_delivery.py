"""Finalize offline evidence; remaining computer-use acceptance belongs to the user."""
import hashlib,json,shutil,zipfile
from pathlib import Path
root=Path(__file__).resolve().parents[2]
evidence=Path(__file__).parent
out=root/'artifacts/PAimageAcquisitionPort'
def sha(data):return hashlib.sha256(data).hexdigest()
archive=root/'build/gui-validation-export.zip'
with zipfile.ZipFile(archive) as z:
    assert z.testzip() is None
    manifest=json.loads(z.read('manifest.json'))
    trace=json.loads(z.read('paimage/manifest.json'))
    for item in trace['files']:assert sha(z.read(item['path']))==item['sha256']
    events=[json.loads(line) for line in z.read('events.jsonl').decode().splitlines()]
    marker=next(e for e in events if e['message']=='experiment_marker')
    assert marker['fields']['trialId']=='gui-validation' and marker['fields']['roundId']=='1'
    assert not manifest['missingFiles'] and not manifest['recorderWriteError']
    result={'date':'2026-09-10','zipSha256':sha(archive.read_bytes()),'zipCrc':'passed',
        'traceManifestFileHashes':'passed','exportMode':'idle; no acquisition trace runs',
        'marker':marker,'sourceRunIds':manifest['sourceRunIds'],
        'guiObserved':['delivery EXE startup and backend/SHA title','experiment marker entry','asynchronous diagnostic export success'],
        'remainingGuiAcceptance':'user-owned by explicit instruction; no further Computer Use',
        'unverified':['GUI parameter persistence after restart','old-registry before/after equality','physical NIC and GPU end-to-end experiment']}
(evidence/'gui-offline-validation.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
shutil.copy2(root/'build/paimage-host-checks/Testing/Temporary/LastTest.log',evidence/'diagnostic-dialog-passed.txt')
replacements={
 '软件验证报告.md':[('production-3候选版。生产采集替换已生效；独立Debug已编译。工程验收仍等待桌面交互，实机效果未验证。','production-3交付版。生产采集替换、独立Debug构建与软件验证完成。用户已接管剩余Computer Use界面验收；实机效果未验证。'),('最近全套25项中24项通过；唯一未通过为diagnostic_dialog_test初始化超时，需解锁后重测','24项非交互回归通过；diagnostic_dialog_test补齐qoffscreen.dll后3.29秒通过，合计25项分组通过；并非同一次全套运行')],
 '移植差异与限制.md':[('候选版production-3','交付版production-3'),('代码审查；实际GUI持久化待解锁','代码审查；实际GUI持久化由用户验收'),('## 尚未关闭的工程验收','## 用户接管的界面验收'),('桌面当前锁定；实际GUI操作、窗口视觉、实验标记与原参数隔离的端到端检查待用户回复“已解锁”。diagnostic_dialog_test卡在QApplication初始化，尚未执行对话框测试逻辑。候选程序与报告不标为最终工程验收通过。','独立交付EXE启动、后端/SHA标题、实验标记和空闲异步导出已实际操作通过。diagnostic_dialog_test超时源于测试目录缺少qoffscreen.dll，补齐后3.29秒通过；已补自动部署规则。用户于2026-09-10要求停止Computer Use测试，剩余参数持久化、旧注册表前后对比及完整现场操作验收交由用户，未将这些项目记为通过。工程实现、软件验证与独立包交付完成；完整人工验收及实机效果仍待用户确认。')],
 '同机分时测试操作说明.md':[('当前为待桌面验收候选，先由开发会话完成解锁后的GUI检查，再用于正式对照。','工程实现和软件验证已完成；用户已接管剩余界面验收。正式对照前先检查参数修改重启后保持、原程序参数不受影响，并确认实际启用的2个成像通道。')],
 'PAimage采集行为映射.md':[('工程候选版已实际替换生产采集路径；桌面验收待解锁。','交付版已实际替换生产采集路径；剩余界面验收由用户接管。'),('实际窗口交互与用户外接网卡实验尚未验收。','实际窗口启动、标记和空闲导出已验证；剩余参数交互及外接网卡实验由用户验收。')]
}
for name,pairs in replacements.items():
    path=root/'CODEX_REPORTS'/name
    text=path.read_text(encoding='utf-8')
    for before,after in pairs:text=text.replace(before,after)
    if name=='软件验证报告.md' and '## 交付窗口验证' not in text:
        text+='\n## 交付窗口验证\n\n交付EXE标题、实验标记和空闲异步导出已实际操作通过。导出ZIP CRC及PAimage子清单文件SHA256通过，事件中trialId=gui-validation、roundId=1、dataTimeNs=50000。未启动采集，不将空闲导出算作采集中GUI验收；活跃writer导出由自动测试覆盖。部分旧runtime中文文本存在编码乱码，结构化标记字段可读；该旧文本问题不作为采集完整性依据。用户已要求停止Computer Use，参数修改/重启持久化和旧注册表前后相等未验收。证据见paimage-port-evidence/gui-offline-validation.json、diagnostic-dialog-passed.txt。\n'
    path.write_text(text,encoding='utf-8')
    shutil.copy2(path,out/name)
print(json.dumps(result,ensure_ascii=False))
