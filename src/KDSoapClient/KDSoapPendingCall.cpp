/****************************************************************************
** Copyright (C) 2010-2016 Klaralvdalens Datakonsult AB, a KDAB Group company, info@kdab.com.
** All rights reserved.
**
** This file is part of the KD Soap library.
**
** Licensees holding valid commercial KD Soap licenses may use this file in
** accordance with the KD Soap Commercial License Agreement provided with
** the Software.
**
**
** This file may be distributed and/or modified under the terms of the
** GNU Lesser General Public License version 2.1 and version 3 as published by the
** Free Software Foundation and appearing in the file LICENSE.LGPL.txt included.
**
** This file is provided AS IS with NO WARRANTY OF ANY KIND, INCLUDING THE
** WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
**
** Contact info@kdab.com if any conditions of this licensing are not
** clear to you.
**
**********************************************************************/
#include "KDSoapPendingCall.h"
#include "KDSoapPendingCall_p.h"
#include "KDSoapNamespaceManager.h"
#include "KDSoapMessageReader_p.h"
#include <QNetworkReply>
#include <QDebug>

KDSoapPendingCall::Private::~Private()
{
    delete reply.data();
    delete buffer;
}

KDSoapPendingCall::KDSoapPendingCall(QNetworkReply *reply, QBuffer *buffer)
    : d(new Private(reply, buffer))
{
}

KDSoapPendingCall::KDSoapPendingCall(const KDSoapPendingCall &other)
    : d(other.d)
{
}

KDSoapPendingCall::~KDSoapPendingCall()
{
}

KDSoapPendingCall &KDSoapPendingCall::operator=(const KDSoapPendingCall &other)
{
    d = other.d;
    return *this;
}

bool KDSoapPendingCall::isFinished() const
{
#if QT_VERSION >= 0x040600
    return d->reply.data()->isFinished();
#else
    return false;
#endif
}

KDSoapMessage KDSoapPendingCall::returnMessage() const
{
    d->parseReply();
    return d->replyMessage;
}

KDSoapHeaders KDSoapPendingCall::returnHeaders() const
{
    d->parseReply();
    return d->replyHeaders;
}

QVariant KDSoapPendingCall::returnValue() const
{
    d->parseReply();
    if (!d->replyMessage.childValues().isEmpty()) {
        return d->replyMessage.childValues().first().value();
    }
    return QVariant();
}

void KDSoapPendingCall::Private::parseReply()
{
    if (parsed) {
        return;
    }
    parsed = true;
    const bool doDebug = (qgetenv("KDSOAP_DEBUG").toInt() != 0);
    QNetworkReply *reply = this->reply.data();
#if QT_VERSION >= 0x040600
    if (!reply->isFinished()) {
        qWarning("KDSoap: Parsing reply before it finished!");
    }
#endif
    if (reply->error()) {
        replyMessage.setFault(true);
        replyMessage.addArgument(QString::fromLatin1("faultcode"), QString::number(reply->error()));
        replyMessage.addArgument(QString::fromLatin1("faultstring"), reply->errorString());
        if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 500) {
            if (doDebug) {
                //qDebug() << reply->readAll();
                qDebug() << reply->errorString();
            }
            return;
        }
        // HTTP 500 is used to return faults, so parse the fault, below
    }

    const QByteArray data = reply->readAll();

    QVariant contentType = reply->header(QNetworkRequest::ContentTypeHeader);
    QString boundaryMarker;
    QString startContentId;
    QString startContentType;
    if (contentType.isValid())
    {
        QMap< QString, QString > contentTypeInfo = extractContentType(contentType.toString());

        if (isMtomXop(contentTypeInfo))
        {
            static const QString sContentTypeAppXopXml(QString::fromLatin1("application/xop+xml"));
            static const QString sContentTypeTextXml(QString::fromLatin1("text/xml"));

            startContentType = contentTypeInfo[QString::fromLatin1("start-info")];

            if ((startContentType.compare(sContentTypeAppXopXml, Qt::CaseInsensitive) == 0) ||
                (startContentType.compare(sContentTypeTextXml, Qt::CaseInsensitive) == 0))
            {
                boundaryMarker   = contentTypeInfo[QString::fromLatin1("boundary")];
                startContentId   = contentTypeInfo[QString::fromLatin1("start")];
            }
            else
            {
                // ERROR. Let KDSoapMessageReader parse the original message for now...
            }
        }
    }

    if (doDebug) {
        qDebug() << data;
    }

    if (!boundaryMarker.isEmpty())
    {
        QMap< QString, MtomXopMessage > messages = extractMessages(boundaryMarker, data);

        if (messages.contains(startContentId))
        {
            KDSoapMessageReader reader;
            reader.xmlToMessage(messages[startContentId].body, &replyMessage, 0, &replyHeaders);
            // The remaining attachments will be ignored for now

            return;
        }
    }

    if (!data.isEmpty()) {
        KDSoapMessageReader reader;
        reader.xmlToMessage(data, &replyMessage, 0, &replyHeaders);
    }
}

