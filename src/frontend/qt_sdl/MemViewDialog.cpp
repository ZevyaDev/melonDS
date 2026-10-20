/*
    Copyright 2016-2025 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#include "MemViewDialog.h"
#include "debugger/DebugBridge.h"
#include "debugger/DebugSymbolsQt.h"
#include "debugger/DebugTool.h"
#include "debugger/DebugTools.h"
#include "Platform.h"
#include "NDSCart.h"
#include "SPI_Firmware.h"
#include <algorithm>
#include <array>
#include <utility>
#include <QPainter>
#include <QGraphicsTextItem>
#include <QTextCursor>
#include <QStyleOptionGraphicsItem>
#include <QStyle>
#include <QFileDialog>
#include <string.h>

#include "main.h"

using namespace melonDS;

// --- MemViewDialog ---

MemViewDialog* MemViewDialog::currentDlg = nullptr;

CustomTextItem::CustomTextItem(const QString &text, QGraphicsItem *parent) : QGraphicsTextItem(text, parent)
{
    this->SetSelectionFlags();
    this->setFlag(QGraphicsItem::ItemIsSelectable, false);
    this->SetSize(QRectF(2, 6, 20, 15));
    this->eHighlightState = hightlightState_Stop;
}

QRectF CustomTextItem::boundingRect() const
{
    return this->Size;
}

bool CustomTextItem::IsKeyValid(int key)
{
    // hex chars
    if (this->IsKeyHex(key))
    {
        return true;
    }

    // navigation arrows
    if (key >= Qt::Key_Left && key <= Qt::Key_Down)
    {
        return true;
    }

    // other important keys
    switch (key)
    {
        case Qt::Key_Enter:
        case Qt::Key_Return:
        case Qt::Key_Delete:
        case Qt::Key_Backspace:
            return true;
        default:
            break;
    }

    return false;
}

void CustomTextItem::setPlainText(const QString &text, bool highlight)
{
    if (highlight)
    {
        this->PrevValue = this->toPlainText();
    } else {
        this->PrevValue = text;
    }

    this->QGraphicsTextItem::setPlainText(text);

    if (this->HasValueChanged())
    {
        this->eHighlightState = hightlightState_Init;
    }
}

void CustomTextItem::paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget)
{
    switch (this->eHighlightState)
    {
        case hightlightState_Init:
            this->Alpha = 200;
            this->eHighlightState = hightlightState_Draw;
            // fallthrough
        case hightlightState_Draw:
            if (this->Alpha > 0)
            {
                painter->setPen(Qt::NoPen);
                painter->setBrush(QBrush(QColor(255, 0, 0, this->Alpha)));
                painter->drawRect(this->boundingRect());
                this->Alpha -= 5;
                break;
            }

            this->eHighlightState = hightlightState_Stop;
            break;
        default:
            break;
    }

    this->QGraphicsTextItem::paint(painter, option, widget);
}

void CustomTextItem::mousePressEvent(QGraphicsSceneMouseEvent *event)
{
    this->SetSelectionFlags();
    this->QGraphicsTextItem::mousePressEvent(event);
    this->SetTextSelection(true);
}

void CustomTextItem::mouseDoubleClickEvent(QGraphicsSceneMouseEvent *event)
{
    this->SetEditionFlags();
    this->QGraphicsTextItem::mouseDoubleClickEvent(event);
}

// empty on purpose to disable the move behaviour
void CustomTextItem::mouseMoveEvent(QGraphicsSceneMouseEvent *event)
{
}

void CustomTextItem::keyPressEvent(QKeyEvent *event)
{
    QString text = this->toPlainText();
    int key = event->key();

    // not accepting or ignoring the event to prevent closing the window
    if (key == Qt::Key_Escape)
    {
        this->clearFocus();
        return;
    }

    if (this->IsEditing)
    {
        // cursor state before processing the key
        QTextCursor cursor = this->textCursor();
        int cursorPos = cursor.position();
        int anchorPos = cursor.anchor();

        if (this->IsKeyHex(key) && cursorPos != 2) {
            text.remove(cursorPos, 2 - cursorPos);
            this->setPlainText(text, false);
            this->eHighlightState = hightlightState_Init;
            this->setTextCursor(cursor);
        }

        qsizetype textLength = text.length();

        // make sure that:
        // - 1. the item is focused (probably always true though but wanna be safe)
        // - 2. the key is valid, so either an hex digit or other keys like enter or delete
        // - 3. if the key is a hex digit (0-9 A-F), make sure we won't exceed a length of 2
        if (!this->hasFocus() || !this->IsKeyValid(key) || (this->IsKeyHex(key) && !this->IsTextSelected() && (cursorPos + 1) > 2 && (textLength + 1) > 2))
        {
            event->ignore();
            return;
        }

        // turn enter keys to a validator
        if (key == Qt::Key_Enter || key == Qt::Key_Return)
        {
            if (this->IsEditing && !text.isEmpty())
            {
                emit applyEditToRAM(text.toUInt(0, 16), this);
                this->SetSelectionFlags();
                this->SetTextSelection(true);
            }
            event->accept();
            return;
        }

        // if the validation checks pass and it's not the enter key, process it
        this->QGraphicsTextItem::keyPressEvent(event);

        // skip to the next byte and enter edition mode (handled by the signal)
        if (this->IsKeyHex(key) && this->toPlainText().length() == 2 && !this->IsTextSelected())
        {
            emit switchFocus(focusDirection_Right, focusAction_SetEditionModeNoPos);
        }

        // go to the previous or next byte and enter edition mode with the left and right arrows
        if (cursorPos == this->GetCursorPosition() && anchorPos == this->GetCursorAnchor()) {
            switch (key)
            {
                case Qt::Key_Left:
                    emit switchFocus(focusDirection_Left, focusAction_SetEditionMode);
                    break;
                case Qt::Key_Right:
                    emit switchFocus(focusDirection_Right, focusAction_SetEditionMode);
                    break;
                default:
                    break;
            }
        }
    }
    else
    {
        switch (key)
        {
            case Qt::Key_Enter:
            case Qt::Key_Return:
                this->SetEditionFlags();
                break;
            case Qt::Key_Left:
                emit switchFocus(focusDirection_Left, focusAction_SetSelectionMode);
                break;
            case Qt::Key_Up:
                emit switchFocus(focusDirection_Up, focusAction_SetSelectionMode);
                break;
            case Qt::Key_Right:
                emit switchFocus(focusDirection_Right, focusAction_SetSelectionMode);
                break;
            case Qt::Key_Down:
                emit switchFocus(focusDirection_Down, focusAction_SetSelectionMode);
                break;
            default:
                event->ignore();
                return;
        }
    }

    event->accept();
}

void CustomTextItem::focusOutEvent(QFocusEvent *event)
{
    QString text = this->toPlainText();

    // apply the value when the focus is cleared, if necessary
    if (this->IsEditing && !text.isEmpty())
    {
        emit applyEditToRAM(text.toUInt(0, 16), this);
        this->IsEditing = false;
    }

    this->SetTextSelection(false);
    this->QGraphicsTextItem::focusOutEvent(event);
}

void CustomGraphicsScene::wheelEvent(QGraphicsSceneWheelEvent *event)
{
    this->QGraphicsScene::wheelEvent(event);

    if (this->ScrollBar != nullptr)
    {
        QWheelEvent *pEvent = new QWheelEvent(
            event->pos(),
            event->screenPos(),
            QPoint(0, 0),
            QPoint(0, event->delta()),
            event->buttons(),
            event->modifiers(),
            event->phase(),
            event->isInverted()
        );

        QApplication::sendEvent(this->ScrollBar, pEvent);
    }
}

void CustomGraphicsScene::onFocusItemChanged(QGraphicsItem *newFocus, QGraphicsItem *oldFocus, Qt::FocusReason reason)
{
    MemViewDialog* dialog = (MemViewDialog*)this->parent();

    if (dialog != nullptr && newFocus != nullptr && newFocus != dialog->GetAddrLabel())
    {
        uint32_t addr = dialog->GetFocusAddress(newFocus);

        if (addr != -1)
        {
            QString text;
            text.setNum(addr, 16);
            dialog->GetAddrLabel()->setPlainText(text.toUpper().rightJustified(8, '0').prepend("0x"));

            if (dialog->GetFocusCheckbox()->isChecked())
            {
                dialog->GetValueAddrLineEdit()->setText(text.toUpper().rightJustified(8, '0').prepend("0x"));
            }
        }
    }
}

void CustomLineEdit::keyPressEvent(QKeyEvent *event)
{
    MemViewDialog* dialog = (MemViewDialog*)this->parent();
    this->QLineEdit::keyPressEvent(event);

    if (event->key() == Qt::Key_Enter)
    {
        dialog->GoToAddress(this->text());
    }
}

MemViewDialog::MemViewDialog(QWidget* parent) : QDialog(parent)
{
    // set the Dialog's basic properties
    this->setObjectName("MemViewDialog");
    this->setFixedSize(730, 340);
    this->setEnabled(true);
    this->setModal(false);
    this->setWindowTitle("Memory Viewer - melonDS");
    setAttribute(Qt::WA_DeleteOnClose);

    this->Highlight = false;

    QColor placeholderColor = QColor(160, 160, 160);

    // create the widgets, maybe not necessary to keep a reference to everything but whatever
    this->GfxScene = new CustomGraphicsScene(this);
    this->GfxView = new QGraphicsView(this);
    this->ScrollBar = new QScrollBar(this);
    this->UpdateThread = new MemViewThread(this);
    this->GoBtn = new QPushButton(this);
    this->UpdateRateLabel = new QLabel(this);
    this->SearchLineEdit = new CustomLineEdit(this);
    this->UpdateRate = new QSpinBox(this);
    this->MemRegionBox = new QComboBox(this); 
    this->SetValGroup = new QGroupBox(this); 
    this->SetValFocus = new QCheckBox(this->SetValGroup);
    this->SetValIsHex = new QCheckBox(this->SetValGroup);
    this->SetValBits = new QComboBox(this->SetValGroup);
    this->SetValBtn = new QPushButton(this->SetValGroup);
    this->SetValNumber = new QLineEdit(this->SetValGroup);
    this->SetValAddr = new QLineEdit(this->SetValGroup);
    this->DumpBtn = new QPushButton(this);

    this->GoBtn->setText("Go");
    this->GoBtn->setGeometry(102, 19, 50, 34);
    this->GoBtn->setObjectName("pushbutton_go");

    this->SearchLineEdit->setMaxLength(10);
    this->SearchLineEdit->setPlaceholderText("Search...");
    this->SearchLineEdit->setGeometry(8, 20, 91, 32);
    QPalette paletteSearch = this->SearchLineEdit->palette();
    paletteSearch.setColor(QPalette::ColorRole::PlaceholderText, placeholderColor);
    this->SearchLineEdit->setPalette(paletteSearch);
    this->SearchLineEdit->setObjectName("lineedit_search");

    this->SetValFocus->setText("Address on focus");
    this->SetValFocus->setGeometry(4, 126, 131, 22);
    this->SetValFocus->setObjectName("checkbox_setval_addr_focus");

    this->SetValIsHex->setText("Hex Value");
    this->SetValIsHex->setGeometry(4, 106, 131, 22);
    this->SetValIsHex->setChecked(true);
    this->SetValIsHex->setObjectName("checkbox_setval_ishex");

    this->UpdateRateLabel->setText("Update:");
    this->UpdateRateLabel->setGeometry(568, 300, 58, 18);
    this->UpdateRateLabel->setObjectName("label_update_rate");

    this->UpdateRate->setGeometry(621, 293, 90, 32);
    this->UpdateRate->setMinimum(5); // below 5 ms it's causing slowdowns
    this->UpdateRate->setMaximum(10000);
    this->UpdateRate->setValue(20);
    this->UpdateRate->setSuffix(" ms");
    this->UpdateRate->setObjectName("spinbox_update_rate");

    this->DumpBtn->setText("Dump Default");
    this->DumpBtn->setGeometry(7, 259, 145, 32);
    this->DumpBtn->setObjectName("pushbutton_dump");

    this->setFixedSize(730, 404);
    this->SearchLineEdit->setMaxLength(128);
    this->SearchLineEdit->setToolTip("Address or symbol[+offset]");

    this->InfoLabel = new QLabel(this);
    this->InfoLabel->setGeometry(8, 334, 714, 24);
    this->InfoLabel->setFont(MonoFont());
    this->InfoLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    this->InfoLabel->setObjectName("label_info");

    QPushButton* breakWrite = new QPushButton("Break on write", this);
    breakWrite->setGeometry(7, 364, 120, 32);
    breakWrite->setToolTip("Add a write watchpoint on the focused address (size from the bits box)");
    connect(breakWrite, &QPushButton::clicked, this, [this] { this->AddWatch(melonDS::Watch_Write); });

    QPushButton* breakRead = new QPushButton("Break on read", this);
    breakRead->setGeometry(131, 364, 120, 32);
    breakRead->setToolTip("Add a read watchpoint on the focused address (size from the bits box)");
    connect(breakRead, &QPushButton::clicked, this, [this] { this->AddWatch(melonDS::Watch_Read); });

    QPushButton* disasm = new QPushButton("Disassemble", this);
    disasm->setGeometry(255, 364, 110, 32);
    connect(disasm, &QPushButton::clicked, this, [this] {
        uint32_t addr = this->GetFocusAddress(this->GfxScene->focusItem());
        if (addr != (uint32_t)-1)
            DebugTools::showDisassembly(this->GetBridge(), (QWidget*)this->parent(), addr);
    });

    QCheckBox* writers = new QCheckBox("Track last writer", this);
    writers->setGeometry(375, 368, 160, 24);
    writers->setToolTip("Record which instruction or DMA last wrote each word of main RAM");
    connect(writers, &QCheckBox::toggled, this, [this](bool on) {
        this->GetBridge()->post([on](melonDS::NDS& nds) { nds.Debugger.SetLastWriter(on); });
    });

    this->GfxView->setScene(this->GfxScene);
    this->GfxView->setGeometry(160, 10, 550, 280);
    this->GfxView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    this->GfxView->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    this->GfxView->setObjectName("gfxview");

    this->GfxScene->clear();
    QRect rect = this->GfxView->contentsRect();
    this->GfxScene->setSceneRect(0, 0, rect.width(), rect.height());
    this->GfxScene->SetScrollBar(this->ScrollBar);
    this->GfxScene->setObjectName("gfxscene");

    this->ScrollBar->setGeometry(710, 10, 16, 280);
    this->ScrollBar->setSingleStep(16);
    this->ScrollBar->setPageStep(16);
    this->ScrollBar->setOrientation(Qt::Orientation::Vertical);
    this->ScrollBar->setObjectName("scrollbar");

    this->SetValGroup->setGeometry(8, 60, 143, 151);
    this->SetValGroup->setObjectName("groupbox_setval");

    this->SetValBits->addItem("8 bits");
    this->SetValBits->addItem("16 bits");
    this->SetValBits->addItem("32 bits");
    this->SetValBits->setCurrentIndex(0);
    this->SetValBits->setGeometry(6, 75, 68, 30);
    this->SetValBits->setObjectName("combobox_setval_bits");

    this->SetValBtn->setText("Set");
    this->SetValBtn->setGeometry(this->SetValBits->width() + 5, 75, 65, 30);
    this->SetValBtn->setObjectName("pushbutton_setval_set_btn");

    this->SetValNumber->setGeometry(7, 7, 129, 30);
    this->SetValNumber->setPlaceholderText("Value");
    QPalette paletteNum = this->SetValNumber->palette();
    paletteNum.setColor(QPalette::ColorRole::PlaceholderText, placeholderColor);
    this->SetValNumber->setPalette(paletteNum);
    this->SetValNumber->setObjectName("lineedit_setval_value");

    this->SetValAddr->setGeometry(7, 40, 129, 30);
    this->SetValAddr->setPlaceholderText("Address");
    QPalette paletteAddr = this->SetValAddr->palette();
    paletteAddr.setColor(QPalette::ColorRole::PlaceholderText, placeholderColor);
    this->SetValAddr->setPalette(paletteAddr);
    this->SetValAddr->setObjectName("lineedit_setval_addr");

    this->MemRegionBox->setGeometry(7, 218, 145, 32);
    this->FillRegions();
    this->MemRegionBox->setObjectName("combobox_mem_region");

    this->SpaceBox = new QComboBox(this);
    this->SpaceBox->setGeometry(7, 296, 145, 30);
    this->SpaceBox->addItems({"ARM9 bus", "ARM7 bus", "Cart ROM", "Firmware"});
    this->SpaceBox->setObjectName("combobox_space");
    connect(this->SpaceBox, &QComboBox::currentIndexChanged, this, [this](int index) { this->SetSpace(index); });

    QPushButton* textDump = new QPushButton("Text dump", this);
    textDump->setGeometry(600, 364, 122, 32);
    textDump->setObjectName("pushbutton_textdump");
    textDump->setToolTip("Save the region as hex and ASCII text");
    connect(textDump, &QPushButton::clicked, this, [this] { this->DumpSpace(true); });

    // initialize the scene
    QString text;
    QString objName;
    QColor color;
    QFont font("Monospace", 10, -1, false);

    // create current address label
    this->AddrLabel = new QGraphicsTextItem("0x02000000");
    this->AddrLabel->setParent(this->GfxScene);
    this->AddrLabel->setFont(font);
    this->AddrLabel->setPos(0, 0);
    this->AddrLabel->setTextInteractionFlags(Qt::TextInteractionFlag::TextSelectableByMouse);
    this->AddrLabel->setObjectName("item_cur_pos_addr");
    this->GfxScene->addItem(this->AddrLabel);

    // create hex digits texts
    for (int i = 0; i < 16; i++)
    {
        text.setNum(i, 16);
        QGraphicsTextItem* textItem = new QGraphicsTextItem(text.toUpper().rightJustified(2, '0'));
        textItem->setParent(this->GfxScene);
        textItem->setFont(font);
        qreal x = i * textItem->font().pointSize() * 2; // column number * font size * text length
        qreal y = 0; // always zero

        // account for addresses column length
        x += textItem->font().pointSize() * 8;

        objName = "item_hex_digit_col_";
        objName.append(text);
        textItem->setObjectName(objName);
        textItem->setPos(x + 10, y);
        this->GfxScene->addItem(textItem);

        // kinda hacky but we need to sync the line's color on the text
        // and since the text color depends on the system's theme... easy way for now...
        if (i == 0)
        {
            color = textItem->defaultTextColor();
        }

        text.clear();
        objName.clear();
    }

    // create addresses texts
    for (int i = 0; i < 16; i++)
    {
        text.setNum(i * 16 + this->ARM9AddrStart, 16);

        QGraphicsTextItem* textItem = new QGraphicsTextItem(text.toUpper().rightJustified(8, '0').prepend("0x"));
        objName.setNum(i);
        objName.prepend("item_addr_row_");
        textItem->setObjectName(objName);
        textItem->setParent(this->GfxScene);
        textItem->setFont(font);
        textItem->setPos(0, i * textItem->font().pointSize() * 1.5f + textItem->font().pointSize() + 15);
        this->GfxScene->addItem(textItem);
        this->LeftAddrItems[i] = textItem;
        text.clear();
        objName.clear();
    }

    // init memory view
    for (int i = 0; i < 16; i++)
    {
        QString iStr;
        iStr.setNum(i);

        for (int j = 0; j < 16; j++)
        {
            QString jStr;
            jStr.setNum(j);

            CustomTextItem* textItem = new CustomTextItem("00");
            connect(textItem, &CustomTextItem::applyEditToRAM, this, &MemViewDialog::onApplyEditToRAM);
            connect(textItem, &CustomTextItem::switchFocus, this, &MemViewDialog::onSwitchFocus);
            textItem->setParent(this->GfxScene);
            textItem->setFont(font);
            qreal x = j * textItem->font().pointSize() * 2; // column number * font size * text length
            qreal y = 0; // always zero

            // account for addresses column length
            x += textItem->font().pointSize() * 8;
            textItem->setPos(x + 10, i * textItem->font().pointSize() * 1.5f + textItem->font().pointSize() + 15);
            objName = "item_val_";
            objName.append(iStr);
            objName.append("_");
            objName.append(jStr);
            textItem->setObjectName(objName);
            this->GfxScene->addItem(textItem);
            this->RAMTextItems[i][j] = textItem;
            objName.clear();
        }
    }

    // init ascii view header
    QGraphicsTextItem* textItem = new QGraphicsTextItem("0123456789ABCDEF");
    textItem->setParent(this->GfxScene);
    textItem->setFont(font);
    textItem->setPos(416, 0);
    textItem->setObjectName("item_decoded_header");
    this->GfxScene->addItem(textItem);

    // init ascii view
    for (int i = 0; i < 16; i++)
    {
        QGraphicsTextItem* textItem = new QGraphicsTextItem("................");
        textItem->setParent(this->GfxScene);
        textItem->setFont(font);
        textItem->setPos(416, i * textItem->font().pointSize() * 1.5f + textItem->font().pointSize() + 15);
        objName.setNum(i);
        objName.prepend("item_decoded_row_");
        textItem->setObjectName(objName);
        this->GfxScene->addItem(textItem);
        this->AsciiStrings[i] = textItem;
    }

    // add separators
    QGraphicsLineItem* line1 = this->GfxScene->addLine(0, 25, this->GfxScene->width(), 25, QPen(color));
    QGraphicsLineItem* line2 = this->GfxScene->addLine(87, 0, 87, this->GfxScene->height(), QPen(color));
    // QGraphicsLineItem* line3 = this->GfxScene->addLine(415, 0, 415, this->GfxScene->height(), QPen(color));

    melonDS::NDS* nds = this->GetNDS();
    if (nds != nullptr) {
        // should never happen for users but wait until the DTCM address is set just in case
        while (nds->ARM9->DTCMBase == 0xFFFFFFFF) {}

        this->MemRegionBox->setCurrentIndex(memRegionType_Default);
        this->UpdateViewRegion(0x02000000, 0x03000000);
    }

    connect(this->UpdateThread, &MemViewThread::updateSceneSignal, this, &MemViewDialog::onUpdateSceneSignal);
    connect(this->GoBtn, &QPushButton::pressed, this, &MemViewDialog::onGoBtnPressed);
    connect(this->SetValBtn, &QPushButton::pressed, this, &MemViewDialog::onValueBtnSetPressed);
    connect(this->GfxScene, &QGraphicsScene::focusItemChanged, this->GfxScene, &CustomGraphicsScene::onFocusItemChanged);
    connect(this->GfxScene, &QGraphicsScene::focusItemChanged, this, [this] { this->UpdateInfo(); });
    connect(this->ScrollBar, &QScrollBar::valueChanged, this, &MemViewDialog::onScrollBarValueChanged);
    connect(this->MemRegionBox, &QComboBox::currentIndexChanged, this, &MemViewDialog::onMemRegionIndexChanged);
    connect(this->DumpBtn, &QPushButton::pressed, this, &MemViewDialog::onDumpBtnPressed);
    this->UpdateThread->Start();

    qRegisterMetaType<QVector<int>>("QVector<int>");
    qRegisterMetaType<u32>("u32");
    qRegisterMetaType<s32>("s32");
    qRegisterMetaType<s16>("s16");
    qRegisterMetaType<s8>("s8");
}

MemViewDialog::~MemViewDialog()
{
    if (this->UpdateThread)
    {
        disconnect(this->UpdateThread, nullptr, this, nullptr);
        this->UpdateThread->Stop();
        delete this->UpdateThread;
        this->UpdateThread = nullptr;
    }
}

melonDS::NDS* MemViewDialog::GetNDS()
{
    // fetch the emulator's instance and the NDS data from the main window
    // we're doing it each time we need to access the NDS data to prevent a segfault later
    // when accessing MainRAM and DTCM
    EmuInstance* emuInstance = ((MainWindow*)this->parent())->getEmuInstance();

    if (emuInstance)
    {
        return emuInstance->getNDS();
    }

    return nullptr;
}

int MemViewDialog::GetAddressFromItem(CustomTextItem* item) 
{
    if (item != nullptr)
    {
        for (int i = 0; i < 16; i++)
        {
            for (int j = 0; j < 16; j++)
            {
                if (this->RAMTextItems[i][j] == item)
                {
                    QGraphicsTextItem* item = this->LeftAddrItems[i];

                    if (item != nullptr)
                    {
                        return item->toPlainText().remove(0, 2).toUInt(0, 16);
                    }

                    return -1;
                }
            }
        }
    }

    return -1;
}

void MemViewDialog::FillRegions()
{
    QSignalBlocker block(this->MemRegionBox);
    this->MemRegionBox->clear();
    switch (this->Space)
    {
        case DebugTools::memSpace_ARM7:
            this->MemRegionBox->addItems({"Default", "ARM7-BIOS", "Main", "Shared WRAM", "ARM7 WRAM", "VRAM"});
            break;
        case DebugTools::memSpace_CartROM:
            this->MemRegionBox->addItem("Cart ROM");
            break;
        case DebugTools::memSpace_Firmware:
            this->MemRegionBox->addItem("Firmware");
            break;
        default:
            this->MemRegionBox->addItems({"Default", "ITCM", "Main", "DTCM", "Shared WRAM", "Palettes", "OAM", "ARM9-BIOS"});
            break;
    }
}

void MemViewDialog::SetSpace(int space)
{
    if (space < 0 || space > DebugTools::memSpace_Firmware)
    {
        return;
    }
    this->Space = space;
    this->CacheValid = false;
    if (this->SpaceBox->currentIndex() != space)
    {
        QSignalBlocker block(this->SpaceBox);
        this->SpaceBox->setCurrentIndex(space);
    }
    this->FillRegions();
    this->MemRegionBox->setCurrentIndex(0);
    this->onMemRegionIndexChanged(0);
}

void MemViewDialog::RequestCache()
{
    if (this->CachePending)
    {
        return;
    }
    uint32_t base = ((uint32_t)this->ScrollBar->value() + 0xF) & ~0xFu;
    int space = this->Space;
    this->CachePending = true;
    this->GetBridge()->call(this, [base, space](melonDS::NDS& nds) {
        std::array<uint8_t, 256> data;
        data.fill(0);
        const melonDS::NDSCart::CartCommon* cart = nds.GetNDSCart();
        const melonDS::Firmware& fw = nds.GetFirmware();
        for (uint32_t i = 0; i < 256; i++)
        {
            uint32_t addr = base + i;
            if (space == DebugTools::memSpace_ARM7)
                data[i] = nds.Debugger.Peek(1, addr, 1);
            else if (space == DebugTools::memSpace_CartROM)
                data[i] = cart && cart->GetROM() && addr < cart->GetROMLength() ? cart->GetROM()[addr] : 0xFF;
            else if (fw.Buffer() && addr < fw.Length())
                data[i] = fw.Buffer()[addr];
        }
        return std::make_pair(base, data);
    }, [this, space](const std::pair<uint32_t, std::array<uint8_t, 256>>& result) {
        this->CachePending = false;
        if (space != this->Space)
        {
            return;
        }
        this->CacheBase = result.first;
        memcpy(this->Cache, result.second.data(), 256);
        this->CacheValid = true;
    });
}

void* MemViewDialog::GetRAM(uint32_t address)
{
    if (this->Space != DebugTools::memSpace_ARM9)
    {
        if (this->CacheValid && address - this->CacheBase < 256)
        {
            return &this->Cache[address - this->CacheBase];
        }
        return nullptr;
    }

    melonDS::NDS* nds = this->GetNDS();

    if (nds != nullptr)
    {
        if (address < nds->ARM9->ITCMSize)
        {
            return &nds->ARM9->ITCM[address & (ITCMPhysicalSize - 1)];
        }
        else if (nds->ARM9->DTCM != nullptr && (address & nds->ARM9->DTCMMask) == nds->ARM9->DTCMBase)
        {
            return &nds->ARM9->DTCM[address & (DTCMPhysicalSize - 1)];
        }
        else if ((address & 0xFFFFF000) == 0xFFFF0000)
        {
            return (void*)&nds->GetARM9BIOS()[address & 0xFFF];
        }

        // based on NDS::ARM9Read8
        switch (address & 0xFF000000)
        {
            case 0x02000000:
                if (nds->MainRAM != nullptr)
                {
                    return &nds->MainRAM[address & nds->MainRAMMask];
                }
                break;
            case 0x03000000:
                if (nds->SWRAM_ARM9.Mem != nullptr)
                {
                    return &nds->SWRAM_ARM9.Mem[address & nds->SWRAM_ARM9.Mask];
                }
                break;
            case 0x05000000:
                return &nds->GPU.Palette[address & 0x7FF];
            case 0x07000000:
                return &nds->GPU.OAM[address & 0x7FF];
            default:
                break;
        }
    }

    return nullptr;
}

uint32_t MemViewDialog::GetFocusAddress(QGraphicsItem *focus)
{
    if (focus != nullptr)
    {
        uint32_t addr = this->GetAddressFromItem((CustomTextItem*)focus);
        int index = this->GetItemIndex(focus) & 0xFF;

        if (addr >= this->ARM9AddrEnd + 0x100)
        {
            addr = this->ARM9AddrEnd;
        }

        if (index >= 0)
        {
            return addr + index;
        }
    }

    return -1;
}

void MemViewDialog::SwitchMemRegion(uint32_t address) {
    melonDS::NDS* nds = this->GetNDS();
    int index = memRegionType_Default;

    if (nds != nullptr)
    {
        if (address < nds->ARM9->ITCMSize)
        {
            index = memRegionType_ITCM;
        }
        else if ((address & nds->ARM9->DTCMMask) == nds->ARM9->DTCMBase)
        {
            index = memRegionType_DTCM;
        }
        else if ((address & 0xFFFFF000) == 0xFFFF0000)
        {
            index = memRegionType_BIOS;
        }
        else
        {
            switch (address & 0xFF000000)
            {
                case 0x02000000:
                    index = memRegionType_Main;
                    break;
                case 0x03000000:
                    index = memRegionType_SWRAM;
                    break;
                case 0x05000000:
                    index = memRegionType_Palettes;
                    break;
                case 0x07000000:
                    index = memRegionType_OAM;
                default:
                    break;
            }
        }
    }

    this->MemRegionBox->setCurrentIndex(index);
    this->onMemRegionIndexChanged(index);
}

void MemViewDialog::UpdateScene()
{
    if (this->Space != DebugTools::memSpace_ARM9)
    {
        this->RequestCache();
    }

    this->UpdateThread->Pause();

    for (int i = 0; i < 16; i++)
    {
        this->UpdateAddress(i);

        for (int j = 0; j < 16; j++)
        {
            this->UpdateText(i, j);
        }

        this->UpdateDecoded(i);
    }

    this->UpdateThread->Unpause();
}

void MemViewDialog::done(int r)
{
    QDialog::done(r);
    closeDlg();
}

#define ALIGN16(n) ((n + 0xF) & ~0xF)

void MemViewDialog::UpdateText(int addrIndex, int index)
{
    CustomTextItem* item = (CustomTextItem*)this->GetItem(addrIndex, index);
    QString text;

    if (this->ScrollBar == nullptr)
    {
        return;
    }

    uint32_t address = ALIGN16(this->ScrollBar->value()) + addrIndex * 16;
    uint8_t* pRAM = (uint8_t*)this->GetRAM(address + index);

    if (item != nullptr)
    {
        // if pRAM is null (can happen for Shared WRAM if unused), fill with 0x69 to make it obvious
        uint8_t byte = pRAM != nullptr ? *pRAM : 0x69;

        // only update the text when the item isn't focused so we can edit it
        if (!item->hasFocus() || this->ForceTextUpdate)
        {
            text.setNum(byte, 16);
            item->setPlainText(text.toUpper().rightJustified(2, '0'), this->Highlight);
        }

        if (index == 0)
        {
            this->DecodedStrings[addrIndex].clear();
        }

        if (this->DecodedStrings[addrIndex].length() < 16)
        {
            // decode printable characters otherwise just use a dot
            if (byte >= 0x20 && byte <= 0x7E)
            {
                this->DecodedStrings[addrIndex].append(QChar(byte));
            }
            else
            {
                this->DecodedStrings[addrIndex].append(QChar('.'));
            }
        }
    }

    if (this->ForceTextUpdate) {
        this->ForceTextUpdate = false;
    }
}

void MemViewDialog::UpdateAddress(int index)
{
    QGraphicsTextItem* item = (QGraphicsTextItem*)this->GetAddressItem(index);
    QString text;

    if (item != nullptr)
    {
        uint32_t address = ALIGN16(this->ScrollBar->value()) + index * 16;

        text.setNum(address, 16);
        item->setPlainText(text.toUpper().rightJustified(8, '0').prepend("0x"));
    }
}

void MemViewDialog::UpdateDecoded(int index)
{
    QGraphicsTextItem* item = (QGraphicsTextItem*)this->GetAsciiItem(index);
    QString text;

    if (item != nullptr)
    {
        item->setPlainText(this->DecodedStrings[index]);
    }
}

void MemViewDialog::GoToAddress(const QString &text)
{
    QString rawAddr = text;
    uint32_t addr = 0;

    if (!this->GetBridge()->symbols().parseAddress(SymStr(text), addr))
    {
        this->StripStringHex(&rawAddr);

        if (rawAddr.length() < 8)
        {
            return;
        }

        addr = rawAddr.toUInt(0, 16);
    }

    // change memory region if necessary
    if (addr < this->ARM9AddrStart || addr > this->ARM9AddrEnd + 0x100)
    {
        if (this->Space == DebugTools::memSpace_ARM9)
        {
            this->SwitchMemRegion(addr);
        }
        else if (this->Space == DebugTools::memSpace_ARM7)
        {
            this->MemRegionBox->setCurrentIndex(0);
        }
        else
        {
            return;
        }
    }

    // scroll bars are dumb and the `value()` method doesn't always return what you want as the step
    // so we have to align the value...
    uint32_t alignedAddr = ALIGN16(addr);

    // compensate if the alignment makes the address higher than the user input
    if (alignedAddr > addr)
    {
        alignedAddr -= 0x10;
    }

    // to go to the desired address we just have to set the value of the scroll bar
    // since the thread will handle updating the view based on that
    this->ScrollBar->setValue(alignedAddr);

    QString val;
    val.setNum(addr, 16);
    this->AddrLabel->setPlainText(val.toUpper().rightJustified(8, '0').prepend("0x"));

    CustomTextItem* item = (CustomTextItem*)this->GetItem(0, addr - alignedAddr);
    if (item != nullptr)
    {
        this->GfxScene->clearFocus();
        this->GfxScene->setFocusItem(item);
    }
}

void MemViewDialog::onValueBtnSetPressed()
{
    QString text = this->SetValAddr->text();

    // don't do anything if the address/value isn't set
    if (text.isEmpty() || this->SetValNumber->text().isEmpty())
    {
        return;
    }

    this->StripStringHex(&text);
    uint32_t address = text.toUInt(0, 16);

    text = this->SetValNumber->text();
    this->StripStringHex(&text);

    bool isHex = this->SetValIsHex->isChecked() || this->SetValNumber->text().startsWith("0x");
    int base = isHex == true ? 16 : 10;
    u32 value = text.toUInt(0, base);
    u8 size = 1 << this->SetValBits->currentIndex();
    this->PokeSpace(address, size, value);
}

void MemViewDialog::PokeSpace(uint32_t address, u8 size, u32 value)
{
    int space = this->Space;
    if (space == DebugTools::memSpace_CartROM)
    {
        return;
    }
    u8 cpu = this->SpaceCPU();
    this->CacheValid = false;
    this->GetBridge()->post([space, cpu, address, size, value](melonDS::NDS& nds) {
        if (space != DebugTools::memSpace_Firmware)
        {
            nds.Debugger.Poke(cpu, address, size, value);
            return;
        }
        melonDS::Firmware& fw = nds.GetFirmware();
        for (u8 i = 0; i < size; i++)
        {
            if (fw.Buffer() && address + i < fw.Length())
            {
                fw.Buffer()[address + i] = value >> (8 * i);
            }
        }
    });
}

void MemViewDialog::onApplyEditToRAM(uint8_t value, QGraphicsItem *focus)
{
    uint32_t addr = this->GetFocusAddress(focus);
    if (addr == -1)
    {
        return;
    }

    this->PokeSpace(addr, 1, value);
}

DebugBridge* MemViewDialog::GetBridge()
{
    return ((MainWindow*)this->parent())->getEmuInstance()->getDebugBridge();
}

void MemViewDialog::UpdateInfo()
{
    uint32_t addr = this->GetFocusAddress(this->GfxScene->focusItem());
    if (addr == (uint32_t)-1)
    {
        this->InfoLabel->clear();
        return;
    }
    DebugBridge* bridge = this->GetBridge();
    QString text = Hex(addr);
    uint8_t bytes[4];
    bool readable = true;
    for (int i = 0; i < 4 && readable; i++)
    {
        uint8_t* p = (uint8_t*)this->GetRAM(addr + i);
        readable = p != nullptr;
        bytes[i] = readable ? *p : 0;
    }
    if (readable)
    {
        u16 half = bytes[0] | (bytes[1] << 8);
        u32 word = half | (bytes[2] << 16) | ((u32)bytes[3] << 24);
        text += QString("  s8 %1 u16 %2 s16 %3 u32 %4 s32 %5 fx32 %6 BE %7")
            .arg((melonDS::s8)bytes[0]).arg(half).arg((melonDS::s16)half).arg(word).arg((melonDS::s32)word)
            .arg((melonDS::s32)word / 4096.0, 0, 'f', 4).arg(Hex((word >> 24) | ((word >> 8) & 0xFF00) | ((word << 8) & 0xFF0000) | (word << 24)));
    }
    QString sym = this->Space <= DebugTools::memSpace_ARM7 ? SymStr(bridge->symbols().describe(addr)) : QString();
    if (!sym.isEmpty())
    {
        text += "  " + sym;
    }
    this->InfoLabel->setText(text);
    this->InfoLabel->setToolTip(text);
    bridge->call(this, [addr](melonDS::NDS& nds) { return nds.Debugger.GetLastWriter(addr); },
        [this, addr, text](const u32& writer) {
            if (this->GetFocusAddress(this->GfxScene->focusItem()) != addr || !writer)
            {
                return;
            }
            DebugBridge* bridge = this->GetBridge();
            QString by = (writer & 0xFFFFFFF0) == 0xFFFFFFF0
                ? QString("DMA%1").arg((writer >> 1) & 3)
                : Hex(writer & ~1u) + " " + SymStr(bridge->symbols().describe(writer & ~1u));
            this->InfoLabel->setText(text + "    last written by " + by);
        });
}

void MemViewDialog::AddWatch(u8 flags)
{
    uint32_t addr = this->GetFocusAddress(this->GfxScene->focusItem());
    if (addr == (uint32_t)-1)
    {
        return;
    }
    if (this->Space >= DebugTools::memSpace_CartROM)
    {
        this->InfoLabel->setText("Watchpoints need the ARM9 or ARM7 bus");
        return;
    }
    u32 size = 1 << this->SetValBits->currentIndex();
    u8 mask = this->Space == DebugTools::memSpace_ARM7 ? 2 : 1;
    this->GetBridge()->post([addr, size, flags, mask](melonDS::NDS& nds) { nds.Debugger.AddWatchpoint(mask, addr, size, flags); });
    this->InfoLabel->setText(QString("Watchpoint on %1 (%2 bytes); see the Debugger window").arg(Hex(addr)).arg(size));
}

void MemViewDialog::onSwitchFocus(FocusDirection eDirection, FocusAction eAction)
{
    CustomTextItem* focusItem = (CustomTextItem*)this->GfxScene->focusItem();

    if (focusItem == nullptr)
    {
        return;
    }

    int packed = this->GetItemIndex(focusItem);
    int scrollValue = this->ScrollBar->value();

    if (packed == -1)
    {
        return;
    }

    int index = packed & 0xFF;
    int addrIndex = (packed >> 8) & 0xFF;

    /* 
        Different things will happen depending on the position and the direction:
            - direction up: decrease the address index if above 0, otherwise decrease the scroll bar's value by 16,
            - direction down: increase the address index if under 15, otherwise increase the scroll bar's value by 16,
            - direction left: decrease item index if above 0, otherwise set it to 15 and do the same actions as going up,
            - direction right: increase item index if under 15, otherwise set it to 0 and do the same actions as going down,
        and of course return immediately if the direction value is not valid, but that should never happen
    */

    switch (eDirection)
    {
        case focusDirection_Left:
            if (index == 0)
            {
                index = 15;
                // fallthrough
            }
            else
            {
                index--;
                break;
            }
        case focusDirection_Up:
            if (addrIndex == 0)
            {
                scrollValue -= 0x10;
            }
            else
            {
                addrIndex--;
            }
            break;
        case focusDirection_Right:
            if (index == 15)
            {
                index = 0;
                // fallthrough
            }
            else
            {
                index++;
                break;
            }
        case focusDirection_Down:
            if (addrIndex == 15)
            {
                scrollValue += 0x10;
            }
            else
            {
                addrIndex++;
            }
            break;
        default:
            return;
    }

    CustomTextItem* item = (CustomTextItem*)this->GetItem(addrIndex, index);
    if (item != nullptr)
    {
        this->GfxScene->setFocusItem(item);

        if (scrollValue >= this->ARM9AddrStart && scrollValue <= this->ARM9AddrEnd)
        {
            this->ScrollBar->setValue(scrollValue);

            // the focus item doesn't change when scrolling up or down with the arrows
            if (eDirection == focusDirection_Up || eDirection == focusDirection_Down) {
                uint32_t address = ALIGN16(scrollValue) + addrIndex * 16 + index;
                QString newAddr;
                newAddr.setNum(address, 16);
                this->AddrLabel->setPlainText(newAddr.toUpper().rightJustified(8, '0').prepend("0x"));
            }

            switch (eAction) {
                case focusAction_SetSelectionMode:
                    item->SetSelectionFlags();
                    item->SetTextSelection(true);
                    break;
                case focusAction_SetEditionModeNoPos:
                    item->SetTextSelection(true);
                    // fallthrough
                case focusAction_SetEditionMode:
                    item->SetEditionFlags();

                    if (eAction == focusAction_SetEditionMode) {
                        item->SetCursorPosition(eDirection == focusDirection_Left ? 0 : 2);
                    }
                    break;
                default:
                    break;
            }
        }
    }

    this->UpdateScene();
}

