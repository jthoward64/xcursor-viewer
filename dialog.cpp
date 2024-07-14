/*
 * Copyright (C) 2019  Ivan Romanov <drizt72@zoho.eu>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "dialog.h"
#include <ui_dialog.h>

#include <QBuffer>
#include <QDataStream>
#include <QDebug>
#include <QDir>
#include <QtSvg>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QShortcut>
#include <QMessageBox>

#define QS(str) QStringLiteral(str)
#define QSU(str) QString::fromUtf8(str)
#define QSL(str) QString::fromLatin1(str)

Dialog::Dialog(const QString &path, QWidget *parent)
    : QDialog(parent), ui(new Ui::Dialog)
{
    ui->setupUi(this);

    connect(ui->twCursors, &QTreeWidget::currentItemChanged, this, &Dialog::showCursor);
    connect(ui->svgSlider, &QSlider::valueChanged, this, &Dialog::setSvgSize);
    connect(ui->pbOpenFolder, &QPushButton::clicked, this, &Dialog::openFolder);
    connect(new QShortcut(QKeySequence("Ctrl+O"), ui->pbOpenFolder), &QShortcut::activated,
            ui->pbOpenFolder, &QPushButton::click);
    connect(new QShortcut(QKeySequence("Ctrl+E"), ui->pbExport), &QShortcut::activated,
            ui->pbExport, &QPushButton::click);

    ui->svgSlider->setMinimum(0);
    ui->svgSlider->setMaximum(512);
    ui->svgSlider->setValue(256);
    ui->svgSlider->setTickPosition(QSlider::TicksBelow);

    if (!path.isEmpty())
    {
        openFolderPath(path);
    }
}

Dialog::~Dialog()
{
    delete ui;
}

void Dialog::openFolder()
{
    QString path = QFileDialog::getExistingDirectory(this);

    if (path.isEmpty())
    {
        return;
    }

    openFolderPath(path);
}

void Dialog::openFolderPath(QString path)
{
    this->currentPath = path;

    const QFileInfo pathInfo(path);
    QString fileToSelect;
    if (!pathInfo.isDir())
    {
        path = pathInfo.absoluteDir().absolutePath();
        fileToSelect = pathInfo.fileName();
    }

    QDir dir(path);

    QFileInfoList fileList = dir.entryInfoList(QDir::Filter::Files);

    _cursorFileMap.clear();

    for (const QFileInfo &fileInfo : fileList)
    {
        QFile file(fileInfo.absoluteFilePath());
        if (!file.open(QIODevice::OpenModeFlag::ReadOnly))
        {
            continue;
        }

        QDataStream stream(&file);
        stream.setByteOrder(QDataStream::ByteOrder::LittleEndian);

        quint32 magic;
        quint32 header;
        quint32 version;
        quint32 ntoc;

        stream >> magic >> header >> version >> ntoc;

        if (magic != 0x72756358 /* Xcur */ && header != 16)
        {
            continue;
        }

        CursorFile cursorFile;

        cursorFile.name = fileInfo.fileName();
        QFileInfo fi = fileInfo;

        while (fi.isSymLink())
        {
            fi = QFileInfo(fi.symLinkTarget());
        }

        cursorFile.realName = fi.fileName();

        for (quint32 i = 0; i < ntoc; ++i)
        {
            quint32 type;
            quint32 subtype;
            quint32 position;

            stream >> type >> subtype >> position;
            qint64 tocPos = file.pos(); // position in table of contents entries

            if (type == 0xfffd0003)
            {
                file.seek(position);

                quint32 imgHeader;
                quint32 imgType;
                quint32 imgSubtype;
                quint32 imgVersion;
                quint32 pathLength;
                quint32 imgYhot;
                quint32 imgXhot;
                quint32 imgDelay;

                stream >> imgHeader >> imgType >> imgSubtype >> imgVersion >> pathLength >> imgXhot >> imgYhot >> imgDelay;

                if (imgHeader != 32 || imgType != type || imgSubtype != subtype || imgVersion != 1)
                {
                    continue;
                }

                Cursor cursor;
                cursor.size = imgSubtype;
                cursor.hotSpot = QPointF(static_cast<float>(imgXhot) / 1000000.0f, static_cast<float>(imgYhot) / 1000000.0f);

                QByteArray imgPathData = file.read(pathLength);
                QString imgPath = QString::fromUtf8(imgPathData.constData(), pathLength);

                float hotSpotScaleFactor = this->svgSize / cursor.size;

                QSvgRenderer renderer(imgPath);
                QImage img(this->svgSize, this->svgSize, QImage::Format_ARGB32);
                img.fill(Qt::transparent);

                QPainter painter(&img);
                renderer.render(&painter);
                painter.setPen(Qt::red);
                painter.drawEllipse((cursor.hotSpot.x() * hotSpotScaleFactor) - this->svgSize / 10, (cursor.hotSpot.y() * hotSpotScaleFactor) - this->svgSize / 10, this->svgSize / 5, this->svgSize / 5);

                cursor.image = img;

                QString key = QS("svg-%1").arg(static_cast<int>(subtype), 3, 10, QLatin1Char('0'));
                cursorFile.cursorMap.insert(key, cursor);

                file.seek(tocPos);
            }
            else if (type == 0xfffd0002)
            {
                file.seek(position);

                quint32 imgHeader;
                quint32 imgType;
                quint32 imgSubtype;
                quint32 imgVersion;
                quint32 imgWidth;
                quint32 imgHeight;
                quint32 imgYhot;
                quint32 imgXhot;
                quint32 imgDelay;

                stream >> imgHeader >> imgType >> imgSubtype >> imgVersion >> imgWidth >> imgHeight >> imgXhot >> imgYhot >> imgDelay;

                if (imgHeader != 36 || imgType != type || imgSubtype != subtype || imgVersion != 1)
                {
                    continue;
                }

                QByteArray imgData = file.read((imgWidth * imgHeight) * 4);

                Cursor cursor;
                cursor.image = QImage(reinterpret_cast<uchar *>(imgData.data()), static_cast<int>(imgWidth), static_cast<int>(imgHeight), QImage::Format::Format_ARGB32).copy();
                QPainter painter(&cursor.image);
                painter.setPen(Qt::red);
                painter.drawEllipse(imgXhot - imgSubtype / 10, imgYhot - imgSubtype / 10, imgSubtype / 5, imgSubtype / 5);
                cursor.hotSpot = QPointF(static_cast<float>(imgXhot), static_cast<float>(imgYhot));
                cursor.size = imgSubtype;

                QString key = QS("img-%1").arg(static_cast<int>(subtype), 3, 10, QLatin1Char('0'));
                cursorFile.cursorMap.insert(key, cursor);

                file.seek(tocPos);
            }
            else if (type == 0xfffe0001)
            {
                file.seek(position);

                quint32 commHeader;
                quint32 commType;
                quint32 commSubtype;
                quint32 commVersion;
                quint32 commLength;

                stream >> commHeader >> commType >> commSubtype >> commVersion >> commLength;

                if (commHeader != 20 || commType != type || commSubtype != subtype || commVersion != 1)
                {
                    continue;
                }

                QByteArray commData;
                commData.resize(static_cast<int>(commLength));
                stream.readRawData(commData.data(), static_cast<int>(commLength));

                switch (subtype)
                {
                case 1:
                    if (!cursorFile.copyright.isEmpty())
                    {
                        cursorFile.copyright += QS("\n");
                    }
                    cursorFile.copyright += QSU(commData);
                    break;

                case 2:
                    if (!cursorFile.license.isEmpty())
                    {
                        cursorFile.license += QS("\n");
                    }
                    cursorFile.license += QSU(commData);
                    break;

                case 3:
                    if (!cursorFile.other.isEmpty())
                    {
                        cursorFile.other += QS("\n");
                    }
                    cursorFile.other += QSU(commData);
                    break;

                default:
                    break;
                }

                file.seek(tocPos);
            }
        }

        _cursorFileMap.insert(cursorFile.name, cursorFile);
    }

    ui->twCursors->clear();

    QList<QTreeWidgetItem *> topLevelItems;
    QTreeWidgetItem *itemToSelect = nullptr;
    QStringList nameList;

    for (const CursorFile &cursorFile : _cursorFileMap)
    {
        if (cursorFile.realName == cursorFile.name)
        {
            QTreeWidgetItem *item = new QTreeWidgetItem({cursorFile.name});
            topLevelItems << item;
            if (cursorFile.name == fileToSelect && !itemToSelect)
            {
                itemToSelect = item;
            }
        }
    }

    for (const CursorFile &cursorFile : _cursorFileMap)
    {
        if (cursorFile.realName != cursorFile.name)
        {
            for (QTreeWidgetItem *topLevel : topLevelItems)
            {
                if (topLevel->text(0) == cursorFile.realName)
                {
                    QTreeWidgetItem *item = new QTreeWidgetItem(topLevel, {cursorFile.name});
                    topLevelItems << item;
                    if (cursorFile.name == fileToSelect && !itemToSelect)
                    {
                        itemToSelect = item;
                    }
                }
            }
        }
    }

    ui->twCursors->addTopLevelItems(topLevelItems);

    if (itemToSelect)
    {
        ui->twCursors->setCurrentItem(itemToSelect);
    }

    ui->pbExport->setEnabled(!_cursorFileMap.isEmpty());
}