QList< QByteArray > KDSoapPendingCall::Private::extractHeaders(
        const QByteArray& data,
        QByteArray&       body) const
{
    QList< QByteArray > headers;

    int indexHeaderEnd = data.indexOf("\r\n\r\n");
    if (indexHeaderEnd > -1)
    {
        int start = 0;
        for (;;)
        {
            int index = data.indexOf("\r\n", start);

            if (index > indexHeaderEnd)
                break;

            headers << data.mid(start, index - start);

            start = index + 2;
        }

        body = data.mid(indexHeaderEnd + 4, data.length() - indexHeaderEnd - 4);
    }

    return headers;
}

QMap< QString, QString > KDSoapPendingCall::Private::extractContentType(
        const QString& data) const
{
    const QStringList contentTypeElements = data.split(QChar::fromLatin1(';'), QString::SkipEmptyParts);
    QMap< QString, QString > contentTypeInfo;
    for (const QString& element : contentTypeElements)
    {
        const QString e = element.trimmed();

        int index = e.indexOf(QChar::fromLatin1('='));
        if (index > -1)
        {
            QString value(e.right(e.length() - index - 1).trimmed());
            if (value.startsWith(QChar::fromLatin1('"'))) value = value.right(value.length() - 1);
            if (value.endsWith(QChar::fromLatin1('"')))   value = value.left(value.length() - 1);
            contentTypeInfo[e.left(index)] = value;
        }
        else
        {
            contentTypeInfo[QString::fromLatin1("content-type")] = e;
        }
    }

    return contentTypeInfo;
}

bool KDSoapPendingCall::Private::isMtomXop(
        QMap< QString, QString >& contentTypeInfo) const
{
    static const QString sContentTypeTag    (QString::fromLatin1("content-type"));
    static const QString sContentTypeTypeTag(QString::fromLatin1("type"));
    static const QString sMimeRelated       (QString::fromLatin1("multipart/related"));
    static const QString sMimeXopXml        (QString::fromLatin1("application/xop+xml"));

    if (QString::compare(contentTypeInfo[sContentTypeTag], sMimeRelated, Qt::CaseInsensitive) == 0)
    {
        if (QString::compare(contentTypeInfo[sContentTypeTypeTag], sMimeXopXml, Qt::CaseInsensitive) == 0)
        {
            return true;
        }
    }

    return false;
}

QMap< QString, KDSoapPendingCall::Private::MtomXopMessage > KDSoapPendingCall::Private::extractMessages(
        const QString& boundaryMarker,
        const QByteArray& data) const
{
    static const QLatin1String sContentTypeTag("content-type:");
    static const QLatin1String sContentIdTag("content-id:");

    const QByteArray boundaryMarkerLine((QString::fromLatin1("--") + boundaryMarker + QString::fromLatin1("\r\n")).toLatin1());

    QMap< QString, MtomXopMessage > result;

    int boundaryMarkerIndex = data.indexOf(boundaryMarkerLine, 0);
    while (boundaryMarkerIndex > -1)
    {
        MtomXopMessage msg;

        const int endIndex = data.indexOf(boundaryMarkerLine, boundaryMarkerIndex + boundaryMarkerLine.length());
        const int startIndex = boundaryMarkerIndex + boundaryMarkerLine.length();
        const QByteArray& localData = data.mid(startIndex, (endIndex == -1 ? data.length() : endIndex) - startIndex);

        // Extract headers and the body
        const QList< QByteArray > headers = extractHeaders(localData, msg.body);


        for (const QByteArray& header : headers)
        {
            const QString& headerStr(QString::fromLatin1(header));

            if (headerStr.startsWith(sContentTypeTag, Qt::CaseInsensitive))
            {
                const QString& contentTypeValue = headerStr.right(headerStr.length() - sContentTypeTag.size()).trimmed();
                QMap< QString, QString > contentTypeInfo = extractContentType(contentTypeValue);
                msg.contentType = contentTypeInfo[QString::fromLatin1("content-type")];
                msg.charSet = contentTypeInfo[QString::fromLatin1("charset")];
            }
            else if (headerStr.startsWith(sContentIdTag, Qt::CaseInsensitive))
            {
                msg.contentId = headerStr.right(headerStr.length() - sContentIdTag.size()).trimmed();
            }
        }

        if (!msg.contentId.isEmpty())
        {
            result[msg.contentId] = msg;
        }

        boundaryMarkerIndex = endIndex;
    }

    return result;
}