void MemViewDialog::onScrollBarValueChanged(int value) {
    CustomTextItem* item = (CustomTextItem*)this->GfxScene->focusItem();
    int packed = this->GetItemIndex(item);

    if (item != nullptr && packed != -1)
    {
        this->ForceTextUpdate = true;
        this->UpdateText(packed >> 8, packed & 0xFF);
        item->SetTextSelection(true);
    }

    this->UpdateScene();
}

void MemViewDialog::onMemRegionIndexChanged(int index) {
    melonDS::NDS* nds = this->GetNDS();

    if (nds != nullptr && this->Space != DebugTools::memSpace_ARM9) {
        static const uint32_t arm7Ranges[6][2] = {
            {0x00000000, 0x08000000}, {0x00000000, 0x00004000}, {0x02000000, 0x02400000},
            {0x03000000, 0x03008000}, {0x03800000, 0x03810000}, {0x06000000, 0x06040000},
        };
        uint32_t newStart = 0;
        uint32_t newEnd = 0;
        if (this->Space == DebugTools::memSpace_ARM7)
        {
            if (index < 0 || index >= 6)
            {
                return;
            }
            newStart = arm7Ranges[index][0];
            newEnd = arm7Ranges[index][1];
        }
        else if (this->Space == DebugTools::memSpace_CartROM)
        {
            const melonDS::NDSCart::CartCommon* cart = nds->GetNDSCart();
            newEnd = cart ? cart->GetROMLength() : 0;
        }
        else
        {
            newEnd = nds->GetFirmware().Length();
        }
        this->CacheValid = false;
        this->UpdateViewRegion(newStart, std::max<uint32_t>(newEnd, newStart + 0x100));
        this->DumpBtn->setText("Dump " + this->MemRegionBox->currentText());
        return;
    }

    if (nds != nullptr) {
        uint32_t newStart;
        uint32_t newEnd;

        switch (index) {
            case memRegionType_Default:
                newStart = 0x02000000;
                newEnd = 0x03000000;
                break;
            case memRegionType_ITCM:
                newStart = 0x01FF8000;
                newEnd = 0x02000000;
                break;
            case memRegionType_Main:
                newStart = 0x02000000;
                newEnd = newStart + nds->MainRAMMask + 1;
                break;
            case memRegionType_DTCM:
                newStart = nds->ARM9->DTCMBase;
                newEnd = nds->ARM9->DTCMBase + DTCMPhysicalSize;
                break;
            case memRegionType_SWRAM:
                newStart = 0x03000000;
                newEnd = newStart + SharedWRAMSize;
                break;
            case memRegionType_Palettes:
                newStart = 0x05000000;
                newEnd = newStart + sizeof(nds->GPU.Palette);
                break;
            case memRegionType_OAM:
                newStart = 0x07000000;
                newEnd = newStart + sizeof(nds->GPU.OAM);
                break;
            case memRegionType_BIOS:
                newStart = 0xFFFF0000;
                newEnd = newStart + ARM9BIOSSize;
                break;
            default:
                return;
        }

        this->UpdateViewRegion(newStart, newEnd);

        QString text = "Dump ";
        text.append(this->MemRegionBox->currentText());
        this->DumpBtn->setText(text);
    }
}

