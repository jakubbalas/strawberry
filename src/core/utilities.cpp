/*
 * Strawberry Music Player
 * This file was part of Clementine.
 * Copyright 2010, David Sansome <me@davidsansome.com>
 * Copyright 2018-2021, Jonas Kvinge <jonas@jkvinge.net>
 *
 * Strawberry is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Strawberry is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Strawberry.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "config.h"

#include <memory>
#include <cstdlib>
#include <iconv.h>

#include <QtGlobal>
#include <QApplication>
#include <QCoreApplication>
#include <QWindow>
#include <QWidget>
#include <QObject>
#include <QIODevice>
#include <QByteArray>
#include <QMetaObject>
#include <QChar>
#include <QCryptographicHash>
#include <QDate>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QRegularExpression>
#include <QRegularExpressionMatch>
#include <QSize>
#include <QColor>
#include <QRegion>
#include <QMetaEnum>
#include <QXmlStreamReader>
#include <QSettings>
#include <QtEvents>
#include <QMessageBox>
#include <QNetworkInterface>
#include <QMimeDatabase>
#include <QtDebug>
#include <QRandomGenerator>

#include <cstdio>

#ifdef Q_OS_LINUX
#  include <unistd.h>
#  include <sys/syscall.h>
#endif
#ifdef Q_OS_MACOS
#  include <sys/resource.h>
#  include <sys/sysctl.h>
#  include <sys/param.h>
#endif

#if defined(Q_OS_UNIX)
#  include <sys/statvfs.h>
#elif defined(Q_OS_WIN)
#  include <windows.h>
#  include <dwmapi.h>
#endif

#ifdef Q_OS_MACOS
#  include "CoreServices/CoreServices.h"
#  include "IOKit/ps/IOPSKeys.h"
#  include "IOKit/ps/IOPowerSources.h"
#endif

#include "core/logging.h"
#include "core/song.h"

#include "utilities.h"
#include "timeconstants.h"
#include "application.h"

#ifdef Q_OS_MACOS
#  include "mac_startup.h"
#  include "mac_utilities.h"
#  include "scoped_cftyperef.h"
#endif

namespace Utilities {

static QString tr(const char *str) {
  return QCoreApplication::translate("", str);
}

QString PrettyTimeDelta(const int seconds) {
  return (seconds >= 0 ? "+" : "-") + PrettyTime(seconds);
}

QString PrettyTime(int seconds) {

  // last.fm sometimes gets the track length wrong, so you end up with negative times.
  seconds = qAbs(seconds);

  int hours = seconds / (60 * 60);
  int minutes = (seconds / 60) % 60;
  seconds %= 60;

  QString ret;
  if (hours > 0) ret = QString::asprintf("%d:%02d:%02d", hours, minutes, seconds);
  else ret = QString::asprintf("%d:%02d", minutes, seconds);

  return ret;

}

QString PrettyTimeNanosec(const qint64 nanoseconds) {
  return PrettyTime(static_cast<int>(nanoseconds / kNsecPerSec));
}

QString WordyTime(const quint64 seconds) {

  quint64 days = seconds / (60 * 60 * 24);

  // TODO: Make the plural rules translatable
  QStringList parts;

  if (days > 0) parts << (days == 1 ? tr("1 day") : tr("%1 days").arg(days));
  parts << PrettyTime(static_cast<int>(seconds - days * 60 * 60 * 24));

  return parts.join(" ");

}

QString WordyTimeNanosec(const quint64 nanoseconds) {
  return WordyTime(nanoseconds / kNsecPerSec);
}

QString Ago(const qint64 seconds_since_epoch, const QLocale &locale) {

  const QDateTime now = QDateTime::currentDateTime();
  const QDateTime then = QDateTime::fromSecsSinceEpoch(seconds_since_epoch);
  const qint64 days_ago = then.date().daysTo(now.date());
  const QString time = then.time().toString(locale.timeFormat(QLocale::ShortFormat));

  if (days_ago == 0) return tr("Today") + " " + time;
  if (days_ago == 1) return tr("Yesterday") + " " + time;
  if (days_ago <= 7) return tr("%1 days ago").arg(days_ago);

  return then.date().toString(locale.dateFormat(QLocale::ShortFormat));

}

QString PrettyFutureDate(const QDate date) {

  const QDate now = QDate::currentDate();
  const qint64 delta_days = now.daysTo(date);

  if (delta_days < 0) return QString();
  if (delta_days == 0) return tr("Today");
  if (delta_days == 1) return tr("Tomorrow");
  if (delta_days <= 7) return tr("In %1 days").arg(delta_days);
  if (delta_days <= 14) return tr("Next week");

  return tr("In %1 weeks").arg(delta_days / 7);

}

QString PrettySize(const quint64 bytes) {

  QString ret;

  if (bytes > 0) {
    if (bytes <= 1000) {
      ret = QString::number(bytes) + " bytes";
    }
    else if (bytes <= 1000 * 1000) {
      ret = QString::asprintf("%.1f KB", static_cast<float>(bytes) / 1000.0F);
    }
    else if (bytes <= 1000 * 1000 * 1000) {
      ret = QString::asprintf("%.1f MB", static_cast<float>(bytes) / (1000.0F * 1000.0F));
    }
    else {
      ret = QString::asprintf("%.1f GB", static_cast<float>(bytes) / (1000.0F * 1000.0F * 1000.0F));
    }
  }
  return ret;

}

quint64 FileSystemCapacity(const QString &path) {

#if defined(Q_OS_UNIX)
  struct statvfs fs_info {};
  if (statvfs(path.toLocal8Bit().constData(), &fs_info) == 0)
    return quint64(fs_info.f_blocks) * quint64(fs_info.f_bsize);
#elif defined(Q_OS_WIN32)
  _ULARGE_INTEGER ret;
  ScopedWCharArray wchar(QDir::toNativeSeparators(path));
  if (GetDiskFreeSpaceEx(wchar.get(), nullptr, &ret, nullptr) != 0)
    return ret.QuadPart;
#endif

  return 0;

}

quint64 FileSystemFreeSpace(const QString &path) {

#if defined(Q_OS_UNIX)
  struct statvfs fs_info {};
  if (statvfs(path.toLocal8Bit().constData(), &fs_info) == 0)
    return quint64(fs_info.f_bavail) * quint64(fs_info.f_bsize);
#elif defined(Q_OS_WIN32)
  _ULARGE_INTEGER ret;
  ScopedWCharArray wchar(QDir::toNativeSeparators(path));
  if (GetDiskFreeSpaceEx(wchar.get(), &ret, nullptr, nullptr) != 0)
    return ret.QuadPart;
#endif

  return 0;

}

bool MoveToTrashRecursive(const QString &path) {

#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
  QDir dir(path);
  for (const QString &child : dir.entryList(QDir::NoDotAndDotDot | QDir::Dirs | QDir::Hidden)) {
    if (!MoveToTrashRecursive(path + "/" + child)) {
      return false;
    }
  }

  for (const QString &child : dir.entryList(QDir::NoDotAndDotDot | QDir::Files | QDir::Hidden)) {
    if (!QFile::moveToTrash(path + "/" + child)) {
      return false;
    }
  }

  return dir.rmdir(path);

#else
  Q_UNUSED(path)
  return false;

#endif

}

bool RemoveRecursive(const QString &path) {

  QDir dir(path);
  for (const QString &child : dir.entryList(QDir::NoDotAndDotDot | QDir::Dirs | QDir::Hidden)) {
    if (!RemoveRecursive(path + "/" + child)) {
      return false;
    }
  }

  for (const QString &child : dir.entryList(QDir::NoDotAndDotDot | QDir::Files | QDir::Hidden)) {
    if (!QFile::remove(path + "/" + child)) {
      return false;
    }
  }

  return dir.rmdir(path);

}

bool CopyRecursive(const QString &source, const QString &destination) {

  // Make the destination directory
  QString dir_name = source.section('/', -1, -1);
  QString dest_path = destination + "/" + dir_name;
  QDir().mkpath(dest_path);

  QDir dir(source);
  for (const QString &child : dir.entryList(QDir::NoDotAndDotDot | QDir::Dirs)) {
    if (!CopyRecursive(source + "/" + child, dest_path)) {
      qLog(Warning) << "Failed to copy dir" << source + "/" + child << "to" << dest_path;
      return false;
    }
  }

  for (const QString &child : dir.entryList(QDir::NoDotAndDotDot | QDir::Files)) {
    if (!QFile::copy(source + "/" + child, dest_path + "/" + child)) {
      qLog(Warning) << "Failed to copy file" << source + "/" + child << "to" << dest_path;
      return false;
    }
  }
  return true;

}

bool Copy(QIODevice *source, QIODevice *destination) {

  if (!source->open(QIODevice::ReadOnly)) return false;

  if (!destination->open(QIODevice::WriteOnly)) return false;

  const qint64 bytes = source->size();
  std::unique_ptr<char[]> data(new char[bytes]);
  qint64 pos = 0;

  qint64 bytes_read = 0;
  do {
    bytes_read = source->read(data.get() + pos, bytes - pos);
    if (bytes_read == -1) return false;

    pos += bytes_read;
  } while (bytes_read > 0 && pos != bytes);

  pos = 0;
  qint64 bytes_written = 0;
  do {
    bytes_written = destination->write(data.get() + pos, bytes - pos);
    if (bytes_written == -1) return false;

    pos += bytes_written;
  } while (bytes_written > 0 && pos != bytes);

  return true;

}

QString ColorToRgba(const QColor &c) {

  return QString("rgba(%1, %2, %3, %4)")
      .arg(c.red())
      .arg(c.green())
      .arg(c.blue())
      .arg(c.alpha());

}

#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
void OpenInFileManager(const QString &path, const QUrl &url);
void OpenInFileManager(const QString &path, const QUrl &url) {

  if (!url.isLocalFile()) return;

  QProcess proc;
#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
  proc.startCommand("xdg-mime query default inode/directory");
#else
  proc.start("xdg-mime", QStringList() << "query" << "default" << "inode/directory");
#endif
  proc.waitForFinished();
  QString desktop_file = proc.readLine().simplified();
  QStringList data_dirs = QString(qgetenv("XDG_DATA_DIRS")).split(":");

  QString command;
  QStringList command_params;
  for (const QString &data_dir : data_dirs) {
    QString desktop_file_path = QString("%1/applications/%2").arg(data_dir, desktop_file);
    if (!QFile::exists(desktop_file_path)) continue;
    QSettings setting(desktop_file_path, QSettings::IniFormat);
    setting.beginGroup("Desktop Entry");
    if (setting.contains("Exec")) {
      QString cmd = setting.value("Exec").toString();
      if (cmd.isEmpty()) break;
      cmd = cmd.remove(QRegularExpression("[%][a-zA-Z]*( |$)", QRegularExpression::CaseInsensitiveOption));
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
      command_params = cmd.split(' ', Qt::SkipEmptyParts);
#else
      command_params = cmd.split(' ', QString::SkipEmptyParts);
#endif
      command = command_params.first();
      command_params.removeFirst();
    }
    setting.endGroup();
    if (!command.isEmpty()) break;
  }

  if (command.startsWith("/usr/bin/")) {
    command = command.split("/").last();
  }

  if (command.isEmpty() || command == "exo-open") {
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
  }
  else if (command.startsWith("nautilus")) {
    proc.startDetached(command, QStringList() << command_params << "--select" << url.toLocalFile());
  }
  else if (command.startsWith("dolphin") || command.startsWith("konqueror") || command.startsWith("kfmclient")) {
    proc.startDetached(command, QStringList() << command_params << "--select" << "--new-window" << url.toLocalFile());
  }
  else if (command.startsWith("caja")) {
    proc.startDetached(command, QStringList() << command_params << "--no-desktop" << path);
  }
  else if (command.startsWith("pcmanfm") || command.startsWith("thunar")) {
    proc.startDetached(command, QStringList() << command_params << path);
  }
  else {
    proc.startDetached(command, QStringList() << command_params << url.toLocalFile());
  }

}
#endif

#ifdef Q_OS_MACOS
// Better than openUrl(dirname(path)) - also highlights file at path
void RevealFileInFinder(const QString &path) {
  QProcess::execute("/usr/bin/open", QStringList() << "-R" << path);
}
#endif  // Q_OS_MACOS

#ifdef Q_OS_WIN
void ShowFileInExplorer(const QString &path);
void ShowFileInExplorer(const QString &path) {
  QProcess::execute("explorer.exe", QStringList() << "/select," << QDir::toNativeSeparators(path));
}
#endif

void OpenInFileBrowser(const QList<QUrl> &urls) {

  QMap<QString, QUrl> dirs;

  for (const QUrl &url : urls) {
    if (!url.isLocalFile()) {
      continue;
    }
    QString path = url.toLocalFile();
    if (!QFile::exists(path)) continue;

    const QString directory = QFileInfo(path).dir().path();
    if (dirs.contains(directory)) continue;
    dirs.insert(directory, url);
  }

  if (dirs.count() > 50) {
    QMessageBox messagebox(QMessageBox::Critical, tr("Show in file browser"), tr("Too many songs selected."));
    messagebox.exec();
    return;
  }

  if (dirs.count() > 5) {
    QMessageBox messagebox(QMessageBox::Information, tr("Show in file browser"), tr("%1 songs in %2 different directories selected, are you sure you want to open them all?").arg(urls.count()).arg(dirs.count()), QMessageBox::Open|QMessageBox::Cancel);
    messagebox.setTextFormat(Qt::RichText);
    int result = messagebox.exec();
    switch (result) {
      case QMessageBox::Open:
        break;
      case QMessageBox::Cancel:
      default:
        return;
    }
  }

  QMap<QString, QUrl>::iterator i;
  for (i = dirs.begin(); i != dirs.end(); ++i) {
#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
    OpenInFileManager(i.key(), i.value());
#elif defined(Q_OS_MACOS)
    // Revealing multiple files in the finder only opens one window, so it also makes sense to reveal at most one per directory
    RevealFileInFinder(i.value().toLocalFile());
#elif defined(Q_OS_WIN32)
    ShowFileInExplorer(i.value().toLocalFile());
#endif
  }

}

QByteArray Hmac(const QByteArray &key, const QByteArray &data, const QCryptographicHash::Algorithm method) {

  constexpr int block_size = 64;
  Q_ASSERT(key.length() <= block_size);

  QByteArray inner_padding(block_size, static_cast<char>(0x36));
  QByteArray outer_padding(block_size, static_cast<char>(0x5c));

  for (int i = 0; i < key.length(); ++i) {
    inner_padding[i] = static_cast<char>(inner_padding[i] ^ key[i]);
    outer_padding[i] = static_cast<char>(outer_padding[i] ^ key[i]);
  }

  QByteArray part;
  part.append(inner_padding);
  part.append(data);

  QByteArray total;
  total.append(outer_padding);
  total.append(QCryptographicHash::hash(part, method));

  return QCryptographicHash::hash(total, method);

}

QByteArray HmacSha256(const QByteArray &key, const QByteArray &data) {
  return Hmac(key, data, QCryptographicHash::Sha256);
}

QByteArray HmacMd5(const QByteArray &key, const QByteArray &data) {
  return Hmac(key, data, QCryptographicHash::Md5);
}

QByteArray HmacSha1(const QByteArray &key, const QByteArray &data) {
  return Hmac(key, data, QCryptographicHash::Sha1);
}

QByteArray Sha1CoverHash(const QString &artist, const QString &album) {

  QCryptographicHash hash(QCryptographicHash::Sha1);
  hash.addData(artist.toLower().toUtf8().constData());
  hash.addData(album.toLower().toUtf8().constData());

  return hash.result();

}

QString PrettySize(const QSize size) {
  return QString::number(size.width()) + "x" + QString::number(size.height());
}

void ConsumeCurrentElement(QXmlStreamReader *reader) {

  int level = 1;
  while (level != 0 && !reader->atEnd()) {
    switch (reader->readNext()) {
      case QXmlStreamReader::StartElement: ++level; break;
      case QXmlStreamReader::EndElement: --level; break;
      default: break;
    }
  }

}

bool ParseUntilElement(QXmlStreamReader *reader, const QString &name) {

  while (!reader->atEnd()) {
    QXmlStreamReader::TokenType type = reader->readNext();
    if (type == QXmlStreamReader::StartElement && reader->name() == name) {
      return true;
    }
  }
  return false;

}

bool ParseUntilElementCI(QXmlStreamReader *reader, const QString &name) {

  while (!reader->atEnd()) {
    QXmlStreamReader::TokenType type = reader->readNext();
    if (type == QXmlStreamReader::StartElement) {
      QString element = reader->name().toString().toLower();
      if (element == name) {
        return true;
      }
    }
  }

  return false;

}

QDateTime ParseRFC822DateTime(const QString &text) {

  QRegularExpression regexp("(\\d{1,2}) (\\w{3,12}) (\\d+) (\\d{1,2}):(\\d{1,2}):(\\d{1,2})");
  QRegularExpressionMatch re_match = regexp.match(text);
  if (!re_match.hasMatch()) {
    return QDateTime();
  }

  enum class MatchNames { DAYS = 1, MONTHS, YEARS, HOURS, MINUTES, SECONDS };

  QMap<QString, int> monthmap;
  monthmap["Jan"] = 1;
  monthmap["Feb"] = 2;
  monthmap["Mar"] = 3;
  monthmap["Apr"] = 4;
  monthmap["May"] = 5;
  monthmap["Jun"] = 6;
  monthmap["Jul"] = 7;
  monthmap["Aug"] = 8;
  monthmap["Sep"] = 9;
  monthmap["Oct"] = 10;
  monthmap["Nov"] = 11;
  monthmap["Dec"] = 12;
  monthmap["January"] = 1;
  monthmap["February"] = 2;
  monthmap["March"] = 3;
  monthmap["April"] = 4;
  monthmap["May"] = 5;
  monthmap["June"] = 6;
  monthmap["July"] = 7;
  monthmap["August"] = 8;
  monthmap["September"] = 9;
  monthmap["October"] = 10;
  monthmap["November"] = 11;
  monthmap["December"] = 12;

  const QDate date(re_match.captured(static_cast<int>(MatchNames::YEARS)).toInt(), monthmap[re_match.captured(static_cast<int>(MatchNames::MONTHS))], re_match.captured(static_cast<int>(MatchNames::DAYS)).toInt());

  const QTime time(re_match.captured(static_cast<int>(MatchNames::HOURS)).toInt(), re_match.captured(static_cast<int>(MatchNames::MINUTES)).toInt(), re_match.captured(static_cast<int>(MatchNames::SECONDS)).toInt());

  return QDateTime(date, time);

}

const char *EnumToString(const QMetaObject &meta, const char *name, const int value) {

  int index = meta.indexOfEnumerator(name);
  if (index == -1) return "[UnknownEnum]";
  QMetaEnum metaenum = meta.enumerator(index);
  const char *result = metaenum.valueToKey(value);
  if (!result) return "[UnknownEnumValue]";
  return result;

}

QStringList Prepend(const QString &text, const QStringList &list) {

  QStringList ret(list);
  for (int i = 0; i < ret.count(); ++i) ret[i].prepend(text);
  return ret;

}

QStringList Updateify(const QStringList &list) {

  QStringList ret(list);
  for (int i = 0; i < ret.count(); ++i) ret[i].prepend(ret[i] + " = :");
  return ret;

}

QString DecodeHtmlEntities(const QString &text) {

  QString copy(text);
  copy.replace("&amp;", "&")
      .replace("&#38;", "&")
      .replace("&quot;", "\"")
      .replace("&#34;", "\"")
      .replace("&apos;", "'")
      .replace("&#39;", "'")
      .replace("&lt;", "<")
      .replace("&#60;", "<")
      .replace("&gt;", ">")
      .replace("&#62;", ">")
      .replace("&#x27;", "'");

  return copy;

}

long SetThreadIOPriority(const IoPriority priority) {

#ifdef Q_OS_LINUX
  return syscall(SYS_ioprio_set, IOPRIO_WHO_PROCESS, GetThreadId(), 4 | priority << IOPRIO_CLASS_SHIFT);
#elif defined(Q_OS_MACOS)
  return setpriority(PRIO_DARWIN_THREAD, 0, priority == IOPRIO_CLASS_IDLE ? PRIO_DARWIN_BG : 0);
#else
  Q_UNUSED(priority);
  return 0;
#endif

}

long GetThreadId() {

#ifdef Q_OS_LINUX
  return syscall(SYS_gettid);
#else
  return 0;
#endif

}

QString PathWithoutFilenameExtension(const QString &filename) {
  if (filename.section('/', -1, -1).contains('.')) return filename.section('.', 0, -2);
  return filename;
}

QString FiddleFileExtension(const QString &filename, const QString &new_extension) {
  return PathWithoutFilenameExtension(filename) + "." + new_extension;
}

QString GetEnv(const QString &key) {
  return QString::fromLocal8Bit(qgetenv(key.toLocal8Bit()));
}

void SetEnv(const char *key, const QString &value) {

#ifdef Q_OS_WIN32
  _putenv(QString("%1=%2").arg(key, value).toLocal8Bit().constData());
#else
  setenv(key, value.toLocal8Bit().constData(), 1);
#endif

}

void IncreaseFDLimit() {

#ifdef Q_OS_MACOS
  // Bump the soft limit for the number of file descriptors from the default of 256 to the maximum (usually 10240).
  struct rlimit limit;
  getrlimit(RLIMIT_NOFILE, &limit);

  // getrlimit() lies about the hard limit so we have to check sysctl.
  int max_fd = 0;
  size_t len = sizeof(max_fd);
  sysctlbyname("kern.maxfilesperproc", &max_fd, &len, nullptr, 0);

  limit.rlim_cur = max_fd;
  int ret = setrlimit(RLIMIT_NOFILE, &limit);

  if (ret == 0) {
    qLog(Debug) << "Max fd:" << max_fd;
  }
#endif

}

QString GetRandomStringWithChars(const int len) {
  const QString UseCharacters("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz");
  return GetRandomString(len, UseCharacters);
}

QString GetRandomStringWithCharsAndNumbers(const int len) {
  const QString UseCharacters("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789");
  return GetRandomString(len, UseCharacters);
}

QString CryptographicRandomString(const int len) {
  const QString UseCharacters("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~");
  return GetRandomString(len, UseCharacters);
}

QString GetRandomString(const int len, const QString &UseCharacters) {

  QString randstr;
  for (int i = 0; i < len; ++i) {
    const qint64 index = QRandomGenerator::global()->bounded(0, UseCharacters.length());
    QChar nextchar = UseCharacters.at(index);
    randstr.append(nextchar);
  }

  return randstr;

}

QString DesktopEnvironment() {

  const QString de = GetEnv("XDG_CURRENT_DESKTOP");
  if (!de.isEmpty()) return de;

  if (!qEnvironmentVariableIsEmpty("KDE_FULL_SESSION"))         return "KDE";
  if (!qEnvironmentVariableIsEmpty("GNOME_DESKTOP_SESSION_ID")) return "Gnome";

  QString session = GetEnv("DESKTOP_SESSION");
  qint64 slash = session.lastIndexOf('/');
  if (slash != -1) {
    QSettings desktop_file(QString(session + ".desktop"), QSettings::IniFormat);
    desktop_file.beginGroup("Desktop Entry");
    QString name = desktop_file.value("DesktopNames").toString();
    desktop_file.endGroup();
    if (!name.isEmpty()) return name;
    session = session.mid(slash + 1);
  }

  if (session == "kde")           return "KDE";
  else if (session == "gnome")    return "Gnome";
  else if (session == "xfce")     return "XFCE";

  return "Unknown";

}

QString UnicodeToAscii(QString unicode) {

#ifdef _MSC_VER

  return unicode
    .replace(QChar(229), "a")
    .replace(QChar(197), 'A')
    .replace(QChar(230), "ae")
    .replace(QChar(198), "AE")
    .replace(QChar(248), 'o')
    .replace(QChar(216), 'O');

#else

#ifdef LC_ALL
  setlocale(LC_ALL, "");
#endif

  iconv_t conv = iconv_open("ASCII//TRANSLIT", "UTF-8");
  if (conv == reinterpret_cast<iconv_t>(-1)) return unicode;

  QByteArray utf8 = unicode.toUtf8();

  size_t input_len = utf8.length() + 1;
  char *input_ptr = new char[input_len];
  char *input = input_ptr;

  size_t output_len = input_len * 2;
  char *output_ptr = new char[output_len];
  char *output = output_ptr;

  snprintf(input, input_len, "%s", utf8.constData());

  iconv(conv, &input, &input_len, &output, &output_len);
  iconv_close(conv);

  QString ret(output_ptr);
  ret = ret.replace('?', '_');

  delete[] input_ptr;
  delete[] output_ptr;

  return ret;

#endif // _MSC_VER

}

QString MacAddress() {

  QString ret;

  for (QNetworkInterface &netif : QNetworkInterface::allInterfaces()) {
    if (
        (netif.hardwareAddress() == "00:00:00:00:00:00") ||
        (netif.flags() & QNetworkInterface::IsLoopBack) ||
        !(netif.flags() & QNetworkInterface::IsUp) ||
        !(netif.flags() & QNetworkInterface::IsRunning)
        ) { continue; }
    if (ret.isEmpty()
        || netif.type() == QNetworkInterface::Ethernet || netif.type() == QNetworkInterface::Wifi
    ) {
      ret = netif.hardwareAddress();
    }
  }

  if (ret.isEmpty()) ret = "00:00:00:00:00:00";

  return ret;

}

QString ReplaceMessage(const QString &message, const Song &song, const QString &newline, const bool html_escaped) {

  QRegularExpression variable_replacer("[%][a-z]+[%]");
  QString copy(message);

  // Replace the first line
  qint64 pos = 0;
  QRegularExpressionMatch match;
  for (match = variable_replacer.match(message, pos); match.hasMatch(); match = variable_replacer.match(message, pos)) {
    pos = match.capturedStart();
    QStringList captured = match.capturedTexts();
    copy.replace(captured[0], ReplaceVariable(captured[0], song, newline, html_escaped));
    pos += match.capturedLength();
  }

  qint64 index_of = copy.indexOf(QRegularExpression(" - (>|$)"));
  if (index_of >= 0) copy = copy.remove(index_of, 3);

  return copy;

}

QString ReplaceVariable(const QString &variable, const Song &song, const QString &newline, const bool html_escaped) {

  QString value = variable;

  if (variable == "%title%") {
    value = song.PrettyTitle();
  }
  else if (variable == "%album%") {
    value = song.album();
  }
  else if (variable == "%artist%") {
    value = song.artist();
  }
  else if (variable == "%albumartist%") {
    value = song.effective_albumartist();
  }
  else if (variable == "%track%") {
    value.setNum(song.track());
  }
  else if (variable == "%disc%") {
    value.setNum(song.disc());
  }
  else if (variable == "%year%") {
    value = song.PrettyYear();
  }
  else if (variable == "%originalyear%") {
    value = song.PrettyOriginalYear();
  }
  else if (variable == "%genre%") {
    value = song.genre();
  }
  else if (variable == "%composer%") {
    value = song.composer();
  }
  else if (variable == "%performer%") {
    value = song.performer();
  }
  else if (variable == "%grouping%") {
    value = song.grouping();
  }
  else if (variable == "%length%") {
    value = song.PrettyLength();
  }
  else if (variable == "%filename%") {
    value = song.basefilename();
  }
  else if (variable == "%url%") {
    value = song.url().toString();
  }
  else if (variable == "%playcount%") {
    value.setNum(song.playcount());
  }
  else if (variable == "%skipcount%") {
    value.setNum(song.skipcount());
  }
  else if (variable == "%rating%") {
    value = song.PrettyRating();
  }
  else if (variable == "%newline%") {
    return QString(newline);  // No HTML escaping, return immediately.
  }

  if (html_escaped) {
    value = value.toHtmlEscaped();
  }
  return value;

}

bool IsColorDark(const QColor &color) {
  return ((30 * color.red() + 59 * color.green() + 11 * color.blue()) / 100) <= 130;
}

QByteArray ReadDataFromFile(const QString &filename) {

  QFile file(filename);
  QByteArray data;
  if (file.open(QIODevice::ReadOnly)) {
    data = file.readAll();
    file.close();
  }
  else {
    qLog(Error) << "Failed to open file" << filename << "for reading:" << file.errorString();
  }
  return data;

}

QString MimeTypeFromData(const QByteArray &data) {

  if (data.isEmpty()) return QString();

  return QMimeDatabase().mimeTypeForData(data).name();

}

#ifdef Q_OS_WIN

HRGN qt_RectToHRGN(const QRect &rc);
HRGN qt_RectToHRGN(const QRect &rc) {
  return CreateRectRgn(rc.left(), rc.top(), rc.right() + 1, rc.bottom() + 1);
}

HRGN toHRGN(const QRegion &region);
HRGN toHRGN(const QRegion &region) {

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
  return region.toHRGN();
#else

  const int rect_count = region.rectCount();
  if (rect_count == 0) {
    return nullptr;
  }

  HRGN resultRgn = nullptr;
  QRegion::const_iterator rects = region.begin();
  resultRgn = qt_RectToHRGN(rects[0]);
  for (int i = 1; i < rect_count; ++i) {
    HRGN tmpRgn = qt_RectToHRGN(rects[i]);
    const int res = CombineRgn(resultRgn, resultRgn, tmpRgn, RGN_OR);
    if (res == ERROR) qWarning("Error combining HRGNs.");
    DeleteObject(tmpRgn);
  }

  return resultRgn;

#endif  // Qt 6

}

void enableBlurBehindWindow(QWindow *window, const QRegion &region) {

  DWM_BLURBEHIND dwmbb = {0, 0, nullptr, 0};
  dwmbb.dwFlags = DWM_BB_ENABLE;
  dwmbb.fEnable = TRUE;
  HRGN rgn = nullptr;
  if (!region.isNull()) {
    rgn = toHRGN(region);
    if (rgn) {
      dwmbb.hRgnBlur = rgn;
      dwmbb.dwFlags |= DWM_BB_BLURREGION;
    }
  }
  DwmEnableBlurBehindWindow(reinterpret_cast<HWND>(window->winId()), &dwmbb);
  if (rgn) {
    DeleteObject(rgn);
  }
}

#endif  // Q_OS_WIN

}  // namespace Utilities

ScopedWCharArray::ScopedWCharArray(const QString &str)
    : chars_(str.length()), data_(new wchar_t[chars_ + 1]) {
  str.toWCharArray(data_.get());
  data_[chars_] = '\0';
}
