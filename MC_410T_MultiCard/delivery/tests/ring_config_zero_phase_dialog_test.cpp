// ring_config_zero_phase_dialog_test — 阶段 B1 RingConfigDialog 零相位滤波控件
// 逻辑联动测试（offscreen；真实像素渲染/实机操作验收单列，见 stage-B1 报告）。
//
// 覆盖（任务 §10 配置与链路）：
//   1. 控件默认值（出厂：全关、HP 0.4MHz/4 阶、LP 40MHz/4 阶）
//   2. 开关联动：关闭时禁用该行数值输入且保留值
//   3. config() 导出：MHz→Hz 一次转换、开关映射到 filterLow/filterHigh
//   4. applyConfig 拒绝路径：非法截止/C2 组合失败时不下发配置
//     （以控制器是否进入 ring 模式判定——configureRing 只在成功路径调用）
//   5. 合法配置应用成功（控制器进入 ring 模式且字段正确）
//
// 注：saveDefaults/restoreDefaults 的持久化往返依赖真实用户 QSettings
//（paimageSettingsPath()），测试进程与被测对话框共用同一设置文件会
// 污染宿主配置；持久化键名对称性由代码结构保证，实机"设为默认/重启恢复"
// 列为 GUI 实测项（stage-B1 报告 UNVERIFIED 区单列）。
#include "RingConfigDialog.h"
#include "ImagingController.h"

#include <QApplication>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QMessageBox>
#include <QTimer>
#include <QWidget>
#include <cstdio>

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

// applyConfig 失败时弹模态 QMessageBox；offscreen 下用单发定时器关闭，
// 使 applyConfig 返回 false 继续测试。
static void armMessageBoxAutoClose() {
    QTimer::singleShot(200, [](){
        const auto tops = QApplication::topLevelWidgets();
        for (QWidget *w : tops) {
            if (auto *mb = qobject_cast<QMessageBox*>(w)) {
                mb->reject();
                return;
            }
        }
    });
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);

    ImagingController ctrl;
    RingConfigDialog dlg(&ctrl);
    dlg.setAcquisitionParams(4.0, 16000);   // 250 MHz、4000 样本

    // 1) 出厂默认：全关、0.4/40 MHz、4 阶
    {
        RingReconCudaConfig c = dlg.config();
        CHECK(c.filterLow == 0 && c.filterHigh == 0,
              "D1 factory defaults: both filters off");
        CHECK(std::fabs(c.wLow - 0.4e6) < 1.0 && c.n1 == 4,
              "D2 HP default 0.4 MHz, order 4");
        CHECK(std::fabs(c.wHigh - 40e6) < 1.0 && c.n2 == 4,
              "D3 LP default 40 MHz, order 4");
    }

    // 2) 开关联动 + 值保留
    {
        auto *hp = findCheckBox(&dlg, "高通零相位");
        auto *lp = findCheckBox(&dlg, "低通零相位");
        CHECK(hp && lp, "D4 filter checkboxes exist");
        const auto spins = dlg.findChildren<QDoubleSpinBox*>();
        QDoubleSpinBox *hpMz = nullptr, *lpMz = nullptr;
        for (auto *sp : spins) {
            if (sp->suffix() == " MHz" && std::fabs(sp->value() - 0.4) < 1e-6) hpMz = sp;
            if (sp->suffix() == " MHz" && std::fabs(sp->value() - 40.0) < 1e-6) lpMz = sp;
        }
        CHECK(hpMz && lpMz, "D5 MHz spinboxes exist");
        if (hp && hpMz) {
            hp->setChecked(true);
            CHECK(hpMz->isEnabled(), "D6 HP row enabled when checked");
            hp->setChecked(false);
            CHECK(!hpMz->isEnabled(), "D7 HP row disabled when unchecked");
            CHECK(std::fabs(hpMz->value() - 0.4) < 1e-9,
                  "D8 HP value preserved while disabled");
        }
        if (lp && lpMz) {
            lp->setChecked(true);
            CHECK(lpMz->isEnabled(), "D9 LP row enabled when checked");
            lp->setChecked(false);
            CHECK(!lpMz->isEnabled(), "D10 LP row disabled when unchecked");
        }
    }

    // 3) config() 导出：MHz→Hz 一次转换、字段映射
    {
        auto *hp = findCheckBox(&dlg, "高通零相位");
        auto *lp = findCheckBox(&dlg, "低通零相位");
        if (hp && lp) {
            hp->setChecked(true);
            lp->setChecked(true);
            RingReconCudaConfig c = dlg.config();
            CHECK(c.filterLow == 1 && c.filterHigh == 1,
                  "D11 config() maps checkboxes to filterLow/filterHigh");
            CHECK(std::fabs(c.wLow - 0.4e6) < 0.5 &&
                  std::fabs(c.wHigh - 40e6) < 0.5,
                  "D12 config() converts MHz to Hz once");
        }
    }

    // 4) 非法截止拒绝（HP 200MHz ≥ fs/2=125MHz）：applyConfig 失败，
    //    控制器未收到配置（isRingMode 仍 false）
    {
        auto *hp = findCheckBox(&dlg, "高通零相位");
        const auto spins = dlg.findChildren<QDoubleSpinBox*>();
        QDoubleSpinBox *hpMz = nullptr;
        for (auto *sp : spins)
            if (sp->suffix() == " MHz" && std::fabs(sp->value() - 0.4) < 1e-6) hpMz = sp;
        if (hp && hpMz) {
            hp->setChecked(true);
            hpMz->setValue(200.0);
            armMessageBoxAutoClose();
            const bool applied = dlg.applyConfig();
            CHECK(!applied, "D13 invalid HP cutoff rejected by applyConfig");
            CHECK(!ctrl.isRingMode(), "D14 rejected config not sent to controller");
            hpMz->setValue(0.4);
        }
    }

    // 5) C2 组合拒绝（HP 开 + DBR 开 + delayCut 关）
    {
        auto *hp = findCheckBox(&dlg, "高通零相位");
        auto *dbr = findCheckBox(&dlg, "扣除DBR强信号");
        auto *dc = findCheckBox(&dlg, "延时截断");
        if (hp && dbr && dc) {
            hp->setChecked(true);
            dbr->setChecked(true);
            dc->setChecked(false);
            armMessageBoxAutoClose();
            const bool applied = dlg.applyConfig();
            CHECK(!applied, "D15 C2 uncut-DBR combination rejected in UI");
            CHECK(!ctrl.isRingMode(), "D16 C2-rejected config not sent");
            dc->setChecked(true);   // 恢复合法状态
        }
    }

    // 6) 合法配置应用成功（控制器进入 ring 模式）
    {
        const bool applied = dlg.applyConfig();
        CHECK(applied, "D17 valid config applied");
        CHECK(ctrl.isRingMode(), "D18 controller entered ring mode");
        const RingReconCudaConfig &c = ctrl.ringConfig();
        CHECK(c.filterLow == 1 && c.filterHigh == 1,
              "D19 controller received filter flags");
        CHECK(std::fabs(c.wLow - 0.4e6) < 0.5 &&
              std::fabs(c.wHigh - 40e6) < 0.5,
              "D20 controller received Hz cutoffs");
    }

    std::printf(g_fail == 0 ? "ALL PASS\n" : "%d FAILURES\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
