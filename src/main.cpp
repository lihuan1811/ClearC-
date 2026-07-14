#include "MainWindow.h"

#include <QApplication>
#include <QMessageBox>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

bool isElevated() {
#ifdef Q_OS_WIN
    BOOL elevated = FALSE;
    PSID administrators = nullptr;
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(
            &authority,
            2,
            SECURITY_BUILTIN_DOMAIN_RID,
            DOMAIN_ALIAS_RID_ADMINS,
            0, 0, 0, 0, 0, 0,
            &administrators
        )) {
        CheckTokenMembership(nullptr, administrators, &elevated);
        FreeSid(administrators);
    }
    return elevated == TRUE;
#else
    return true;
#endif
}

bool requestElevation(const QStringList& arguments) {
#ifdef Q_OS_WIN
    QStringList quoted;
    for (const QString& argument : arguments) {
        QString escaped = argument;
        escaped.replace(QLatin1Char('"'), QStringLiteral("\\\""));
        quoted.push_back(QStringLiteral("\"%1\"").arg(escaped));
    }
    quoted.push_back(QStringLiteral("--elevated-attempt"));
    const QString parameters = quoted.join(QLatin1Char(' '));
    const HINSTANCE result = ShellExecuteW(
        nullptr,
        L"runas",
        reinterpret_cast<LPCWSTR>(QCoreApplication::applicationFilePath().utf16()),
        reinterpret_cast<LPCWSTR>(parameters.utf16()),
        nullptr,
        SW_SHOWNORMAL
    );
    return reinterpret_cast<INT_PTR>(result) > 32;
#else
    Q_UNUSED(arguments);
    return false;
#endif
}

}  // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("C 盘清理大师"));
    QApplication::setOrganizationName(QStringLiteral("ClearC"));

    bool elevated = isElevated();
#ifdef Q_OS_WIN
    const QStringList arguments = app.arguments().mid(1);
    if (!elevated && !arguments.contains(QStringLiteral("--elevated-attempt")) && requestElevation(arguments)) {
        return 0;
    }
    elevated = isElevated();
#endif
    app.setProperty("isElevated", elevated);

    if (!elevated) {
        QMessageBox::warning(
            nullptr,
            QStringLiteral("管理员权限受限"),
            QStringLiteral("未获得管理员权限。扫描和只读功能仍可使用，系统优化、修复、卸载残留和目录迁移可能失败。")
        );
    }

    MainWindow window;
    window.show();
    return app.exec();
}
