//=============================================================================
// ParameterPanel.cpp — 参数配置面板实现
//=============================================================================
#include "ParameterPanel.h"
#include "protocol/Protocol.h"
#include <QHBoxLayout>
#include <QVBoxLayout>

ParameterPanel::ParameterPanel(QWidget *parent)
    : QWidget(parent)
{
    setupUi();
}

void ParameterPanel::setupUi()
{
    auto *mainLayout = new QVBoxLayout(this);

    // 标题
    auto *title = new QLabel(QStringLiteral("<b>参数配置</b>"), this);
    mainLayout->addWidget(title);

    // 使用 FormLayout 布局
    auto *form = new QFormLayout;

    // ---- L (电感) ----
    {
        auto *row = new QHBoxLayout;
        m_editL = new QLineEdit("100.0", this);
        m_editL->setPlaceholderText(QStringLiteral("μH"));
        m_btnApplyL = new QPushButton(QStringLiteral("设置"), this);
        row->addWidget(m_editL, 1);
        row->addWidget(m_btnApplyL);
        form->addRow(QStringLiteral("L (μH):"), row);
        connect(m_btnApplyL, &QPushButton::clicked, this, &ParameterPanel::onApplyL);
        connect(m_editL, &QLineEdit::returnPressed, this, &ParameterPanel::onApplyL);
    }

    // ---- C (电容) ----
    {
        auto *row = new QHBoxLayout;
        m_editC = new QLineEdit("100.0", this);
        m_editC->setPlaceholderText(QStringLiteral("μF"));
        m_btnApplyC = new QPushButton(QStringLiteral("设置"), this);
        row->addWidget(m_editC, 1);
        row->addWidget(m_btnApplyC);
        form->addRow(QStringLiteral("C (μF):"), row);
        connect(m_btnApplyC, &QPushButton::clicked, this, &ParameterPanel::onApplyC);
        connect(m_editC, &QLineEdit::returnPressed, this, &ParameterPanel::onApplyC);
    }

    // ---- R_load (负载) ----
    {
        auto *row = new QHBoxLayout;
        m_editRLoad = new QLineEdit("10.0", this);
        m_editRLoad->setPlaceholderText(QStringLiteral("Ω"));
        m_btnApplyRLoad = new QPushButton(QStringLiteral("设置"), this);
        row->addWidget(m_editRLoad, 1);
        row->addWidget(m_btnApplyRLoad);
        form->addRow(QStringLiteral("Rload (Ω):"), row);
        connect(m_btnApplyRLoad, &QPushButton::clicked, this, &ParameterPanel::onApplyRLoad);
        connect(m_editRLoad, &QLineEdit::returnPressed, this, &ParameterPanel::onApplyRLoad);
    }

    // ---- Vin (输入电压) ----
    {
        auto *row = new QHBoxLayout;
        m_editVin = new QLineEdit("12.0", this);
        m_editVin->setPlaceholderText(QStringLiteral("V"));
        m_btnApplyVin = new QPushButton(QStringLiteral("设置"), this);
        row->addWidget(m_editVin, 1);
        row->addWidget(m_btnApplyVin);
        form->addRow(QStringLiteral("Vin (V):"), row);
        connect(m_btnApplyVin, &QPushButton::clicked, this, &ParameterPanel::onApplyVin);
        connect(m_editVin, &QLineEdit::returnPressed, this, &ParameterPanel::onApplyVin);
    }

    // ---- Fsw (开关频率) ----
    {
        auto *row = new QHBoxLayout;
        m_editFsw = new QLineEdit("200.0", this);
        m_editFsw->setPlaceholderText(QStringLiteral("kHz"));
        m_btnApplyFsw = new QPushButton(QStringLiteral("设置"), this);
        row->addWidget(m_editFsw, 1);
        row->addWidget(m_btnApplyFsw);
        form->addRow(QStringLiteral("Fsw (kHz):"), row);
        connect(m_btnApplyFsw, &QPushButton::clicked, this, &ParameterPanel::onApplyFsw);
        connect(m_editFsw, &QLineEdit::returnPressed, this, &ParameterPanel::onApplyFsw);
    }

    // ---- R_L (电感 ESR) ----
    {
        auto *row = new QHBoxLayout;
        m_editRL = new QLineEdit("0.1", this);
        m_editRL->setPlaceholderText(QStringLiteral("Ω"));
        m_btnApplyRL = new QPushButton(QStringLiteral("设置"), this);
        row->addWidget(m_editRL, 1);
        row->addWidget(m_btnApplyRL);
        form->addRow(QStringLiteral("R_L (Ω):"), row);
        connect(m_btnApplyRL, &QPushButton::clicked, this, &ParameterPanel::onApplyRL);
    }

    // ---- Vf (二极管压降) ----
    {
        auto *row = new QHBoxLayout;
        m_editVf = new QLineEdit("0.7", this);
        m_editVf->setPlaceholderText(QStringLiteral("V"));
        m_btnApplyVf = new QPushButton(QStringLiteral("设置"), this);
        row->addWidget(m_editVf, 1);
        row->addWidget(m_btnApplyVf);
        form->addRow(QStringLiteral("Vf (V):"), row);
        connect(m_btnApplyVf, &QPushButton::clicked, this, &ParameterPanel::onApplyVf);
    }

    // ---- IL_MAX (电流上限) ----
    {
        auto *row = new QHBoxLayout;
        m_editILMax = new QLineEdit("10.0", this);
        m_editILMax->setPlaceholderText(QStringLiteral("A"));
        auto *btn = new QPushButton(QStringLiteral("设置"), this);
        row->addWidget(m_editILMax, 1);
        row->addWidget(btn);
        form->addRow(QStringLiteral("I_L Max (A):"), row);
        connect(btn, &QPushButton::clicked, this, &ParameterPanel::onApplyILMax);
    }

    mainLayout->addLayout(form);

    // ---- 底部按钮 ----
    auto *btnRow = new QHBoxLayout;
    m_btnReadAll = new QPushButton(QStringLiteral("读取全部参数"), this);
    btnRow->addWidget(m_btnReadAll);
    mainLayout->addLayout(btnRow);

    connect(m_btnReadAll, &QPushButton::clicked, this, &ParameterPanel::onReadAll);

    mainLayout->addStretch();
}

