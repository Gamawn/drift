#pragma once

#include <QObject>
#include <memory>

class Backend;

// Windows' System Media Transport Controls: media keys and the media flyout
// play, pause and skip Drift and show its track, without switching to it.
// Only this file's .cpp sees C++/WinRT, which clashes with moc and Qt's keywords.
class Smtc : public QObject {
public:
    explicit Smtc(Backend *backend, QObject *parent = nullptr);
    ~Smtc() override;

private:
    struct Private;
    std::unique_ptr<Private> d;
};
