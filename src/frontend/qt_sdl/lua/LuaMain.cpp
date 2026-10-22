#include "lua/LuaMain.h"
#include "lua/LuaDesmume.h"
#include "LuaDebug.h"
#include "debugger/DebugBridge.h"
#include "debugger/FrameTools.h"
#include "Config.h"
#include <QApplication>
#include <QBoxLayout>
#include <QCheckBox>
#include <QDesktopServices>
#include <QFileSystemWatcher>
#include <QMenu>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QFileDialog>
#include <QPushButton>
#include <QScrollBar>

LuaBundle::LuaBundle(LuaConsoleDialog* dialog, EmuInstance* inst)
{
    emuInstance = inst;
    emuThread = emuInstance->getEmuThread();
    luaDialog = dialog;
    overlays = new std::vector<OverlayCanvas>;
    imageHash = new QHash<QString, QImage>;
    luaWidgets = new QList<QWidget*>;
}

LuaConsoleDialog::LuaConsoleDialog(QWidget* parent) : QDialog(parent)
{
    QWidget* w = parent;
    MainWindow* mainWindow;
    for (;;) //copied from ScreenPanel in Screen.cpp
    {
        mainWindow = qobject_cast<MainWindow*>(w);
        if (mainWindow) break;
        w = w->parentWidget();
        if (!w) break;
    }
    bundle = new LuaBundle(this,mainWindow->getEmuInstance());
    console = new LuaConsole(this);
    bar = console->verticalScrollBar();
    buttonPausePlay = new QPushButton("Pause/UnPause",this);
    buttonStartStop = new QPushButton("Stop",this);
    buttonOpenScript = new QPushButton("OpenLuaFile",this);
    commandLine = new QLineEdit(this);
    commandLine->setObjectName("command");
    commandLine->setPlaceholderText("Lua command, e.g. print(emu.getregister(\"pc\"))");

    QPushButton* buttonRestart = new QPushButton("Restart", this);
    buttonRestart->setObjectName("restart");
    QPushButton* buttonEdit = new QPushButton("Edit", this);
    buttonEdit->setObjectName("edit");
    QPushButton* buttonRecent = new QPushButton("Recent", this);
    buttonRecent->setObjectName("recent");
    QMenu* recentMenu = new QMenu(buttonRecent);
    buttonRecent->setMenu(recentMenu);
    QCheckBox* autoReload = new QCheckBox("Reload on change", this);
    autoReload->setObjectName("autoReload");
    watcher = new QFileSystemWatcher(this);

    QVBoxLayout* layout = new QVBoxLayout(this);
    QHBoxLayout* buttons = new QHBoxLayout();
    buttons->addWidget(buttonPausePlay);
    buttons->addWidget(buttonStartStop);
    buttons->addWidget(buttonOpenScript);
    buttons->addWidget(buttonRestart);
    buttons->addWidget(buttonEdit);
    buttons->addWidget(buttonRecent);
    buttons->addWidget(autoReload);
    buttons->addStretch();
    layout->addLayout(buttons);
    layout->addWidget(console);
    layout->addWidget(commandLine);
    resize(640, 360);

    connect(buttonRestart, &QPushButton::clicked, this, [this] {
        if (currentScript.exists())
            loadScript(currentScript.absoluteFilePath());
    });
    connect(buttonEdit, &QPushButton::clicked, this, [this] {
        if (currentScript.exists())
            QDesktopServices::openUrl(QUrl::fromLocalFile(currentScript.absoluteFilePath()));
    });
    connect(recentMenu, &QMenu::aboutToShow, this, [this, recentMenu] {
        recentMenu->clear();
        QStringList recent = recentScripts();
        for (const QString& path : recent)
            recentMenu->addAction(path, this, [this, path] { loadScript(path); });
        if (recent.isEmpty())
            recentMenu->addAction("(none)")->setEnabled(false);
        else
        {
            recentMenu->addSeparator();
            recentMenu->addAction("Clear", this, [this] {
                Config::Table& cfg = bundle->getEmuInstance()->getGlobalConfig();
                cfg.SetString("Lua.RecentScripts", "");
                Config::Save();
            });
        }
    });
    connect(watcher, &QFileSystemWatcher::fileChanged, this, [this, autoReload](const QString& path) {
        if (autoReload->isChecked() && QFileInfo(path) == currentScript)
            QTimer::singleShot(200, this, [this, path] { loadScript(path); });
    });

    connect(buttonOpenScript,&QPushButton::clicked,this,&LuaConsoleDialog::onOpenScript);
    connect(buttonStartStop,&QPushButton::clicked,this,&LuaConsoleDialog::onStop);
    connect(buttonPausePlay,&QPushButton::clicked,this,&LuaConsoleDialog::onPausePlay);
    connect(commandLine, &QLineEdit::returnPressed, this, [this] {
        QString line = commandLine->text();
        if (line.trimmed().isEmpty())
            return;
        commandLine->clear();
        bundle->runLine(line);
    });
    flagClosed = false;
    this->setWindowTitle("Lua Script");
}

