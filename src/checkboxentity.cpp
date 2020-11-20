/*
 *  Cascade Image Editor
 *
 *  Copyright (C) 2020 The Cascade developers
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

#include "checkboxentity.h"
#include "ui_checkboxentity.h"

#include "nodeproperties.h"

CheckBoxEntity::CheckBoxEntity(UIElementType et, QWidget *parent) :
    UiEntity(et, parent),
    ui(new Ui::CheckBoxEntity)
{
    ui->setupUi(this);

    connect(ui->checkBox, &QCheckBox::stateChanged,
            this, &CheckBoxEntity::valueChanged);
}

void CheckBoxEntity::setName(const QString &name)
{
    ui->checkBox->setText(name);
}

void CheckBoxEntity::setChecked(bool b)
{
    ui->checkBox->setChecked(b);
}

bool CheckBoxEntity::isChecked()
{
    return ui->checkBox->isChecked();
}

void CheckBoxEntity::selfConnectToValueChanged(NodeProperties *p)
{
    connect(this, &CheckBoxEntity::valueChanged,
            p, [p]{p->handleSomeValueChanged();});
}

QString CheckBoxEntity::getValuesAsString()
{
    return QString::number(ui->checkBox->isChecked());
}

CheckBoxEntity::~CheckBoxEntity()
{
    delete ui;
}