void Dialog::showCursor(QTreeWidgetItem *current, QTreeWidgetItem *previous)
{
    this->current = current;
    this->previous = previous;

    this->reloadCursor();
}

void Dialog::setSvgSize(qint32 size)
{
    this->svgSize = size;

    openFolderPath(this->currentPath);

    this->reloadCursor();
}

void Dialog::reloadCursor()
{
    if (!this->current)
    {
        ui->teCursorInfo->clear();
        return;
    }

    CursorFile currentCursorFile = _cursorFileMap.value(this->current->text(0));
    CursorFile prevCursorFile = this->previous ? _cursorFileMap.value(this->previous->text(0)) : CursorFile();

    if (currentCursorFile.name.isEmpty())
    {
        ui->teCursorInfo->clear();
        return;
    }

    if (prevCursorFile.realName == currentCursorFile.realName)
    {
        return;
    }

    if (currentCursorFile.cachedCursors.isEmpty())
    {
        currentCursorFile.cachedCursors = QS("<html><body><h1>%1</h1>").arg(currentCursorFile.realName);

        QStringList keys = currentCursorFile.cursorMap.keys();
        keys.removeDuplicates();
        keys.sort();

        if (!currentCursorFile.copyright.isEmpty())
        {
            currentCursorFile.cachedCursors += QS("Copyright: %1<br/>").arg(currentCursorFile.copyright);
        }

        if (!currentCursorFile.license.isEmpty())
        {
            currentCursorFile.cachedCursors += QS("License: %1<br/>").arg(currentCursorFile.license);
        }

        if (!currentCursorFile.other.isEmpty())
        {
            currentCursorFile.cachedCursors += QS("Other: %1<br/>").arg(currentCursorFile.other);
        }

        for (const QString &key : keys)
        {
            QList<Cursor> cursorList = currentCursorFile.cursorMap.values(key);
            currentCursorFile.cachedCursors += "<p>";
            Cursor firstCursor = cursorList.first();
            currentCursorFile.cachedCursors += QS("Nominal size: %1. Image size: %2x%3. Hot spot: %4x%5<br/>").arg(QString::number(firstCursor.size), QString::number(firstCursor.image.width()), QString::number(firstCursor.image.height()), QString::number(firstCursor.hotSpot.x()), QString::number(firstCursor.hotSpot.y()));
            for (const Cursor &cursor : cursorList)
            {
                QByteArray imgBa;
                QBuffer buffer(&imgBa);
                cursor.image.save(&buffer, "PNG");

                imgBa = imgBa.toBase64();

                currentCursorFile.cachedCursors += "<img src=\"data:image/png;base64, " + QSL(imgBa) + "\"/>";
            }
            currentCursorFile.cachedCursors += "</p>";
        }

        currentCursorFile.cachedCursors += "</body></html>";

        QMutableMapIterator<QString, CursorFile> it = _cursorFileMap;
        QString realName = currentCursorFile.realName.isEmpty() ? currentCursorFile.name : currentCursorFile.realName;
        while (it.hasNext())
        {
            CursorFile &cursorFile = it.next().value();
            if (cursorFile.realName == currentCursorFile.realName)
            {
                cursorFile.cachedCursors = currentCursorFile.cachedCursors;
            }
        }
    }

    ui->teCursorInfo->setHtml(currentCursorFile.cachedCursors);
}

void Dialog::on_pbExport_clicked()
{
    QString path = QFileDialog::getExistingDirectory(this);
    for (const CursorFile &cursorFile : _cursorFileMap)
    {
        for (const Cursor &cursor : cursorFile.cursorMap)
        {
            QString fpath = QS("%1/%2_%3_%4.png").arg(path, cursorFile.name, QString::number(cursor.hotSpot.x()), QString::number(cursor.hotSpot.y()));
            if (!cursor.image.save(fpath))
            {
                QMessageBox::critical(this, tr("Export Failed"), tr("Could not save file: <pre>%1</pre>").arg(fpath));
                return;
            }
        }
    }
    QMessageBox::information(this, tr("Export Completed"), tr("The cursors have been exported successfully!"));
}
