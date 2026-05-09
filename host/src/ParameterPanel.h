//=============================================================================
// ParameterPanel.h — 参数配置面板 (Widget, 驻 MainWindow)
//=============================================================================
#pragma once
#include <QWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QFormLayout>
#include <cstdint>

class ParameterPanel : public QWidget
{
    Q_OBJECT
public:
    explicit ParameterPanel(QWidget *parent = nullptr);

    /// 用 PS 返回的参数值更新 UI (不触发写命令)
    void updateFromDevice(uint16_t id, uint32_t value);

signals:
    /// 参数变化 → MainWindow → CommWorker
    void paramChanged(uint16_t id, uint32_t value);

    void readAllClicked();
    void writeAllClicked();
    void batchApplyRequested();

private slots:
    void onApplyL();
    void onApplyC();
    void onApplyRLoad();
    void onApplyVin();
    void onApplyFsw();
    void onApplyRL();
    void onApplyVf();
    void onApplyILMax();
    void onReadAll();

private:
    void setupUi();
    uint32_t toRawValue(const QString &text, double scale);

    QLineEdit *m_editL;
    QLineEdit *m_editC;
    QLineEdit *m_editRLoad;
    QLineEdit *m_editVin;
    QLineEdit *m_editFsw;
    QLineEdit *m_editRL;
    QLineEdit *m_editVf;
    QLineEdit *m_editILMax;

    QPushButton *m_btnApplyL;
    QPushButton *m_btnApplyC;
    QPushButton *m_btnApplyRLoad;
    QPushButton *m_btnApplyVin;
    QPushButton *m_btnApplyFsw;
    QPushButton *m_btnApplyRL;
    QPushButton *m_btnApplyVf;
    QPushButton *m_btnReadAll;
};
