/*
 * SPDX-FileCopyrightText: 2026 kio-chomikuj contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <KCModule>

class KPluginMetaData;
class QLabel;
class QLineEdit;
class QWidget;

class ChomikujKcm : public KCModule
{
    Q_OBJECT

public:
    explicit ChomikujKcm(QObject *parent, const KPluginMetaData &data);

    void load() override;
    void save() override;
    void defaults() override;

private:
    void markChanged();

    QLineEdit *m_account = nullptr;
    QLineEdit *m_deviceName = nullptr;
    QLabel *m_deviceId = nullptr;
};