//=============================================================================
// Apply slots
//=============================================================================
void ParameterPanel::onApplyL()
{
    bool ok;
    double val = m_editL->text().toDouble(&ok);
    if (ok && val > 0) {
        // μH → nH (协议单位)
        uint32_t raw = static_cast<uint32_t>(val * 1000.0);
        emit paramChanged(Protocol::PARAM_L, raw);
    }
}

void ParameterPanel::onApplyC()
{
    bool ok;
    double val = m_editC->text().toDouble(&ok);
    if (ok && val > 0) {
        // μF → pF (协议单位)
        uint32_t raw = static_cast<uint32_t>(val * 1'000'000.0);
        emit paramChanged(Protocol::PARAM_C, raw);
    }
}

void ParameterPanel::onApplyRLoad()
{
    bool ok;
    double val = m_editRLoad->text().toDouble(&ok);
    if (ok && val > 0) {
        uint32_t raw = static_cast<uint32_t>(val * 1000.0);  // Ω → mΩ
        emit paramChanged(Protocol::PARAM_R_LOAD, raw);
    }
}

void ParameterPanel::onApplyVin()
{
    bool ok;
    double val = m_editVin->text().toDouble(&ok);
    if (ok && val >= 0) {
        uint32_t raw = static_cast<uint32_t>(val * 1000.0);  // V → mV
        emit paramChanged(Protocol::PARAM_VIN, raw);
    }
}

void ParameterPanel::onApplyFsw()
{
    bool ok;
    double val = m_editFsw->text().toDouble(&ok);
    if (ok && val > 0) {
        uint32_t raw = static_cast<uint32_t>(val * 1000.0);  // kHz → Hz
        emit paramChanged(Protocol::PARAM_F_SW, raw);
    }
}

void ParameterPanel::onApplyRL()
{
    bool ok;
    double val = m_editRL->text().toDouble(&ok);
    if (ok && val >= 0) {
        uint32_t raw = static_cast<uint32_t>(val * 1000.0);  // Ω → mΩ
        emit paramChanged(Protocol::PARAM_R_L, raw);
    }
}

void ParameterPanel::onApplyVf()
{
    bool ok;
    double val = m_editVf->text().toDouble(&ok);
    if (ok && val >= 0) {
        uint32_t raw = static_cast<uint32_t>(val * 1000.0);  // V → mV
        emit paramChanged(Protocol::PARAM_VF, raw);
    }
}

void ParameterPanel::onApplyILMax()
{
    bool ok;
    double val = m_editILMax->text().toDouble(&ok);
    if (ok && val >= 0) {
        uint32_t raw = static_cast<uint32_t>(val * 1000.0);  // A → mA
        emit paramChanged(Protocol::PARAM_IL_MAX, raw);
    }
}

void ParameterPanel::onReadAll()
{
    emit readAllClicked();
}

//=============================================================================
// 从设备回读参数更新 UI (不触发写)
//=============================================================================
void ParameterPanel::updateFromDevice(uint16_t id, uint32_t value)
{
    // 临时断开信号以免触发循环
    auto blockEdit = [](QLineEdit *edit, const QString &text) {
        edit->blockSignals(true);
        edit->setText(text);
        edit->blockSignals(false);
    };

    switch (id) {
    case Protocol::PARAM_L:
        blockEdit(m_editL, QString::number(value / 1000.0, 'f', 3));
        break;
    case Protocol::PARAM_C:
        blockEdit(m_editC, QString::number(value / 1'000'000.0, 'f', 3));
        break;
    case Protocol::PARAM_R_LOAD:
        blockEdit(m_editRLoad, QString::number(value / 1000.0, 'f', 3));
        break;
    case Protocol::PARAM_VIN:
        blockEdit(m_editVin, QString::number(value / 1000.0, 'f', 2));
        break;
    case Protocol::PARAM_F_SW:
        blockEdit(m_editFsw, QString::number(value / 1000.0, 'f', 1));
        break;
    case Protocol::PARAM_R_L:
        blockEdit(m_editRL, QString::number(value / 1000.0, 'f', 4));
        break;
    case Protocol::PARAM_VF:
        blockEdit(m_editVf, QString::number(value / 1000.0, 'f', 3));
        break;
    case Protocol::PARAM_IL_MAX:
        blockEdit(m_editILMax, QString::number(value / 1000.0, 'f', 2));
        break;
    default: break;
    }
}

//=============================================================================
// 辅助
//=============================================================================
uint32_t ParameterPanel::toRawValue(const QString &text, double scale)
{
    bool ok;
    double val = text.toDouble(&ok);
    return (ok && val >= 0) ? static_cast<uint32_t>(val * scale) : 0;
}
