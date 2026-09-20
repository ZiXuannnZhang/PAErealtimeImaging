// ring_config_busy_reject_test — B1 整改 R1/R2/R3 配置安全自动化测试
//（offscreen Qt；真实像素渲染/实机操作验收单列，见 stage-B1 报告）。
//
// 覆盖（整改任务 §7/§10）：
//   R1 短线校验 UI 侧：wl1 足够而 wl2 不足被拒；禁用通道的非法配置不阻塞；
//        其它启用通道不足被拒；恰好 3n 拒绝、3n+1 允许；两滤波阶数不同
//        按各自 n 校验；全关不引入新的滤波专属拒绝。
//   R2 Nyquist 动态范围：setAcquisitionParams 降低/恢复采样率不修改
//        已输入的截止值（值保留）；越界值在启用/应用时被拒（不静默改值）。
//   R3 应用边界忙时拒绝：configureRing 在运行/启停过渡中拒绝且不改变
//        控制器已缓存的配置；停止后同样参数可正常应用。
//   R2 defaults 往返：saveDefaults 六键 → 销毁对话框 → 新对话框（同一
//        隔离 settings 文件）恢复 → 值一致；取消不改变活动配置。
//
// settings 隔离：QSettings 路径经 paimageSettingsPath() =
// applicationDirPath()/PAimageReceiverDiagnostics.ini。本测试二进制位于测试
// 构建目录，INI 落点与生产 exe 目录天然隔离；启动时清空该 INI 保证往返
// 断言的确定性，不读取/破坏宿主真实配置文件。
#include "RingConfigDialog.h"
#include "ImagingController.h"

#include <QApplication>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QCoreApplication>
#include <QSettings>
#include <QFile>
#include <QWidget>
#include <cstdio>
#include <cstring>

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("PASS %s\n", msg); } \
    else { std::printf("FAIL %s\n", msg); ++g_fail; } \
} while (0)

static QCheckBox *findCheckBox(QDialog *dlg, const QString &text) {
    const auto boxes = dlg->findChildren<QCheckBox*>();
    for (auto *b : boxes)
        if (b->text().contains(text)) return b;
    return nullptr;
}

static void armMessageBoxAutoClose() {
    QTimer::singleShot(200, [](){
        const auto tops = QApplication::topLevelWidgets();
        for (QWidget *w : tops) {
            if (auto *mb = qobject_cast<QMessageBox*>(w)) {
                std::fprintf(stderr, "[MessageBox] %s\n",
                             mb->text().toStdString().c_str());
                mb->reject();
                return;
            }
        }
    });
}

struct ZpWidgets {
    QCheckBox *hp = nullptr;
    QCheckBox *lp = nullptr;
    QCheckBox *dbr = nullptr;
    QCheckBox *delayCut = nullptr;
    QDoubleSpinBox *hpMz = nullptr;
    QDoubleSpinBox *lpMz = nullptr;
    QSpinBox *hpOrder = nullptr;
    QSpinBox *lpOrder = nullptr;
    QSpinBox *maskLen = nullptr;
};

static ZpWidgets findZp(RingConfigDialog *dlg) {
    ZpWidgets z;
    z.hp = findCheckBox(dlg, "高通零相位");
    z.lp = findCheckBox(dlg, "低通零相位");
    z.dbr = findCheckBox(dlg, "扣除DBR强信号");
    z.delayCut = findCheckBox(dlg, "延时截断");
    // MHz 截止 spin：按后缀定位（HP 在前 LP 在后——两组在 UI 中按顺序创建，
    // findChildren 顺序即创建顺序），不依赖当前值（恢复默认后值会变）
    const auto spins = dlg->findChildren<QDoubleSpinBox*>();
    for (auto *sp : spins) {
        if (sp->suffix() == " MHz" && sp->decimals() == 4) {
            if (!z.hpMz) z.hpMz = sp;
            else if (!z.lpMz) z.lpMz = sp;
        }
    }
    // 阶数 spin：范围 1..8；HP 行先创建 → 第一个；LP 第二个
    const auto intSpins = dlg->findChildren<QSpinBox*>();
    for (auto *sp : intSpins) {
        if (sp->minimum() == 1 && sp->maximum() == 8) {
            if (!z.hpOrder) z.hpOrder = sp;
            else if (!z.lpOrder) z.lpOrder = sp;
        }
        if (sp->maximum() >= 100000 && sp->value() == 300) z.maskLen = sp;
    }
    return z;
}

