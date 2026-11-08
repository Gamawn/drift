#include <QDir>
#include <QFile>
#include <QLocalServer>
#include <QLocalSocket>
#include <QNetworkProxy>
#include <QScopeGuard>
#include <QSet>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

#include "backend.h"
#include "theme.h"
#include "translator.h"

class DriftTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        // Backend reads QSettings; keep the tests away from the real config.
        QStandardPaths::setTestModeEnabled(true);
    }

    void cleanup()
    {
        QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
    }

    void parsesPlaylistEntries()
    {
        const auto tracks = Backend::parseEntries(R"json({"entries": [
            {"id": "a1", "title": "Song One", "channel": "Band - Topic", "duration": 200},
            {"id": "b2", "title": "[Private video]", "channel": null},
            {"id": "c3", "title": "Other - Song Two (Official Video)", "uploader": "Other"}
        ]})json");
        QCOMPARE(tracks.size(), 2);
        QCOMPARE(tracks[0].artist, QString("Band"));
        QVERIFY(tracks[0].fromPlaylist);
        QCOMPARE(tracks[0].duration, 200.0);
        QCOMPARE(tracks[1].artist, QString("Other"));
    }

    void parsesYouTubeMusicRadio()
    {
        const auto tracks = Backend::parseRadio(R"json({"contents": {"tabs": [{"items": [
            {"playlistPanelVideoRenderer": {"videoId": "pRpeEdMmmQ0",
                "title": {"runs": [{"text": "Waka Waka"}]},
                "longBylineText": {"runs": [{"text": "Shakira"}, {"text": " • 4.8B views"}]},
                "lengthText": {"runs": [{"text": "3:31"}]}}},
            {"automixPreviewVideoRenderer": {}}
        ]}]}})json");
        QCOMPARE(tracks.size(), 1);
        QCOMPARE(tracks[0].id, QString("pRpeEdMmmQ0"));
        QCOMPARE(tracks[0].title, QString("Waka Waka"));
        QCOMPARE(tracks[0].artist, QString("Shakira"));
        QCOMPARE(tracks[0].duration, 211.0);
    }

    void parsesYouTubeMusicSearch()
    {
        const auto tracks = Backend::parseSearch(R"json({"contents": [
            {"musicResponsiveListItemRenderer": {"playlistItemData": {"videoId": "xtxjm7ciwmc"}, "flexColumns": [
                {"musicResponsiveListItemFlexColumnRenderer": {"text": {"runs": [{"text": "Группа крови"}]}}},
                {"musicResponsiveListItemFlexColumnRenderer": {"text": {"runs": [{"text": "Кино"}, {"text": " • "}, {"text": "Группа крови"}, {"text": " • "}, {"text": "4:45"}]}}}
            ]}},
            {"musicResponsiveListItemRenderer": {"flexColumns": []}}
        ]})json");
        QCOMPARE(tracks.size(), 1);
        QCOMPARE(tracks[0].id, QString("xtxjm7ciwmc"));
        QCOMPARE(tracks[0].title, QString("Группа крови"));
        QCOMPARE(tracks[0].artist, QString("Кино"));
        QCOMPARE(tracks[0].duration, 285.0);
        QVERIFY(!tracks[0].fromPlaylist);
    }

    void tellsLinksFromSearches()
    {
        QVERIFY(Backend::isLink(" https://music.youtube.com/playlist?list=PLx "));
        QVERIFY(Backend::isLink("music.youtube.com/playlist?list=PLx"));
        QVERIFY(!Backend::isLink("кино группа крови"));
    }

    void songKeyIgnoresUploadVariants()
    {
        Track video{"x", "Rick Astley - Never Gonna Give You Up (Official Video) [4K]", "Rick Astley"};
        Track audio{"y", "Never Gonna Give You Up", "Rick Astley"};
        QCOMPARE(Backend::songKey(video), Backend::songKey(audio));
        QCOMPARE(Backend::songKey(Track{"z", "(Intro)", ""}), QString("z"));
    }

    void radioSkipsSeedAndRepeatsAndCaps()
    {
        Backend backend("/nonexistent/yt-dlp", "/nonexistent/mpv");
        const Track seed{"s", "Seed Song", "A"};
        QList<Track> mix{seed, {"d1", "Dup", "B"}, {"d2", "B - Dup (Lyrics)", "B"}};
        for (int i = 0; i < 10; ++i)
            mix.append(Track{QString("m%1").arg(i), QString("Mix %1").arg(i), "C"});
        backend.addRadio(seed, mix);
        const auto queue = backend.queue();
        QCOMPARE(queue.size(), 5);
        QCOMPARE(queue[0].id, QString("d1"));
        QCOMPARE(queue[1].id, QString("m0"));
        backend.addRadio(seed, mix);
        QCOMPARE(backend.queue().size(), 10);
        QCOMPARE(backend.queue()[5].id, QString("m4"));
    }

    void seedsCycleThroughWholePlaylist()
    {
        Backend backend("/nonexistent/yt-dlp", "/nonexistent/mpv");
        QList<Track> seeds;
        for (int i = 0; i < 6; ++i)
            seeds.append(Track{QString::number(i), QString("Song %1").arg(i), "A"});
        backend.setSeeds(seeds);
        QSet<QString> firstPass, secondPass;
        for (int i = 0; i < 6; ++i) {
            const Track seed = backend.nextSeed();
            firstPass.insert(seed.id);
        }
        for (int i = 0; i < 6; ++i)
            secondPass.insert(backend.nextSeed().id);
        QCOMPARE(firstPass.size(), 6);
        QCOMPARE(secondPass, firstPass);
    }

    void missingYtDlpIsReported()
    {
        Backend backend("/nonexistent/yt-dlp", "/nonexistent/mpv");
        backend.start("https://music.youtube.com/playlist?list=PLx");
        QTRY_VERIFY(backend.error().contains("yt-dlp"));
        QVERIFY(backend.status().isEmpty());
    }

    void failedStreamIsResolvedOnce()
    {
        QTemporaryDir dir;
        const QString log = dir.filePath("calls");
        const QString ytdlp = script(dir.filePath("yt-dlp"), "#!/bin/sh\necho >> '" + log.toUtf8() + "'\nexit 1\n");
        Backend backend(ytdlp, "/nonexistent/mpv");
        backend.addRadio({"s", "Seed", "A"}, {{"blocked", "Blocked Song", "B"}});
        QTest::qWait(500);
        QCOMPARE(lines(log), 1);
    }

    void stopsAfterTracksFailInARow()
    {
        if (QStandardPaths::findExecutable("python3").isEmpty())
            QSKIP("needs python3 for the fake mpv");
        QTemporaryDir dir;
        const QString log = dir.filePath("loads");
        // Answers every loadfile with the end-file error a broken yt-dlp gives.
        const QString mpv = script(dir.filePath("mpv"), "#!/usr/bin/env python3\n"
            "import json, socket, sys\n"
            "path = [a.split('=', 1)[1] for a in sys.argv if a.startswith('--input-ipc-server=')][0]\n"
            "server = socket.socket(socket.AF_UNIX); server.bind(path); server.listen(1)\n"
            "conn, _ = server.accept()\n"
            "for line in conn.makefile('rb'):\n"
            "    if json.loads(line).get('command', [''])[0] == 'loadfile':\n"
            "        open('" + log.toUtf8() + "', 'a').write('load\\n')\n"
            "        conn.sendall(b'{\"event\":\"end-file\",\"reason\":\"error\"}\\n')\n");
        QTcpServer proxy;
        int requests = 0;
        failNetwork(proxy, requests);

        Backend backend("/nonexistent/yt-dlp", mpv);
        const Track seed{"s", "Seed", "A"};
        QList<Track> mix;
        for (int i = 0; i < 5; ++i)
            mix.append(Track{QString("m%1").arg(i), QString("Mix %1").arg(i), "C"});
        backend.setSeeds({seed});
        backend.addRadio(seed, mix);
        backend.togglePause();
        QTRY_VERIFY(backend.error().contains("in a row"));
        QTest::qWait(300);
        QCOMPARE(lines(log), 3);
        QVERIFY(backend.current().isEmpty());
    }

    void mixLookupWaitsAfterFailure()
    {
        QTemporaryDir dir;
        const QString ytdlp = script(dir.filePath("yt-dlp"),
            "#!/bin/sh\necho '{\"entries\": [{\"id\": \"a1\", \"title\": \"Song One\", \"channel\": \"A\"}]}'\n");
        QTcpServer proxy;
        int requests = 0;
        failNetwork(proxy, requests);

        Backend backend(ytdlp, "/nonexistent/mpv");
        backend.start("https://music.youtube.com/playlist?list=PLx");
        QTRY_COMPARE(requests, 1);
        // Offline, an instant retry would fail at once and give up within a second.
        QTest::qWait(1000);
        QCOMPARE(requests, 1);
        QVERIFY(!backend.error().contains("YouTube Music"));
    }

    void smallPlaylistRadioStartsOverWhenMixesRunDry()
    {
        Backend backend("/nonexistent/yt-dlp", "/nonexistent/mpv");
        const Track seed{"s", "Seed", "A"};
        QList<Track> mix;
        for (int i = 0; i < 5; ++i)
            mix.append(Track{QString("m%1").arg(i), QString("Mix %1").arg(i), "C"});
        backend.setSeeds({seed});
        backend.addRadio(seed, mix);
        backend.addRadio(seed, mix);
        QCOMPARE(backend.queue().size(), 5);
        backend.addRadio(seed, mix);
        QCOMPARE(backend.queue().size(), 10);
    }

    void themeFallsBack()
    {
        QTemporaryDir dir;
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#FFD60A"));
        QCOMPARE(theme.accentForeground(), QString("black"));
    }

    void themeReadsAccent()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        QFile colors(dir.path() + "/theme/colors.toml");
        QVERIFY(colors.open(QIODevice::WriteOnly));
        colors.write("accent = \"#112233\"\n");
        colors.close();

        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#112233"));
        QCOMPARE(theme.accentForeground(), QString("white"));
    }

    void themeFollowsHyprlandRounding()
    {
        QTemporaryDir dir, themeDir;
        const QString response = dir.filePath("response.json"), command = dir.filePath("hyprctl");
        auto write = [](const QString &path, const QByteArray &data) {
            QFile file(path);
            return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
        };
        QVERIFY(write(command, "#!/bin/sh\ncat '" + response.toUtf8() + "'\n"));
        QVERIFY(QFile::setPermissions(command, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QVERIFY(write(response, "{\"int\":0}"));

        // A fake Hyprland event socket where Theme looks for it.
        const QByteArray runtime = qgetenv("XDG_RUNTIME_DIR"), signature = qgetenv("HYPRLAND_INSTANCE_SIGNATURE");
        const auto restore = qScopeGuard([&] {
            qputenv("XDG_RUNTIME_DIR", runtime);
            qputenv("HYPRLAND_INSTANCE_SIGNATURE", signature);
        });
        qputenv("XDG_RUNTIME_DIR", dir.path().toUtf8());
        qputenv("HYPRLAND_INSTANCE_SIGNATURE", "test");
        QVERIFY(QDir(dir.path()).mkpath("hypr/test"));
        QLocalServer hyprland;
        QVERIFY(hyprland.listen(dir.filePath("hypr/test/.socket2.sock")));

        Theme theme(themeDir.path(), nullptr, command);
        QTRY_VERIFY(hyprland.hasPendingConnections());
        QLocalSocket *events = hyprland.nextPendingConnection();
        QTest::qWait(300); // the startup query finishes
        QCOMPARE(theme.radius(), 0);
        QVERIFY(write(response, "{\"int\":6}"));
        events->write("activewindow>>kitty,shell\n");
        QTest::qWait(300);
        QCOMPARE(theme.radius(), 0);
        events->write("configreloaded>>\n");
        QTRY_COMPARE(theme.radius(), 6);
    }

    void translatorFallsBackToEnglish()
    {
        QTemporaryDir dir;
        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("en"));
        QVERIFY(translator.translate("Main", "Save").isNull());
    }

    void translatorFollowsTheLanguageFile()
    {
        QTemporaryDir dir;
        write(dir.path() + "/language", "pt-BR\n");
        write(dir.path() + "/pt.json", R"({"Save": "Guardar", "Open": "Abrir"})");
        write(dir.path() + "/pt-BR.json", R"({"Save": "Salvar"})");

        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("pt-BR"));
        QCOMPARE(translator.translate("Main", "Save"), QString("Salvar"));
        QCOMPARE(translator.translate("Main", "Open"), QString("Abrir"));
        QVERIFY(translator.translate("Main", "Quit").isNull());
    }

    void translatorPicksPluralForms()
    {
        QTemporaryDir dir;
        write(dir.path() + "/language", "ru\n");
        write(dir.path() + "/en.json", R"({"%n clips": {"one": "%n clip", "other": "%n clips"}})");
        write(dir.path() + "/ru.json", R"({"%n clips": {"one": "%n клип", "few": "%n клипа", "many": "%n клипов", "other": "%n клипа"}})");

        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 21), QString("%n клип"));
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 22), QString("%n клипа"));
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 25), QString("%n клипов"));
        QCOMPARE(Translator::pluralCategory("ar", 11), QString("many"));
        QCOMPARE(Translator::pluralCategory("ja", 1), QString("other"));
    }

private:
    static QString script(const QString &path, const QByteArray &contents)
    {
        write(path, contents);
        QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        return path;
    }

    static int lines(const QString &path)
    {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? int(file.readAll().count('\n')) : 0;
    }

    // Every request goes through a local proxy that refuses it, so network calls fail fast and
    // are counted. A refusal, not a hang-up: Qt retries a dropped connection by itself.
    static void failNetwork(QTcpServer &proxy, int &requests)
    {
        QVERIFY(proxy.listen(QHostAddress::LocalHost));
        QObject::connect(&proxy, &QTcpServer::newConnection, &proxy, [&proxy, &requests] {
            while (QTcpSocket *socket = proxy.nextPendingConnection()) {
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket, &requests] {
                    ++requests;
                    socket->readAll();
                    socket->write("HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                    socket->disconnectFromHost();
                });
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
        QNetworkProxy::setApplicationProxy({QNetworkProxy::HttpProxy, "127.0.0.1", proxy.serverPort()});
    }

    static void write(const QString &path, const QByteArray &contents)
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(contents);
    }
};

QTEST_MAIN(DriftTests)
#include "drift_tests.moc"
