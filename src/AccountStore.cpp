#include "AccountStore.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QUuid>

namespace {

const QString remoteAccountApiBaseUrl = QStringLiteral("http://47.93.103.220");

QJsonObject emptyStore() {
    QJsonObject root;
    root.insert(QStringLiteral("currentEmail"), QString());
    root.insert(QStringLiteral("deviceId"), QUuid::createUuid().toString(QUuid::WithoutBraces));
    root.insert(QStringLiteral("users"), QJsonArray());
    return root;
}

QJsonObject userForEmail(const QJsonObject& store, const QString& email) {
    const QJsonArray users = store.value(QStringLiteral("users")).toArray();
    for (const QJsonValue& value : users) {
        const QJsonObject user = value.toObject();
        if (user.value(QStringLiteral("email")).toString() == email) {
            return user;
        }
    }
    return {};
}

QJsonObject replaceUser(QJsonObject store, const QJsonObject& updatedUser) {
    QJsonArray users;
    bool replaced = false;
    const QString email = updatedUser.value(QStringLiteral("email")).toString();
    for (const QJsonValue& value : store.value(QStringLiteral("users")).toArray()) {
        const QJsonObject user = value.toObject();
        if (user.value(QStringLiteral("email")).toString() == email) {
            users.push_back(updatedUser);
            replaced = true;
        } else {
            users.push_back(user);
        }
    }
    if (!replaced) {
        users.push_back(updatedUser);
    }
    store.insert(QStringLiteral("users"), users);
    return store;
}

QJsonObject subscriptionObject(const QString& plan, int days) {
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QJsonObject subscription;
    subscription.insert(QStringLiteral("plan"), plan);
    subscription.insert(QStringLiteral("activatedAt"), now.toString(Qt::ISODate));
    subscription.insert(QStringLiteral("expiresAt"), now.addDays(days).toString(Qt::ISODate));
    return subscription;
}

AccountState stateFromUser(const QJsonObject& user, const QString& message = {}) {
    AccountState state;
    state.message = message;
    if (user.isEmpty()) {
        state.loggedIn = false;
        state.plan = QStringLiteral("Guest");
        return state;
    }

    state.loggedIn = true;
    state.email = user.value(QStringLiteral("email")).toString();
    state.displayName = user.value(QStringLiteral("displayName")).toString(state.email);
    const QJsonObject subscription = user.value(QStringLiteral("subscription")).toObject();
    state.plan = subscription.value(QStringLiteral("plan")).toString(QStringLiteral("体验卡"));
    state.expiresAt = subscription.value(QStringLiteral("expiresAt")).toString();
    return state;
}

bool cardInfo(const QString& code, QString* plan, int* days) {
    if (code == QStringLiteral("WINCLEANER-WEEK-DEMO")) {
        *plan = QStringLiteral("周卡");
        *days = 7;
        return true;
    }
    if (code == QStringLiteral("WINCLEANER-MONTH-DEMO")) {
        *plan = QStringLiteral("月卡");
        *days = 30;
        return true;
    }
    if (code == QStringLiteral("WINCLEANER-QUARTER-DEMO")) {
        *plan = QStringLiteral("季卡");
        *days = 90;
        return true;
    }
    if (code == QStringLiteral("WINCLEANER-YEAR-DEMO")) {
        *plan = QStringLiteral("年卡");
        *days = 365;
        return true;
    }
    if (code == QStringLiteral("WINCLEANER-VIP-30D")) {
        *plan = QStringLiteral("专业会员");
        *days = 30;
        return true;
    }
    if (code == QStringLiteral("WINCLEANER-VIP-90D")) {
        *plan = QStringLiteral("专业会员");
        *days = 90;
        return true;
    }
    if (code == QStringLiteral("WINCLEANER-VIP-365D")) {
        *plan = QStringLiteral("专业会员");
        *days = 365;
        return true;
    }
    return false;
}

}  // namespace

AccountStore::AccountStore() = default;

AccountState AccountStore::currentState() const {
    const QJsonDocument doc = QJsonDocument::fromJson(readText().toUtf8());
    const QJsonObject store = doc.isObject() ? doc.object() : emptyStore();
    const QString currentEmail = store.value(QStringLiteral("currentEmail")).toString();
    return stateFromUser(userForEmail(store, currentEmail));
}

