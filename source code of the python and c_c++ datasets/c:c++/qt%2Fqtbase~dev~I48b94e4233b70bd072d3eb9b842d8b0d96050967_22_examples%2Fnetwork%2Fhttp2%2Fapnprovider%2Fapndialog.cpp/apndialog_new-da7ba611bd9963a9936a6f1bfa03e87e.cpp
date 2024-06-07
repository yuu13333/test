/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the examples of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:BSD$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** BSD License Usage
** Alternatively, you may use this file under the terms of the BSD license
** as follows:
**
** "Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions are
** met:
**   * Redistributions of source code must retain the above copyright
**     notice, this list of conditions and the following disclaimer.
**   * Redistributions in binary form must reproduce the above copyright
**     notice, this list of conditions and the following disclaimer in
**     the documentation and/or other materials provided with the
**     distribution.
**   * Neither the name of The Qt Company Ltd nor the names of its
**     contributors may be used to endorse or promote products derived
**     from this software without specific prior written permission.
**
**
** THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
** "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
** LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
** A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
** OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
** SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
** LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
** OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE."
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "apnprovider.h"
#include "apndialog.h"

ApnDialog::ApnDialog(ApnProvider *apn, QWidget *parent)
    : QDialog(parent),
      ui(new Ui::ApnDialog),
      provider(apn),
      listViewModel(0, 1)
{
    Q_ASSERT(apn);

    ui->setupUi(this);
    ui->devicesView->setModel(&listViewModel);
    ui->devicesView->setSelectionMode(QAbstractItemView::SingleSelection);

    if (loadPreviousSslConfiguration() && startProvider())
        enableControls();
}

ApnDialog::~ApnDialog()
{
    delete ui;
}

void ApnDialog::deviceRegistered(const QByteArray &deviceToken)
{
    const QString itemText(QString::fromLatin1(deviceToken));
    listViewModel.appendRow(new QStandardItem(itemText));
    ui->sendBtn->setEnabled(true);
}

void ApnDialog::apnReply(const QString &description)
{
    ui->replyStatus->setText(description);
}

void ApnDialog::on_sendBtn_clicked()
{
    ui->replyStatus->clear();

    const QModelIndex index = ui->devicesView->currentIndex();
    if (index.isValid()) {
        if (QStandardItem *item = listViewModel.itemFromIndex(index)) {
            Q_ASSERT(provider.data());
            provider->sendNotification(item->text().toLatin1(),
                                       ui->modeCheckBox->isChecked());
            return;
        }
    }

    QMessageBox::critical(nullptr, tr("APN provider"), tr("Please, select a device token ..."));
}

void ApnDialog::on_loadCertBtn_clicked()
{
    const auto path = QFileDialog::getOpenFileName();
    if (path.size() && path != currentCertificatePath) {
        disableControls();

        Q_ASSERT(provider.data());
        provider->stop();

        if (loadSslConfiguration(path) && startProvider())
            enableControls();
    }
}

void ApnDialog::disableControls()
{
    listViewModel.clear();
    ui->replyStatus->clear();
    ui->serverStatus->clear();

    ui->devicesView->setEnabled(false);
    ui->sendBtn->setEnabled(false);
}

void ApnDialog::enableControls()
{
    ui->serverStatus->setText(tr("The server is running on IP: %1\tport: %2\n"
                                 "Run the APN client example now.\n\n")
                                 .arg(provider->serverAddress().toString())
                                 .arg(provider->serverPort()));
    ui->devicesView->setEnabled(true);
}

bool ApnDialog::loadPreviousSslConfiguration()
{
    QSettings settings(QSettings::UserScope, QLatin1String("QtProject"));
    settings.beginGroup(QLatin1String("QtNetwork"));
    const QString certificatePath = settings.value(QLatin1String("APNCertificate")).toString();
    settings.endGroup();

    if (!certificatePath.size())
        return false;

    return loadSslConfiguration(certificatePath);
}

bool ApnDialog::loadSslConfiguration(const QString &certificatePath)
{
    QFile pkcs12File(certificatePath);
    if (!pkcs12File.open(QFile::ReadOnly)) {
        QMessageBox::critical(this, tr("APN provider"),
                              tr("Failed to open certificate file: %1").arg(certificatePath));
        return false;
    }

    QSslKey key;
    QSslCertificate cert;
    QList<QSslCertificate> chain;
    if (!QSslCertificate::importPkcs12(&pkcs12File, &key, &cert, &chain, "")) {
        QMessageBox::critical(this, tr("APN provider"),
                              tr("Failed to import pkcs12 from: %1").arg(certificatePath));
        return false;
    }

    auto config = QSslConfiguration::defaultConfiguration();
    QList<QSslCertificate> localCerts = config.localCertificateChain();
    localCerts.append(chain);
    localCerts.append(cert);
    config.setLocalCertificateChain(localCerts);
    config.setPrivateKey(key);
    config.setPeerVerifyMode(QSslSocket::VerifyNone);

    Q_ASSERT(provider.data());
    provider->setSslConfiguration(config);

    QSettings settings(QSettings::UserScope, QLatin1String("QtProject"));
    settings.beginGroup(QLatin1String("QtNetwork"));
    settings.setValue(QLatin1String("APNCertificate"), certificatePath);
    settings.endGroup();
    currentCertificatePath = certificatePath;
    return true;
}

bool ApnDialog::startProvider()
{
    Q_ASSERT(provider.data());
    provider->disconnect();

    if (!provider->start()) {
        QMessageBox::critical(this, tr("APN provider"), tr("Failed to start a TCP server"));
        return false;
    }

    connect(provider.data(), &ApnProvider::deviceRegistered, this,
            &ApnDialog::deviceRegistered);
    connect(provider.data(), &ApnProvider::apnError, this, &ApnDialog::apnReply);
    connect(provider.data(), &ApnProvider::apnReply, this, &ApnDialog::apnReply);

    return true;
}

void ApnDialog::on_quitBtn_clicked()
{
    close();
}