void MemViewDialog::onGoBtnPressed()
{
    this->GoToAddress(this->SearchLineEdit->text());
}

void MemViewDialog::onUpdateSceneSignal()
{
    this->Highlight = true;
    this->UpdateScene();
    this->Highlight = false;
}

void MemViewDialog::DumpSpace(bool text)
{
    QString filter = text ? "Text files (*.txt)" : "Binary files (*.bin)";
    QString path = QFileDialog::getSaveFileName(this, "Save memory dump to... - melonDS", QDir::currentPath(), filter);
    if (path.isEmpty())
    {
        return;
    }
    QString suffix = text ? ".txt" : ".bin";
    if (!path.endsWith(suffix))
    {
        path.append(suffix);
    }
    uint32_t start = this->ARM9AddrStart;
    uint32_t size = this->ARM9AddrEnd - this->ARM9AddrStart;
    int space = this->Space;
    QByteArray data;
    bool ok = this->GetBridge()->run([&](melonDS::NDS& nds) {
        data.resize(size);
        const melonDS::NDSCart::CartCommon* cart = nds.GetNDSCart();
        const melonDS::Firmware& fw = nds.GetFirmware();
        for (uint32_t i = 0; i < size; i++)
        {
            uint32_t addr = start + i;
            uint8_t value = 0;
            if (space == DebugTools::memSpace_ARM9 || space == DebugTools::memSpace_ARM7)
                value = nds.Debugger.Peek(space == DebugTools::memSpace_ARM7 ? 1 : 0, addr, 1);
            else if (space == DebugTools::memSpace_CartROM)
                value = cart && cart->GetROM() && addr < cart->GetROMLength() ? cart->GetROM()[addr] : 0xFF;
            else if (fw.Buffer() && addr < fw.Length())
                value = fw.Buffer()[addr];
            data[i] = (char)value;
        }
    }, 30000);
    if (!ok)
    {
        this->InfoLabel->setText("The emulator thread did not answer");
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
    {
        this->InfoLabel->setText("Cannot write " + path);
        return;
    }
    if (!text)
    {
        file.write(data);
    }
    else
    {
        QByteArray out;
        for (uint32_t row = 0; row < size; row += 16)
        {
            out += QByteArray::number(start + row, 16).toUpper().rightJustified(8, '0') + "  ";
            QByteArray ascii;
            for (uint32_t i = row; i < row + 16 && i < size; i++)
            {
                uint8_t c = (uint8_t)data[i];
                out += QByteArray::number(c, 16).toUpper().rightJustified(2, '0') + " ";
                ascii += (c >= 0x20 && c <= 0x7E) ? (char)c : '.';
            }
            out += " " + ascii + "\n";
        }
        file.write(out);
    }
    this->InfoLabel->setText(QString("Saved %1 bytes to %2").arg(size).arg(QFileInfo(path).fileName()));
}

void MemViewDialog::onDumpBtnPressed()
{
    if (this->Space != DebugTools::memSpace_ARM9)
    {
        this->DumpSpace(false);
        return;
    }

    // get region to save
    uint8_t* pRAM = (uint8_t*)this->GetRAM(this->ARM9AddrStart);
    size_t size = this->ARM9AddrEnd - this->ARM9AddrStart;

    if (pRAM == nullptr)
    {
        return;
    }

    // get file path
    QString path = QFileDialog::getSaveFileName(this, "Save RAM Dump to... - melonDS", QDir::currentPath(), "Binary files (*.bin)");

    if (path.isEmpty())
    {
        return;
    }

    if (!path.endsWith(".bin"))
    {
        path.append(".bin");
    }

    std::string filename = path.toStdString();

    // execute the save
    Platform::FileHandle* file = Platform::OpenFile(filename, Platform::FileMode::Write);

    if (file == nullptr)
    {
        // If the file couldn't be opened...
        return;
    }

    melonDS::u64 result;

    if (this->MemRegionBox->currentIndex() == memRegionType_Default)
    {
        // special case where we want to save both main ram and dtcm in the same file
        //! TODO: is there a better way to achieve that?
        melonDS::NDS* nds = this->GetNDS();

        if (nds != nullptr)
        {
            // create temp buffer to avoid overwriting the actual data
            uint8_t* pBuffer = new uint8_t[size];

            // copy main ram
            memcpy(pBuffer, pRAM, 0x400000);

            // copy dtcm
            memcpy(pBuffer + (nds->ARM9->DTCMBase & 0x00FFFFFF), nds->ARM9->DTCM, DTCMPhysicalSize);

            // write file and free previous alloc
            result = Platform::FileWrite(pBuffer, size, 1, file);
            delete pBuffer;
        }
    }
    else
    {
        result = Platform::FileWrite(pRAM, size, 1, file);
    }

    Platform::CloseFile(file);

    if (result == 0)
    {
        Platform::Log(Platform::Error, "Failed to write ram dump to %s\n", filename.c_str());
    } else {
        Platform::Log(Platform::Info, "File saved successfully to %s!\n", filename.c_str());
    }
}

// --- MemViewThread ---

MemViewThread::~MemViewThread()
{
    this->Stop();
    this->Dialog = nullptr;
}

void MemViewThread::Start()
{
    this->start();
    this->Running = true;
}

void MemViewThread::Stop()
{
    this->Running = false;
    this->quit();
    this->wait();
}

void MemViewThread::run()
{
    while (this->Running)
    {
        if (this->Dialog == nullptr || this->Dialog->UpdateRate == nullptr)
        {
            return;
        }

        if (this->IsPaused)
        {
            continue;
        }

        int time = this->Dialog->UpdateRate->value();

        // make sure it's never below 5 ms, this can happen if you have an empty field
        time = time < 5 ? 5 : time;

        QThread::msleep(time);

        emit updateSceneSignal();
    }
}
