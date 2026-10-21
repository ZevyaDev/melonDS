/*
    Copyright 2016-2026 melonDS team

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

#include <algorithm>
#include <vector>

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkDiskCache>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

#include "DebugSource.h"

namespace DebugSource
{

namespace
{

constexpr int FetchTimeoutMs = 30000;
constexpr qint64 DiskCacheBytes = 64 << 20;
constexpr int SignatureLines = 2;

QNetworkAccessManager& Net()
{
    static QNetworkAccessManager* net = nullptr;
    if (!net)
    {
        net = new QNetworkAccessManager();
        auto* cache = new QNetworkDiskCache(net);
        cache->setCacheDirectory(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/debugger-sources");
        cache->setMaximumCacheSize(DiskCacheBytes);
        net->setCache(cache);
    }
    return *net;
}

QNetworkReply* Get(const QString& url)
{
    QNetworkRequest req{QUrl(url)};
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    return Net().get(req);
}

bool Succeeded(QNetworkReply* reply)
{
    int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    return reply->error() == QNetworkReply::NoError && (status == 0 || status == 200);
}

bool ReadLocal(const QString& path, QByteArray& out)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    out = f.readAll();
    return true;
}

bool LooksLikeFile(const QString& path)
{
    QString suffix = QFileInfo(path).suffix().toLower();
    return suffix == "yaml" || suffix == "yml" || suffix == "txt" || suffix == "sym";
}

QString WithSlash(const QString& dir)
{
    return dir.endsWith('/') ? dir : dir + '/';
}

bool IsIdent(QChar c)
{
    return c.isLetterOrNumber() || c == '_';
}

bool AtLineStart(const QString& s, int i)
{
    for (int k = i - 1; k >= 0 && s[k] != '\n'; k--)
        if (!s[k].isSpace())
            return false;
    return true;
}

QString MaskCode(const QString& text)
{
    QString out = text;
    int n = text.size();
    auto blank = [&](int at) {
        if (text[at] != '\n')
            out[at] = ' ';
    };
    int i = 0;
    while (i < n)
    {
        QChar c = text[i];
        QChar d = i + 1 < n ? text[i + 1] : QChar();
        if (c == '/' && d == '/')
        {
            while (i < n && text[i] != '\n')
                blank(i++);
        }
        else if (c == '/' && d == '*')
        {
            int end = text.indexOf("*/", i + 2);
            end = end < 0 ? n : end + 2;
            while (i < end)
                blank(i++);
        }
        else if (c == '"' || c == '\'')
        {
            blank(i++);
            while (i < n && text[i] != c && text[i] != '\n')
            {
                if (text[i] == '\\' && i + 1 < n)
                    blank(i++);
                blank(i++);
            }
            if (i < n && text[i] == c)
                blank(i++);
        }
        else if (c == '#' && AtLineStart(text, i))
        {
            while (i < n && text[i] != '\n')
            {
                if (text[i] == '\\' && i + 1 < n && text[i + 1] == '\n')
                {
                    blank(i);
                    i += 2;
                    continue;
                }
                blank(i++);
            }
        }
        else
            i++;
    }
    return out;
}

int MatchClose(const QString& s, int open, QChar o, QChar c)
{
    int depth = 0;
    for (int i = open; i < s.size(); i++)
    {
        if (s[i] == o)
            depth++;
        else if (s[i] == c && --depth == 0)
            return i;
    }
    return -1;
}

int MatchOpen(const QString& s, int close, QChar o, QChar c)
{
    int depth = 0;
    for (int i = close; i >= 0; i--)
    {
        if (s[i] == c)
            depth++;
        else if (s[i] == o && --depth == 0)
            return i;
    }
    return -1;
}

int CountParams(const QString& list)
{
    QString t = list.trimmed();
    if (t.isEmpty() || t == "void")
        return 0;
    int depth = 0, count = 1;
    for (QChar ch : t)
    {
        if (ch == '<' || ch == '(' || ch == '[' || ch == '{')
            depth++;
        else if (ch == '>' || ch == ')' || ch == ']' || ch == '}')
            depth--;
        else if (ch == ',' && depth == 0)
            count++;
    }
    return count;
}

