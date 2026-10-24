/*
 * SPDX-FileCopyrightText: 2026 kio-chomikuj contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "chomikujkcm.h"

#include <KConfigGroup>
#include <KLocalizedString>
#include <KPluginFactory>
#include <KSharedConfig>

#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QUuid>

K_PLUGIN_CLASS_WITH_JSON(ChomikujKcm, "kcm_chomikuj.json")

ChomikujKcm::ChomikujKcm(QObject *parent, const KPluginMetaData &data)
    : KCModule(parent, data)
{
    KLocalizedString::setApplicationDomain("kio6_chomikuj");
    setButtons(KCModule::Apply | KCModule::Default);

    auto *layout = new QFormLayout(widget());

    m_account = new QLineEdit(widget());
    m_account->setPlaceholderText(i18n("Chomikuj account name"));
    layout->addRow(i18n("Default account:"), m_account);

    m_deviceName = new QLineEdit(widget());
    m_deviceName->setPlaceholderText(QStringLiteral("KDE"));
    layout->addRow(i18n("Device name:"), m_deviceName);

    auto *deviceIdRow = new QWidget(widget());
    auto *deviceIdLayout = new QHBoxLayout(deviceIdRow);
    deviceIdLayout->setContentsMargins(0, 0, 0, 0);
    m_deviceId = new QLabel(deviceIdRow);
    m_deviceId->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *regenerate = new QPushButton(i18n("Generate new ID"), deviceIdRow);
    deviceIdLayout->addWidget(m_deviceId, 1);
    deviceIdLayout->addWidget(regenerate);
    layout->addRow(i18n("Device ID:"), deviceIdRow);

    auto *hint = new QLabel(
        i18n("The account password is stored through KIO/KWallet, not in this configuration file."),
        widget());
    hint->setWordWrap(true);
    layout->addRow(hint);

    connect(m_account, &QLineEdit::textChanged, this, &ChomikujKcm::markChanged);
    connect(m_deviceName, &QLineEdit::textChanged, this, &ChomikujKcm::markChanged);
    connect(regenerate, &QPushButton::clicked, this, [this] {
        m_deviceId->setText(QUuid::createUuid().toString(QUuid::WithoutBraces));
        markChanged();
    });
}

void ChomikujKcm::load()
{
    const KConfigGroup group(KSharedConfig::openConfig(QStringLiteral("chomikujrc")),
                             QStringLiteral("General"));

    m_account->setText(group.readEntry("LastAccount", QString()));
    m_deviceName->setText(group.readEntry("DeviceName", QStringLiteral("KDE")));

    QString deviceId = group.readEntry("DeviceId", QString());
    if (deviceId.isEmpty()) {
        deviceId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    m_deviceId->setText(deviceId);

    setNeedsSave(false);
    setRepresentsDefaults(false);
}

void ChomikujKcm::save()
{
    KConfigGroup group(KSharedConfig::openConfig(QStringLiteral("chomikujrc")),
                       QStringLiteral("General"));

    group.writeEntry("LastAccount", m_account->text().trimmed());

    const QString deviceName = m_deviceName->text().trimmed();
    group.writeEntry("DeviceName", deviceName.isEmpty() ? QStringLiteral("KDE") : deviceName);

    QString deviceId = m_deviceId->text().trimmed();
    if (deviceId.isEmpty()) {
        deviceId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        m_deviceId->setText(deviceId);
    }
    group.writeEntry("DeviceId", deviceId);
    group.sync();

    setNeedsSave(false);
    setRepresentsDefaults(false);
}

void ChomikujKcm::defaults()
{
    m_account->clear();
    m_deviceName->setText(QStringLiteral("KDE"));
    m_deviceId->setText(QUuid::createUuid().toString(QUuid::WithoutBraces));
    markChanged();
    setRepresentsDefaults(true);
}

void ChomikujKcm::markChanged()
{
    markAsChanged();
    setRepresentsDefaults(false);
}

#include "chomikujkcm.moc"