void LuaConsoleDialog::closeEvent(QCloseEvent *event)
{
    onStop();
    bundle->closeState();
    bundle->overlays->clear();
    bundle->luaCanvas = nullptr;
    flagClosed = true;
    event->accept();
}

void LuaConsoleDialog::onOpenScript()
{
    QString path = QFileDialog::getOpenFileName(this, "Load Lua Script", QDir::currentPath(), "Lua scripts (*.lua);;All files (*)");
    if (!QFileInfo::exists(path)) return;
    loadScript(path);
}

QStringList LuaConsoleDialog::recentScripts()
{
    Config::Table& cfg = bundle->getEmuInstance()->getGlobalConfig();
    return QString::fromStdString(cfg.GetString("Lua.RecentScripts")).split('\n', Qt::SkipEmptyParts);
}

void LuaConsoleDialog::loadScript(const QString& path)
{
    QFileInfo file(path);
    if (!file.exists()) return;
    currentScript = file;
    bundle->flagNewLua = true;
    setWindowTitle("Lua Script - " + file.fileName());
    if (!watcher->files().isEmpty())
        watcher->removePaths(watcher->files());
    watcher->addPath(file.absoluteFilePath());
    QStringList recent = recentScripts();
    recent.removeAll(file.absoluteFilePath());
    recent.prepend(file.absoluteFilePath());
    while (recent.size() > 10)
        recent.removeLast();
    Config::Table& cfg = bundle->getEmuInstance()->getGlobalConfig();
    cfg.SetString("Lua.RecentScripts", recent.join('\n').toStdString());
    Config::Save();
}

LuaConsole::LuaConsole(QWidget* parent)
{
    this->setParent(parent);
}

void LuaConsole::onGetText(const QString& string)
{
    this->appendPlainText(string);
    QScrollBar* bar = verticalScrollBar();
    bar->setValue(bar->maximum());
}

void LuaBundle::printText(QString string)
{
    LuaConsole* console = this->luaDialog->console;
    if (QThread::currentThread() == console->thread())
        console->onGetText(string);
    else
        QMetaObject::invokeMethod(console, [console, string] { console->onGetText(string); }, Qt::QueuedConnection);
}

static int GuiOnly(lua_State* L)
{
    if (QThread::currentThread() != qApp->thread())
        return luaL_error(L, "%s is not available inside a memory hook; set a flag there and act on it in _Update",
            lua_tostring(L, lua_upvalueindex(2)));
    if (lua_toboolean(L, lua_upvalueindex(3)) && !get_bundle(L)->luaCanvas)
        return luaL_error(L, "%s needs a canvas: call gui.MakeCanvas, then gui.SetCanvas", lua_tostring(L, lua_upvalueindex(2)));
    auto fn = (lua_CFunction)lua_touserdata(L, lua_upvalueindex(1));
    return fn(L);
}

