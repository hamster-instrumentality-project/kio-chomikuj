/*
 * SPDX-FileCopyrightText: 2026 kio-chomikuj contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "chomikujkcm.h"

#include <KConfigGroup>
#include <KLocalizedString>
#include <KPluginFactory>
#include <KSharedConfig>

#include <QComboBox>
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

    m_streamingTarget = new QComboBox(widget());
    m_streamingTarget->addItem(i18n("Off"), QStringLiteral("off"));
    m_streamingTarget->addItem(i18n("Read-only accounts"), QStringLiteral("read-only"));
    m_streamingTarget->addItem(i18n("All video files"), QStringLiteral("all"));
    layout->addRow(i18n("Use streaming for videos:"), m_streamingTarget);

    m_streamingPlayer = new QLineEdit(widget());
    m_streamingPlayer->setPlaceholderText(QStringLiteral("mpv"));
    layout->addRow(i18n("Streaming player:"), m_streamingPlayer);

    auto *streamingHint = new QLabel(
        i18n("“All video files” makes normal Open With use random-access streaming, "
             "but video files opened through kio-fuse become read-only."),
        widget());
    streamingHint->setWordWrap(true);
    layout->addRow(streamingHint);

    auto *hint = new QLabel(
        i18n("The account password is stored through KIO/KWallet, not in this configuration file."),
        widget());
    hint->setWordWrap(true);
    layout->addRow(hint);

    connect(m_account, &QLineEdit::textChanged, this, &ChomikujKcm::markChanged);
    connect(m_deviceName, &QLineEdit::textChanged, this, &ChomikujKcm::markChanged);
    connect(m_streamingTarget, &QComboBox::currentIndexChanged, this, &ChomikujKcm::markChanged);
    connect(m_streamingPlayer, &QLineEdit::textChanged, this, &ChomikujKcm::markChanged);
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

    const KConfigGroup streaming(KSharedConfig::openConfig(QStringLiteral("chomikujrc")),
                                 QStringLiteral("Streaming"));
    const QString target = streaming.readEntry("TargetUrl", QStringLiteral("read-only"));
    const int targetIndex = m_streamingTarget->findData(target);
    m_streamingTarget->setCurrentIndex(targetIndex >= 0 ? targetIndex : 1);
    m_streamingPlayer->setText(streaming.readEntry("Player", QStringLiteral("mpv")));

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

    KConfigGroup streaming(KSharedConfig::openConfig(QStringLiteral("chomikujrc")),
                           QStringLiteral("Streaming"));
    streaming.writeEntry("TargetUrl",
                         m_streamingTarget->currentData().toString());
    const QString player = m_streamingPlayer->text().trimmed();
    streaming.writeEntry("Player", player.isEmpty() ? QStringLiteral("mpv") : player);
    streaming.sync();

    setNeedsSave(false);
    setRepresentsDefaults(false);
}

void ChomikujKcm::defaults()
{
    m_account->clear();
    m_deviceName->setText(QStringLiteral("KDE"));
    m_deviceId->setText(QUuid::createUuid().toString(QUuid::WithoutBraces));
    m_streamingTarget->setCurrentIndex(1);
    m_streamingPlayer->setText(QStringLiteral("mpv"));
    markChanged();
    setRepresentsDefaults(true);
}

void ChomikujKcm::markChanged()
{
    markAsChanged();
    setRepresentsDefaults(false);
}

#include "chomikujkcm.moc"
