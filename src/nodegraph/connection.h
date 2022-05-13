/*
 *  Cascade Image Editor
 *
 *  Copyright (C) 2022 Till Dechent and contributors
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#ifndef CONNECTION_H
#define CONNECTION_H

#include <QWidget>
#include <QGraphicsItem>

#include "nodebase.h"

namespace Cascade {

class Connection : public QObject, public QGraphicsLineItem
{
    Q_OBJECT

public:
    explicit Connection(NodeOutput* source);
    QPainterPath shape() const override;

    void updatePosition();
    void updatePosition(const QPoint end);

    void addConnectionToJsonObject(QJsonArray& jsonConnectionsArray);

    NodeOutput* mSourceOutput = nullptr;
    NodeInput* mTargetInput = nullptr;

private:
    void paint(QPainter* painter, const QStyleOptionGraphicsItem*  opt, QWidget* wdgt) override;

    const QPen mNormalPen = QPen(QColor(0x92, 0x99, 0xa1), 1);
    const QPen mFrontConnectedPen = QPen(Config::sRed, 1);
    const QPen mBackConnectedPen = QPen(Config::sGreen, 1);
    const QPen mAlphaConnectedPen = QPen(Config::sBlue, 1);

};

} // namespace Cascade

#endif // CONNECTION_H