QString StripTemplates(const QString& s)
{
    QString out;
    int depth = 0;
    for (QChar ch : s)
    {
        if (ch == '<')
            depth++;
        else if (ch == '>')
            depth = std::max(0, depth - 1);
        else if (!depth)
            out += ch;
    }
    return out.trimmed();
}

struct Target
{
    QString Method;
    QString Owner;
    int Params = -1;
};

Target ParseTarget(const QString& signature)
{
    Target t;
    QString sig = signature.trimmed();
    sig.remove("(anonymous namespace)::");
    while (sig.endsWith(" const") || sig.endsWith(" volatile"))
        sig = sig.left(sig.lastIndexOf(' '));
    QString qualified = sig;
    if (sig.endsWith(')'))
    {
        int open = MatchOpen(sig, sig.size() - 1, '(', ')');
        if (open > 0)
        {
            t.Params = CountParams(sig.mid(open + 1, sig.size() - open - 2));
            qualified = sig.left(open);
        }
    }
    QStringList parts;
    int depth = 0, start = 0;
    for (int i = 0; i < qualified.size(); i++)
    {
        QChar ch = qualified[i];
        if (ch == '<')
            depth++;
        else if (ch == '>')
            depth--;
        else if (depth == 0 && ch == ' ' && parts.isEmpty())
            start = i + 1;
        else if (depth == 0 && ch == ':' && i + 1 < qualified.size() && qualified[i + 1] == ':')
        {
            parts << qualified.mid(start, i - start);
            start = i + 2;
            i++;
        }
    }
    parts << qualified.mid(start);
    t.Method = StripTemplates(parts.last());
    if (parts.size() > 1)
        t.Owner = StripTemplates(parts[parts.size() - 2]);
    return t;
}

int BodyStart(const QString& code, int i)
{
    static const QStringList specifiers = {"const", "volatile", "noexcept", "override", "final", "throw"};
    int n = code.size();
    while (i < n)
    {
        while (i < n && code[i].isSpace())
            i++;
        if (i >= n)
            return -1;
        if (code[i] == '{')
            return i;
        if (code[i] == ':' && !(i + 1 < n && code[i + 1] == ':'))
        {
            i++;
            while (true)
            {
                while (i < n && (code[i].isSpace() || IsIdent(code[i]) || code[i] == ':' || code[i] == '<' || code[i] == '>'))
                    i++;
                if (i >= n || (code[i] != '(' && code[i] != '{'))
                    return -1;
                int close = MatchClose(code, i, code[i], code[i] == '(' ? ')' : '}');
                if (close < 0)
                    return -1;
                i = close + 1;
                while (i < n && code[i].isSpace())
                    i++;
                if (i < n && code[i] == ',')
                {
                    i++;
                    continue;
                }
                return i < n && code[i] == '{' ? i : -1;
            }
        }
        if (!IsIdent(code[i]))
            return -1;
        int b = i;
        while (i < n && IsIdent(code[i]))
            i++;
        if (!specifiers.contains(code.mid(b, i - b)))
            return -1;
        while (i < n && code[i].isSpace())
            i++;
        if (i < n && code[i] == '(')
        {
            int close = MatchClose(code, i, '(', ')');
            if (close < 0)
                return -1;
            i = close + 1;
        }
    }
    return -1;
}

int SignatureStart(const QStringList& lines, int line)
{
    for (int up = 0; up < SignatureLines && line > 0; up++)
    {
        QString prev = lines[line - 1].trimmed();
        if (prev.isEmpty() || prev.endsWith(';') || prev.endsWith('{') || prev.endsWith('}') || prev.endsWith(':'))
            break;
        line--;
    }
    return line;
}

