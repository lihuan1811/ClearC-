#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

struct DismRuleEntry {
    QString path;
    QString ruleName;
    QString group;
    qint64 size = 0;
};

class DismRuleScanner {
public:
    explicit DismRuleScanner(const QString& rulesPath = {});

    QVector<DismRuleEntry> scan() const;

    static QString defaultRulesPath();

private:
    QString rulesPath_;

    QString expandEnvironmentPath(const QString& path) const;
    QString normalizeQuery(const QString& query) const;
    bool isDynamicExpression(const QString& value) const;
    bool isExcluded(const QString& rootPath, const QString& path, const QStringList& excluded) const;
    QStringList scanGeneral(const QString& rootPath, const QStringList& queries, const QStringList& excluded) const;
    qint64 pathSize(const QString& path) const;
};