AccountState AccountStore::registerUser(
    const QString& email,
    const QString& password,
    const QString& displayName
) {
    const QString normalized = normalizedEmail(email);
    bool asciiOnly = true;
    for (const QChar& ch : normalized) {
        if (ch.unicode() > 127) {
            asciiOnly = false;
            break;
        }
    }
    if (normalized.size() < 6 || !asciiOnly) {
        return stateFromUser({}, QStringLiteral("名称至少需要 6 位，且不能包含中文。"));
    }
    if (password.size() < 6) {
        return stateFromUser({}, QStringLiteral("密码至少需要 6 位。"));
    }

    QJsonObject store = QJsonDocument::fromJson(readText().toUtf8()).object();
    if (store.isEmpty()) {
        store = emptyStore();
    }
    if (store.value(QStringLiteral("deviceId")).toString().isEmpty()) {
        store.insert(QStringLiteral("deviceId"), QUuid::createUuid().toString(QUuid::WithoutBraces));
    }
    if (!userForEmail(store, normalized).isEmpty()) {
        return stateFromUser({}, QStringLiteral("该名称已被注册。"));
    }

    QJsonObject user;
    user.insert(QStringLiteral("email"), normalized);
    user.insert(QStringLiteral("displayName"), displayName.trimmed().isEmpty() ? normalized : displayName.trimmed());
    user.insert(QStringLiteral("passwordHash"), passwordHash(normalized, password));
    user.insert(QStringLiteral("createdAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    user.insert(QStringLiteral("subscription"), subscriptionObject(QStringLiteral("体验卡"), 3));
    user.insert(QStringLiteral("redeemedCodes"), QJsonArray());

    QJsonArray users = store.value(QStringLiteral("users")).toArray();
    users.push_back(user);
    store.insert(QStringLiteral("users"), users);
    store.insert(QStringLiteral("currentEmail"), normalized);
    writeText(QString::fromUtf8(QJsonDocument(store).toJson(QJsonDocument::Indented)));
    return stateFromUser(user, QStringLiteral("注册成功，已赠送体验卡。"));
}

AccountState AccountStore::login(const QString& email, const QString& password) {
    const QString normalized = normalizedEmail(email);
    QJsonObject store = QJsonDocument::fromJson(readText().toUtf8()).object();
    if (store.isEmpty()) {
        store = emptyStore();
    }
    QJsonObject user = userForEmail(store, normalized);
    if (user.isEmpty() || user.value(QStringLiteral("passwordHash")).toString() != passwordHash(normalized, password)) {
        return stateFromUser({}, QStringLiteral("名称或密码不正确。"));
    }
    store.insert(QStringLiteral("currentEmail"), normalized);
    writeText(QString::fromUtf8(QJsonDocument(store).toJson(QJsonDocument::Indented)));
    return stateFromUser(user, QStringLiteral("登录成功。"));
}

AccountState AccountStore::redeemCard(const QString& rawCode) {
    const QString code = rawCode.trimmed().toUpper();
    QString plan;
    int days = 0;
    if (!cardInfo(code, &plan, &days)) {
        AccountState state = currentState();
        state.message = QStringLiteral("卡密不存在或格式不正确。");
        return state;
    }

    QJsonObject store = QJsonDocument::fromJson(readText().toUtf8()).object();
    if (store.isEmpty()) {
        store = emptyStore();
    }
    const QString currentEmail = store.value(QStringLiteral("currentEmail")).toString();
    QJsonObject user = userForEmail(store, currentEmail);
    if (user.isEmpty()) {
        return stateFromUser({}, QStringLiteral("请先登录或注册账户，再兑换会员卡。"));
    }

    QJsonArray redeemed = user.value(QStringLiteral("redeemedCodes")).toArray();
    for (const QJsonValue& value : redeemed) {
        if (value.toString() == code) {
            return stateFromUser(user, QStringLiteral("这张会员卡已经兑换过。"));
        }
    }

    redeemed.push_back(code);
    user.insert(QStringLiteral("redeemedCodes"), redeemed);
    user.insert(QStringLiteral("subscription"), subscriptionObject(plan, days));
    store = replaceUser(store, user);
    writeText(QString::fromUtf8(QJsonDocument(store).toJson(QJsonDocument::Indented)));
    return stateFromUser(user, QStringLiteral("卡密兑换成功。"));
}

void AccountStore::logout() {
    QJsonObject store = QJsonDocument::fromJson(readText().toUtf8()).object();
    if (store.isEmpty()) {
        store = emptyStore();
    }
    store.insert(QStringLiteral("currentEmail"), QString());
    writeText(QString::fromUtf8(QJsonDocument(store).toJson(QJsonDocument::Indented)));
}

QString AccountStore::storePath() const {
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base.isEmpty()) {
        base = QDir::homePath() + QStringLiteral("/.wincleaner");
    }
    return QDir(base).filePath(QStringLiteral("account.json"));
}

QString AccountStore::normalizedEmail(const QString& email) const {
    return email.trimmed().toLower();
}

QString AccountStore::passwordHash(const QString& email, const QString& password) const {
    const QByteArray input = QStringLiteral("%1::%2").arg(email, password).toUtf8();
    return QString::fromLatin1(QCryptographicHash::hash(input, QCryptographicHash::Sha256).toHex());
}

QString AccountStore::readText() const {
    QFile file(storePath());
    if (!file.exists() || !file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QString::fromUtf8(QJsonDocument(emptyStore()).toJson(QJsonDocument::Compact));
    }
    return QString::fromUtf8(file.readAll());
}

void AccountStore::writeText(const QString& text) const {
    QFileInfo info(storePath());
    QDir().mkpath(info.absolutePath());
    QFile file(storePath());
    if (file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        file.write(text.toUtf8());
    }
}