Span FindCode(const QString& src, const Target& t)
{
    if (t.Method.isEmpty())
        return {};
    QString code = MaskCode(src);
    const QString& m = t.Method;
    int bestScore = -1, bestStart = -1, bestEnd = -1;
    for (int at = code.indexOf(m); at >= 0; at = code.indexOf(m, at + 1))
    {
        if (at > 0 && IsIdent(code[at - 1]))
            continue;
        int after = at + m.size();
        if (after < code.size() && IsIdent(code[after]))
            continue;
        int open = after;
        while (open < code.size() && code[open].isSpace())
            open++;
        if (open >= code.size() || code[open] != '(')
            continue;
        int close = MatchClose(code, open, '(', ')');
        if (close < 0)
            continue;
        int body = BodyStart(code, close + 1);
        if (body < 0)
            continue;

        int q = at - 1;
        while (q >= 0 && code[q].isSpace())
            q--;
        bool qualified = q >= 1 && code[q] == ':' && code[q - 1] == ':';
        QString owner;
        if (qualified)
        {
            int e = q - 2;
            while (e >= 0 && code[e].isSpace())
                e--;
            if (e >= 0 && code[e] == '>')
            {
                e = MatchOpen(code, e, '<', '>') - 1;
                while (e >= 0 && code[e].isSpace())
                    e--;
            }
            int b = e;
            while (b >= 0 && IsIdent(code[b]))
                b--;
            owner = code.mid(b + 1, e - b);
        }
        int score;
        if (t.Owner.isEmpty())
            score = qualified ? -1 : 4;
        else
            score = qualified ? (owner == t.Owner ? 4 : -1) : 1;
        if (score < 0)
            continue;
        if (t.Params >= 0 && CountParams(code.mid(open + 1, close - open - 1)) == t.Params)
            score += 2;
        if (score > bestScore)
        {
            int end = MatchClose(code, body, '{', '}');
            bestScore = score;
            bestStart = at;
            bestEnd = end < 0 ? code.size() - 1 : end;
        }
    }
    if (bestStart < 0)
        return {};
    QStringList lines = code.split('\n');
    Span span;
    span.First = SignatureStart(lines, code.left(bestStart).count('\n'));
    span.Last = code.left(bestEnd).count('\n');
    return span;
}

Span FindAsm(const QString& src, const QString& name)
{
    QStringList lines = src.split('\n');
    QString escaped = QRegularExpression::escape(name);
    QRegularExpression start("func_start\\s+" + escaped + "\\b");
    QRegularExpression label("^\\s*" + escaped + "\\s*:");
    QRegularExpression global("\\.glob(a)?l\\s+" + escaped + "\\b");
    int first = -1;
    for (int i = 0; i < lines.size() && first < 0; i++)
        if (start.match(lines[i]).hasMatch())
            first = i;
    for (int i = 0; i < lines.size() && first < 0; i++)
        if (label.match(lines[i]).hasMatch())
            first = i > 0 && global.match(lines[i - 1]).hasMatch() ? i - 1 : i;
    if (first < 0)
        return {};
    int last = first;
    for (int i = first + 2; i < lines.size(); i++)
    {
        if (lines[i].contains("func_end"))
        {
            last = i;
            break;
        }
        if (lines[i].contains("func_start") || lines[i].contains(".global") || lines[i].contains(".globl"))
            break;
        last = i;
    }
    while (last > first && lines[last].trimmed().isEmpty())
        last--;
    return {first, last};
}

}

bool IsRemote(const QString& location)
{
    return location.startsWith("http://", Qt::CaseInsensitive) || location.startsWith("https://", Qt::CaseInsensitive);
}

QString Normalize(const QString& raw)
{
    QString location = raw.trimmed();
    if (!IsRemote(location))
    {
        if (QFileInfo(location).isDir())
            return QDir(location).filePath("config.yaml");
        return location;
    }
    QUrl url(location);
    url.setFragment(QString());
    if (url.host() == "github.com")
    {
        QStringList parts = url.path().split('/', Qt::SkipEmptyParts);
        if (parts.size() >= 4 && (parts[2] == "blob" || parts[2] == "tree" || parts[2] == "raw"))
        {
            parts.removeAt(2);
            url.setHost("raw.githubusercontent.com");
            url.setPath("/" + parts.join('/'));
            url.setQuery(QString());
        }
    }
    if (!LooksLikeFile(url.path()))
        url.setPath(WithSlash(url.path()) + "config.yaml");
    return url.toString();
}