static void GuardLibrary(lua_State* L, const char* lib)
{
    bool gui = std::string(lib) == "gui";
    if (lua_getglobal(L, lib) != LUA_TTABLE)
    {
        lua_pop(L, 1);
        return;
    }
    std::vector<std::pair<std::string, lua_CFunction>> funcs;
    lua_pushnil(L);
    while (lua_next(L, -2))
    {
        if (lua_type(L, -2) == LUA_TSTRING && lua_iscfunction(L, -1))
            funcs.emplace_back(lua_tostring(L, -2), lua_tocfunction(L, -1));
        lua_pop(L, 1);
    }
    for (auto& [name, fn] : funcs)
    {
        lua_pushlightuserdata(L, (void*)fn);
        lua_pushstring(L, (std::string(lib) + "." + name).c_str());
        lua_pushboolean(L, gui && name != "MakeCanvas" && name != "SetCanvas" && name != "clearImageCache");
        lua_pushcclosure(L, GuiOnly, 3);
        lua_setfield(L, -2, name.c_str());
    }
    lua_pop(L, 1);
}

void LuaBundle::closeState()
{
    lua_State* L = luaState;
    if (!L)
        return;
    luaState = nullptr;
    emuInstance->getDebugBridge()->frames().setLuaHook(this, nullptr);
    {
        LuaDebug::Guard guard(L);
        LuaDesmume::Close(L);
        LuaDebug::Unregister(L);
    }
    lua_close(L);
}

void LuaConsole::onClear()
{
    this->clear();
}

LuaFunction::LuaFunction(luaFunctionPointer cf,const char* n,std::vector<LuaFunction*>* container)
{
    this->cfunction = cf;
    this->name = n;
    container->push_back(this);
}

static_assert(sizeof(LuaBundle*) <= LUA_EXTRASPACE,"LUA_EXTRASPACE too small");

LuaBundle* get_bundle(lua_State * L)
{
	LuaBundle* pBundle;
	std::memcpy(&pBundle, lua_getextraspace(L), sizeof(LuaBundle*));
	return pBundle;
}

#define MELON_LUA_HOOK_INSTRUCTION_COUNT 50 //number of vm instructions between hook calls
void luaHookFunction(lua_State* L, lua_Debug *arg)
{
    LuaBundle* bundle = get_bundle(L);
    if (bundle->flagStop and (arg->event == LUA_HOOKCOUNT)) 
        luaL_error(L, "Force Stopped");
}

std::vector<LuaFunction*> definedLuaFunctions;//List of all defined lua functions
QList<LuaLibrary*> luaLibraries;

LuaLibrary::LuaLibrary(const char* libName,std::vector<luaL_Reg>* luaFuncs)
{
    this->libName=libName;
    this->luaFuncs=luaFuncs;
    luaLibraries.push_back(this);
}

void LuaLibrary::load(lua_State* L)
{
    lua_createtable(L, 0, this->luaFuncs->size());
    this->luaFuncs->push_back((luaL_Reg){NULL,NULL});//append sentinel value
    luaL_setfuncs(L,this->luaFuncs->data(),0);
    this->luaFuncs->pop_back();
    lua_setglobal(L,this->libName);
}

void LuaBundle::createLuaState()
{
    if (!flagNewLua) return;
    closeState();
    overlays->clear();
    luaCanvas = nullptr;
    flagNewLua = false;
    flagStop = false;
    QByteArray fileName = luaDialog->currentScript.fileName().toLocal8Bit();
    QString filedir = luaDialog->currentScript.dir().path();
    lua_State* L = newState();
    QDir::setCurrent(filedir);
    printText("Script started: " + luaDialog->currentScript.fileName());
    bool loaded;
    {
        LuaDebug::Guard guard(L);
        QString error;
        loaded = LuaDesmume::Start(L, fileName.data(), error);
        if (!error.isEmpty())
            printText(error);
    }
    luaState = L;
    if (!loaded)
        closeState();
}

