#include "DismRuleScanner.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QXmlStreamReader>

namespace {

QString cleanLabel(const QString& value) {
    QString cleaned = value.trimmed();
    while (cleaned.startsWith(QLatin1Char('#'))) {
        cleaned.remove(0, 1);
    }
    return cleaned.trimmed();
}

QString normKey(const QString& path) {
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath()).toLower();
}

}  // namespace

DismRuleScanner::DismRuleScanner(const QString& rulesPath)
    : rulesPath_(rulesPath.isEmpty() ? defaultRulesPath() : rulesPath) {}

QVector<DismRuleEntry> DismRuleScanner::scan() const {
    QVector<DismRuleEntry> entries;
    QFile file(rulesPath_);
    if (!file.exists() || !file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return entries;
    }

    QXmlStreamReader xml(&file);
    QSet<QString> seen;

    QString currentRuleName;
    QString currentGroup;
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement()) {
            continue;
        }

        if (xml.name() == QLatin1String("Item")) {
            currentRuleName = cleanLabel(xml.attributes().value(QStringLiteral("Name")).toString());
            currentGroup.clear();
            continue;
        }

        if (xml.name() == QLatin1String("Group")) {
            currentGroup = cleanLabel(xml.readElementText());
            continue;
        }

        if (xml.name() != QLatin1String("General")) {
            continue;
        }

        const QString rootPath = expandEnvironmentPath(xml.attributes().value(QStringLiteral("RootPath")).toString().trimmed());
        QStringList queries;
        QStringList excluded;

        while (!xml.atEnd()) {
            xml.readNext();
            if (xml.isEndElement() && xml.name() == QLatin1String("General")) {
                break;
            }
            if (!xml.isStartElement()) {
                continue;
            }
            if (xml.name() == QLatin1String("Query")) {
                const QString query = normalizeQuery(xml.readElementText());
                if (!query.isEmpty() && !isDynamicExpression(query)) {
                    queries.push_back(query);
                }
            } else if (xml.name() == QLatin1String("Excluded")) {
                const QString pattern = normalizeQuery(xml.readElementText());
                if (!pattern.isEmpty() && !isDynamicExpression(pattern)) {
                    excluded.push_back(pattern);
                }
            }
        }

        if (rootPath.isEmpty() || isDynamicExpression(rootPath)) {
            continue;
        }

        for (const QString& path : scanGeneral(rootPath, queries, excluded)) {
            const QString key = normKey(path);
            if (seen.contains(key)) {
                continue;
            }
            seen.insert(key);

            DismRuleEntry entry;
            entry.path = QDir::toNativeSeparators(path);
            entry.ruleName = currentRuleName.isEmpty() ? QStringLiteral("Dism++规则") : currentRuleName;
            entry.group = currentGroup;
            entry.size = pathSize(path);
            if (entry.size > 0) {
                entries.push_back(entry);
            }
        }
    }

    return entries;
}

QString DismRuleScanner::defaultRulesPath() {
    const QString appPath = QCoreApplication::applicationDirPath();
    const QString packaged = QDir(appPath).filePath(QStringLiteral("rules/dismpp/Data.xml"));
    if (QFileInfo::exists(packaged)) {
        return packaged;
    }

    const QString source = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("../rules/dismpp/Data.xml"));
    if (QFileInfo::exists(source)) {
        return source;
    }
    return QDir::current().filePath(QStringLiteral("rules/dismpp/Data.xml"));
}

QString DismRuleScanner::expandEnvironmentPath(const QString& path) const {
    if (path.isEmpty() || isDynamicExpression(path)) {
        return {};
    }

    QString expanded = path;
    const QRegularExpression envPattern(QStringLiteral("%([^%]+)%"));
    QRegularExpressionMatchIterator matches = envPattern.globalMatch(path);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        const QString name = match.captured(1);
        QString value = qEnvironmentVariable(name.toLocal8Bit().constData());
        if (value.isEmpty()) {
            value = qEnvironmentVariable(name.toUpper().toLocal8Bit().constData());
        }
        if (value.isEmpty() && name.compare(QStringLiteral("SystemDrive"), Qt::CaseInsensitive) == 0) {
            value = QStringLiteral("C:");
        }
        if (value.isEmpty() && (name.compare(QStringLiteral("SystemRoot"), Qt::CaseInsensitive) == 0
                || name.compare(QStringLiteral("WinDir"), Qt::CaseInsensitive) == 0)) {
            value = QStringLiteral("C:\\Windows");
        }
        if (value.isEmpty()) {
            return {};
        }
        expanded.replace(match.captured(0), value);
    }

    if (expanded.contains(QLatin1Char('%')) || isDynamicExpression(expanded)) {
        return {};
    }
    return QDir::cleanPath(QDir::fromNativeSeparators(expanded));
}

QString DismRuleScanner::normalizeQuery(const QString& query) const {
    const QString trimmed = query.trimmed();
    return trimmed == QStringLiteral("{*}") ? QStringLiteral("*") : trimmed;
}

bool DismRuleScanner::isDynamicExpression(const QString& value) const {
    const QString trimmed = value.trimmed();
    return trimmed.startsWith(QLatin1Char('?')) || trimmed.contains(QStringLiteral("?Get"));
}

bool DismRuleScanner::isExcluded(const QString& rootPath, const QString& path, const QStringList& excluded) const {
    if (excluded.isEmpty()) {
        return false;
    }

    const QString fileName = QFileInfo(path).fileName();
    const QString relative = QDir(rootPath).relativeFilePath(path);
    for (const QString& pattern : excluded) {
        if (QDir::match(pattern, fileName) || QDir::match(pattern, relative)) {
            return true;
        }
    }
    return false;
}

QStringList DismRuleScanner::scanGeneral(const QString& rootPath, const QStringList& queries, const QStringList& excluded) const {
    QStringList matches;
    const QFileInfo root(rootPath);
    if (!root.exists()) {
        return matches;
    }

    if (queries.isEmpty()) {
        if (!isExcluded(rootPath, rootPath, excluded)) {
            matches.push_back(rootPath);
        }
        return matches;
    }

    for (const QString& query : queries) {
        const QString nativeQuery = QDir::fromNativeSeparators(query);
        if (!nativeQuery.contains(QLatin1Char('*')) && !nativeQuery.contains(QLatin1Char('?'))) {
            const QString candidate = QDir(rootPath).filePath(nativeQuery);
            if (QFileInfo::exists(candidate) && !isExcluded(rootPath, candidate, excluded)) {
                matches.push_back(candidate);
            }
            continue;
        }

        const QString pattern = QFileInfo(nativeQuery).fileName();
        const QString subdir = QFileInfo(nativeQuery).path() == QStringLiteral(".")
            ? rootPath
            : QDir(rootPath).filePath(QFileInfo(nativeQuery).path());
        QDirIterator iterator(subdir, QStringList{pattern}, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (iterator.hasNext()) {
            const QString path = iterator.next();
            if (!isExcluded(rootPath, path, excluded)) {
                matches.push_back(path);
            }
        }
    }

    return matches;
}

qint64 DismRuleScanner::pathSize(const QString& path) const {
    const QFileInfo info(path);
    if (!info.exists()) {
        return 0;
    }
    if (info.isFile()) {
        return info.size();
    }

    qint64 total = 0;
    QDirIterator iterator(path, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        iterator.next();
        total += iterator.fileInfo().size();
    }
    return total;
}