QString DirOf(const QString& file)
{
    if (IsRemote(file))
        return QUrl(file).resolved(QUrl(".")).toString();
    return QFileInfo(file).absolutePath();
}

QString Join(const QString& dir, const QString& relative)
{
    if (IsRemote(dir))
        return QUrl(WithSlash(dir)).resolved(QUrl(relative)).toString();
    return QDir::cleanPath(QDir(dir).filePath(relative));
}

QString Parent(const QString& dir)
{
    if (IsRemote(dir))
    {
        QString up = QUrl(WithSlash(dir)).resolved(QUrl("..")).toString();
        return up == WithSlash(dir) ? QString() : up;
    }
    QDir d(QDir::cleanPath(dir));
    return d.cdUp() ? d.absolutePath() : QString();
}

bool Exists(const QString& location)
{
    return !IsRemote(location) && QFileInfo::exists(location);
}

void ReadAll(const QStringList& locations, QHash<QString, QByteArray>& out)
{
    std::vector<std::pair<QString, QNetworkReply*>> replies;
    QSet<QString> seen;
    for (const QString& loc : locations)
    {
        if (loc.isEmpty() || out.contains(loc) || seen.contains(loc))
            continue;
        seen.insert(loc);
        if (!IsRemote(loc))
        {
            QByteArray data;
            if (ReadLocal(loc, data))
                out.insert(loc, data);
            continue;
        }
        replies.emplace_back(loc, Get(loc));
    }
    if (replies.empty())
        return;
    QEventLoop loop;
    int left = (int)replies.size();
    for (auto& [loc, reply] : replies)
        QObject::connect(reply, &QNetworkReply::finished, &loop, [&left, &loop] {
            if (--left == 0)
                loop.quit();
        });
    QTimer::singleShot(FetchTimeoutMs, &loop, &QEventLoop::quit);
    loop.exec();
    for (auto& [loc, reply] : replies)
    {
        if (!reply->isFinished())
            reply->abort();
        else if (Succeeded(reply))
            out.insert(loc, reply->readAll());
        reply->deleteLater();
    }
}

void Read(const QString& location, QObject* context, std::function<void(bool ok, const QByteArray& data)> done)
{
    if (!IsRemote(location))
    {
        QByteArray data;
        bool ok = ReadLocal(location, data);
        done(ok, data);
        return;
    }
    QNetworkReply* reply = Get(location);
    QTimer::singleShot(FetchTimeoutMs, reply, [reply] {
        if (!reply->isFinished())
            reply->abort();
    });
    QObject::connect(reply, &QNetworkReply::finished, context, [reply, done] {
        bool ok = Succeeded(reply);
        done(ok, ok ? reply->readAll() : QByteArray());
    });
    QObject::connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
}

QString BrowseUrl(const QString& location, int first, int last)
{
    if (!IsRemote(location))
        return QUrl::fromLocalFile(location).toString();
    QUrl url(location);
    QStringList parts = url.path().split('/', Qt::SkipEmptyParts);
    if (url.host() != "raw.githubusercontent.com" || parts.size() < 4)
        return location;
    parts.insert(2, "blob");
    url.setHost("github.com");
    url.setPath("/" + parts.join('/'));
    if (first >= 0)
        url.setFragment(last > first ? QString("L%1-L%2").arg(first + 1).arg(last + 1) : QString("L%1").arg(first + 1));
    return url.toString();
}

Span FindFunction(const QString& text, const QString& file, const QString& name, const QString& pretty)
{
    QString src = text;
    src.remove('\r');
    QString suffix = QFileInfo(file).suffix().toLower();
    if (suffix == "s" || suffix == "asm" || suffix == "inc")
        return FindAsm(src, name);
    Span span = FindCode(src, ParseTarget(pretty.isEmpty() ? name : pretty));
    if (span.First < 0 && !pretty.isEmpty() && pretty != name)
        span = FindCode(src, ParseTarget(name));
    return span;
}

}