void LuaBundle::runLine(const QString& line)
{
    printText("> " + line);
    if (!luaState)
    {
        flagStop = false;
        luaState = newState();
    }
    lua_State* L = luaState;
    LuaDebug::Guard guard(L);
    int base = lua_gettop(L);
    QByteArray code = line.toUtf8();
    QByteArray expr = "return " + code;
    int status = luaL_loadbuffer(L, expr.constData(), expr.size(), "=console");
    if (status != LUA_OK)
    {
        lua_settop(L, base);
        status = luaL_loadbuffer(L, code.constData(), code.size(), "=console");
    }
    if (status == LUA_OK)
        status = lua_pcall(L, 0, LUA_MULTRET, 0);
    if (status != LUA_OK)
    {
        printText(lua_tostring(L, -1));
        lua_settop(L, base);
        return;
    }
    QStringList values;
    int top = lua_gettop(L);
    for (int i = base + 1; i <= top; i++)
    {
        if (lua_isinteger(L, i))
        {
            lua_Integer v = lua_tointeger(L, i);
            values << QString("%1 (0x%2)").arg(v).arg((quint64)(quint32)v, 0, 16);
        }
        else
        {
            values << QString::fromUtf8(luaL_tolstring(L, i, nullptr));
            lua_pop(L, 1);
        }
    }
    lua_settop(L, base);
    if (!values.isEmpty())
        printText(values.join("\t"));
}

lua_State* LuaBundle::newState()
{
    lua_State* L = luaL_newstate();
    LuaBundle* pBundle = this;
    std::memcpy(lua_getextraspace(L), &pBundle, sizeof(LuaBundle*)); //Write a pointer to this LuaBundle into the extra space of the new lua_State
    luaL_openlibs(L);
    for (LuaFunction* function : definedLuaFunctions)
        lua_register(L,function->name,function->cfunction);
    for (LuaLibrary* lLib: luaLibraries)
        lLib->load(L);
    for (const char* lib : {"gui", "forms", "input", "client", "savestate", "console"})
        GuardLibrary(L, lib);

    EmuInstance* inst = emuInstance;
    DebugBridge* bridge = inst->getDebugBridge();
    LuaDebug::Register(L, {
        [inst] { return inst->getNDS(); },
        [this](const std::string& s) { printText(QString::fromStdString(s)); },
        [bridge](const std::function<void(melonDS::NDS&)>& fn) { return bridge->run(fn); },
        [bridge] { bridge->idle(1); },
        [bridge] { bridge->resume(); },
    });
    LuaDesmume::Register(L);
    bridge->frames().setLuaHook(this, LuaDebug::FrameHook(L, &LuaDesmume::Frame));

    lua_sethook(L,&luaHookFunction,LUA_MASKCOUNT,MELON_LUA_HOOK_INSTRUCTION_COUNT);
    return L;
}

void LuaConsoleDialog::onStop()
{
    if (bundle->getLuaState()) 
        bundle->flagStop = true;
}

void LuaConsoleDialog::onPausePlay()
{
    bundle->flagPause = !bundle->flagPause;
}

void LuaConsoleDialog::onLuaUpdate()
{
    bundle->createLuaState();
    bundle->luaUpdate();
}

//Gets Called once a frame
void LuaBundle::luaUpdate()
{
    if (!luaState) return;
    LuaDesmume::UpdateHostInput(emuInstance);
    if (flagStop)
    {
        closeState();
        printText("Script stopped");
        return;
    }
    if (flagPause) return;
    LuaDebug::Guard guard(luaState);
    if (lua_getglobal(luaState,"_Update")!=LUA_TFUNCTION)
        lua_pop(luaState, 1);
    else if (lua_pcall(luaState,0,0,0)!=0)
    {
        //Handel Errors
        printText(lua_tostring(luaState,-1));
        closeState();
        return;
    }
    LuaDebug::RunFrameEnd(luaState);
}

//"Global" lua functions

namespace luaDefinitions
{

#define AddLuaFunction(functPointer,name)LuaFunction name(functPointer,#name,&definedLuaFunctions)

int lua_MelonPrint(lua_State* L)
{
    LuaBundle* bundle = get_bundle(L);
    const char* string = luaL_checkstring(L,1);
    bundle->printText((QString)string);
    return 0;
}
AddLuaFunction(lua_MelonPrint,print); //re-defines gloabal "print" function

}