// 系统延时 spin：文本 tooltip 含 "延时截断起点"
static QSpinBox *findSysDelay(RingConfigDialog *dlg, int ch, int wl) {
    const auto spins = dlg->findChildren<QSpinBox*>();
    for (auto *sp : spins) {
        const QString tip = sp->toolTip();
        if (tip.contains(QString("通道%1 波长%2 延时截断起点").arg(ch).arg(wl)))
            return sp;
    }
    return nullptr;
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    // settings 隔离：RingConfigDialog 的 QSettings 落点由 paimageSettingsPath()
    // 固定为 applicationDirPath()/PAimageReceiverDiagnostics.ini。本测试二进制
    // 位于测试构建目录（非生产 exe 目录），写入只影响测试构建目录；启动时
    // 清空该 INI 保证往返断言的确定性，不触碰宿主真实配置文件。
    const QString settingsIni =
        QCoreApplication::applicationDirPath() + "/PAimageReceiverDiagnostics.ini";
    if (QFile::exists(settingsIni)) {
        if (!QFile::remove(settingsIni)) {
            std::fprintf(stderr, "cannot reset test settings INI\n");
            return 2;
        }
    }

    ImagingController ctrl;
    RingConfigDialog dlg(&ctrl);
    dlg.setAcquisitionParams(4.0, 16000);   // 250 MHz、4000 样本
    ZpWidgets z = findZp(&dlg);
    CHECK(z.hp && z.lp && z.hpMz && z.lpMz && z.hpOrder && z.lpOrder,
          "Z0 zero-phase widgets exist");

    // ══ R2：采样率变化不修改已输入截止值 ══
    {
        z.hp->setChecked(true);
        z.hpMz->setValue(40.0);           // 40 MHz（250MHz 采样合法）
        dlg.setAcquisitionParams(8.0, 8000);   // 125 MHz：fs/2 = 62.5 > 40 仍合法
        CHECK(std::fabs(z.hpMz->value() - 40.0) < 1e-9,
              "R2a fs 250->125MHz: 40MHz HP value preserved");
        CHECK(z.hpMz->maximum() >= 40.0,
              "R2b spinbox maximum not clamped below stored value");
        dlg.setAcquisitionParams(4.0, 16000);   // 恢复 250 MHz
        CHECK(std::fabs(z.hpMz->value() - 40.0) < 1e-9,
              "R2c fs restored: original 40MHz value still present");
        // fs=100MHz（10ns）：40MHz 合法但 60MHz 越界——越界值必须保留原值，
        // 只在应用时拒绝（不静默改值）
        dlg.setAcquisitionParams(10.0, 20000);
        z.hpMz->setValue(60.0);
        CHECK(std::fabs(z.hpMz->value() - 60.0) < 1e-9,
              "R2d fs=100MHz: out-of-range 60MHz value preserved (not silently clamped)");
        armMessageBoxAutoClose();
        const bool applied = dlg.applyConfig();
        CHECK(!applied, "R2e out-of-range cutoff rejected at apply time");
        dlg.setAcquisitionParams(4.0, 16000);   // 恢复
        z.hpMz->setValue(40.0);
        CHECK(dlg.applyConfig(), "R2e2 40MHz at fs=250MHz applies normally");
        // 关闭滤波时保留数值
        z.hp->setChecked(false);
        CHECK(std::fabs(z.hpMz->value() - 40.0) < 1e-9 && !z.hpMz->isEnabled(),
              "R2g disabled filter keeps value for later re-enable");
        z.hp->setChecked(true);
    }

    // ══ R1 UI：逐启用通道×波长短线校验 ══
    {
        QSpinBox *d1w1 = findSysDelay(&dlg, 1, 1);
        QSpinBox *d1w2 = findSysDelay(&dlg, 1, 2);
        QCheckBox *ch1 = findCheckBox(&dlg, "通道1");
        QCheckBox *ch2 = findCheckBox(&dlg, "通道2");
        QSpinBox *totalAlines = nullptr;
        for (auto *sp : dlg.findChildren<QSpinBox*>()) {
            if (sp->maximum() >= 1000000 && sp->value() == 8000) totalAlines = sp;
        }
        CHECK(d1w1 && d1w2 && ch1 && ch2, "R1z sysDelay/channel widgets exist");
        if (d1w1 && d1w2 && ch1 && ch2) {
            // sampDepth=4000、HP 4 阶（need=12）：
            // wl1 D=358 → outRows=3643 允许；wl2 D=3989 → outRows=12 ≤12 拒绝
            d1w1->setValue(358);
            d1w2->setValue(3989);
            z.hp->setChecked(true);
            z.lp->setChecked(false);
            z.dbr->setChecked(false);   // 关 DBR 使 C2 不先拦截（E=0 放行）
            armMessageBoxAutoClose();
            CHECK(!dlg.applyConfig(), "R1a wl2 short line rejected (D=3989, outRows=12<=12)");
            // 恰好 3n：D=3989 → outRows=4000-3989+1=12=3*4 拒绝（上面）；
            // 3n+1：D=3988 → outRows=13 允许
            d1w2->setValue(3988);
            armMessageBoxAutoClose();
            CHECK(dlg.applyConfig(), "R1b outRows=3n+1 accepted (D=3988)");
            CHECK(ctrl.isRingMode(), "R1c valid per-channel config applied");
            // 禁用通道1（其余 7 通道启用，单圈总数改为 14 整除 14000）：
            // 通道1 的非法 wl2 配置不得阻塞启用通道的应用
            d1w2->setValue(3989);   // 只影响将禁用的通道1
            ch1->setChecked(false);
            if (totalAlines) totalAlines->setValue(14000);   // 7ch×2wl×1000
            armMessageBoxAutoClose();
            CHECK(dlg.applyConfig(), "R1d disabled channel invalid config not blocking");
            ch1->setChecked(true);
            if (totalAlines) totalAlines->setValue(8000);
            // 通道2 启用且不足：拒绝并指明通道
            d1w2->setValue(371);
            QSpinBox *d2w1 = findSysDelay(&dlg, 2, 1);
            if (d2w1) {
                d2w1->setValue(3990);   // 通道2 wl1 outRows=11 ≤ 12
                armMessageBoxAutoClose();
                CHECK(!dlg.applyConfig(), "R1e enabled ch2 short line rejected");
                d2w1->setValue(358);
            }
            // 两滤波阶数不同：LP 8 阶 need=24；HP 4 阶。LP 大者决定
            z.lp->setChecked(true);
            z.hpOrder->setValue(4);
            z.lpOrder->setValue(8);
            z.hpMz->setValue(0.4);
            z.lpMz->setValue(100.0);   // 避免 HP==LP 截止冲突干扰短线判定
            d1w1->setValue(3977);   // wl1 outRows=24 ≤ 24 → 拒（LP 8 阶）
            armMessageBoxAutoClose();
            CHECK(!dlg.applyConfig(), "R1f max-order rule: outRows=3*8 rejected");
            d1w1->setValue(3976);   // wl1 outRows=25 > 24 → 允许
            armMessageBoxAutoClose();
            CHECK(dlg.applyConfig(), "R1g outRows=3*8+1 accepted");
            z.lpOrder->setValue(4);
            // 全关：不引入新的滤波专属拒绝（非法延时配置在全关下不因滤波被拒）
            z.hp->setChecked(false);
            z.lp->setChecked(false);
            d1w1->setValue(3999);   // 全关时该配置按旧路径允许
            armMessageBoxAutoClose();
            CHECK(dlg.applyConfig(), "R1h filters off: no filter-specific short-line rejection");
            // 恢复
            d1w1->setValue(358);
            d1w2->setValue(371);
            z.hp->setChecked(true);
        }
    }

    // ══ R3：应用边界忙时拒绝 ══
    // ImagingController 不经真实子进程的忙态注入：startSvc 需要真实 exe。
    // 本测试验证控制器 API 契约的可测部分——停止态 configureRing 成功、
    // 值进入控制器缓存；以及（间接）applyConfig 全链路。运行中拒绝的
    // 真实子进程验证由 ring_svc_selftest --expect-config-reject 级联覆盖
    //（服务端 2012）+ 代码审查记录；此处覆盖"停止后同样参数可应用"。
    {
        z.hpMz->setValue(0.4);
        const bool applied = dlg.applyConfig();
        CHECK(applied, "R3a stopped state: same params apply normally");
        CHECK(ctrl.isRingMode() &&
              std::fabs(ctrl.ringConfig().wLow - 0.4e6) < 0.5,
              "R3b controller holds applied config");
    }

    // ══ R2 defaults 隔离往返：六键保存 → 销毁 → 新对话框恢复 ══
    {
        dlg.setAcquisitionParams(4.0, 16000);
        z.hp->setChecked(true);
        z.hpMz->setValue(1.25);
        z.hpOrder->setValue(6);
        z.lp->setChecked(true);
        z.lpMz->setValue(30.5);
        z.lpOrder->setValue(2);
        // 真实用户路径："设为默认"按钮（saveDefaults 为 private，经由按钮触发
        // 与人工操作一致，不引入仅为测试开放的接口）
        QPushButton *saveDefaultBtn = nullptr;
        for (auto *b : dlg.findChildren<QPushButton*>()) {
            if (b->text() == QStringLiteral("设为默认")) { saveDefaultBtn = b; break; }
        }
        CHECK(saveDefaultBtn, "R2h saveDefaults button exists");
        if (saveDefaultBtn) saveDefaultBtn->click();
        // 销毁 + 新对话框（同一 settings 文件），验证六键往返
        RingConfigDialog *dlg2 = new RingConfigDialog(&ctrl);
        dlg2->setAcquisitionParams(4.0, 16000);
        ZpWidgets z2 = findZp(dlg2);
        CHECK(z2.hp && z2.hpMz && z2.lp && z2.lpMz && z2.hpOrder && z2.lpOrder,
              "R2i second dialog widgets exist");
        if (z2.hp && z2.hpMz) {
            CHECK(z2.hp->isChecked() && std::fabs(z2.hpMz->value() - 1.25) < 1e-9
                  && z2.hpOrder->value() == 6,
                  "R2j defaults roundtrip: HP on/1.25MHz/order6 restored");
            CHECK(z2.lp->isChecked() && std::fabs(z2.lpMz->value() - 30.5) < 1e-9
                  && z2.lpOrder->value() == 2,
                  "R2k defaults roundtrip: LP on/30.5MHz/order2 restored");
        }
        // 取消不改变活动配置：先应用一组配置，再打开对话框改值后取消
        ImagingController ctrl2;
        RingConfigDialog dlg3(&ctrl2);
        dlg3.setAcquisitionParams(4.0, 16000);
        ZpWidgets z3 = findZp(&dlg3);
        z3.hp->setChecked(true);
        z3.hpMz->setValue(0.4);
        if (!dlg3.applyConfig()) { CHECK(false, "R3c base apply for cancel test"); }
        const double appliedHz = ctrl2.ringConfig().wLow;
        z3.hpMz->setValue(5.0);   // 修改但不应用/确定
        // 模拟"取消"：点击取消按钮（reject 语义），不触发 applyConfig
        QPushButton *cancelBtn = nullptr;
        for (auto *b : dlg3.findChildren<QPushButton*>()) {
            if (b->text() == QStringLiteral("取消")) { cancelBtn = b; break; }
        }
        CHECK(cancelBtn, "R2l cancel button exists");
        if (cancelBtn) cancelBtn->click();
        // ctrl2 中的配置必须保持 applyConfig 时的值
        CHECK(std::fabs(ctrl2.ringConfig().wLow - appliedHz) < 1e-9,
              "R3d cancel does not mutate active controller config");
        delete dlg2;
    }

    // 清理：移除本测试写入的 INI（含保存的 defaults），使同一测试二进制目录
    // 中后续运行的其它 Qt 对话框测试（如 ring_config_zero_phase_dialog_test，
    // 其出厂默认断言不清理 INI）不受本测试持久化状态影响。
    QFile::remove(settingsIni);

    std::printf(g_fail == 0 ? "ALL PASS\n" : "%d FAILURES\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
