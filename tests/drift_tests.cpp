#include <QDir>
#include <QFile>
#include <QJsonArray>
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
#include "discord.h"
#include "room.h"
#ifdef Q_OS_LINUX
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include "mpris.h"
#endif
#include "theme.h"
#include "translator.h"
#include "updater.h"

class DriftTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        // Backend reads QSettings; keep the tests away from the real config.
        QStandardPaths::setTestModeEnabled(true);
    }

    void init()
    {
        // The last track and the disliked ones are settings; one test's mustn't reach the next.
        QSettings().clear();
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

    void readsADriftLink()
    {
        QCOMPARE(Backend::linkId("drift://play/pRpeEdMmmQ0"), QString("pRpeEdMmmQ0"));
        QCOMPARE(Backend::linkId("drift://play/pRpeEdMmmQ0/\n"), QString("pRpeEdMmmQ0"));
        QCOMPARE(Backend::linkId("drift://play/a_b-c_d-e_f"), QString("a_b-c_d-e_f"));
        // Anything else, a longer id or one with an option's dashes and spaces included, is no link.
        QCOMPARE(Backend::linkId(""), QString());
        QCOMPARE(Backend::linkId("--from-setup"), QString());
        QCOMPARE(Backend::linkId("https://music.youtube.com/watch?v=pRpeEdMmmQ0"), QString());
        QCOMPARE(Backend::linkId("drift://play/pRpeEdMmmQ0x"), QString());
        QCOMPARE(Backend::linkId("drift://play/short"), QString());
        QCOMPARE(Backend::linkId("drift://play/--exec rm -"), QString());
    }

    void discordFramesCarryOpcodeAndLength()
    {
        const QByteArray frame = Discord::frame(1, {{"cmd", "SET_ACTIVITY"}});
        const QByteArray json = R"({"cmd":"SET_ACTIVITY"})";
        QCOMPARE(frame.left(4), QByteArray("\x01\x00\x00\x00", 4));
        QCOMPARE(frame.mid(4, 4), QByteArray("\x16\x00\x00\x00", 4));
        QCOMPARE(frame.mid(8), json);
    }

    void discordActivityShowsTheTrackAndItsProgress()
    {
        const QVariantMap track{{"id", "pRpeEdMmmQ0"}, {"title", "Waka Waka"}, {"artist", "Shakira"}};
        const QJsonObject activity = Discord::activity(track, 30, 211, 1000000, "abc");
        QCOMPARE(activity.value("type").toInt(), 2);
        QCOMPARE(activity.value("details").toString(), QString("Waka Waka"));
        QCOMPARE(activity.value("state").toString(), QString("Shakira"));
        const QJsonObject times = activity.value("timestamps").toObject();
        QCOMPARE(times.value("start").toInteger(), 970000);
        QCOMPARE(times.value("end").toInteger(), 1181000);
        QVERIFY(activity.value("assets").toObject().value("large_image").toString().contains("pRpeEdMmmQ0"));
        // The first button's page turns into a drift://join link the viewer's own Drift opens.
        const QJsonArray buttons = activity.value("buttons").toArray();
        QCOMPARE(buttons.size(), 2);
        QCOMPARE(buttons.at(0).toObject().value("url").toString(), QString("https://gamawn.github.io/drift/?v=pRpeEdMmmQ0&room=abc"));
        QCOMPARE(buttons.at(1).toObject().value("url").toString(), QString("https://music.youtube.com/watch?v=pRpeEdMmmQ0"));

        // Discord refuses text under 2 or over 128 characters, and an empty state.
        const QJsonObject odd = Discord::activity({{"id", "x"}, {"title", "A"}, {"artist", ""}}, 0, 0, 0, {});
        QCOMPARE(odd.value("details").toString().size(), 2);
        QVERIFY(!odd.contains("state"));
        QVERIFY(!odd.contains("timestamps"));
        const QJsonObject wordy = Discord::activity({{"id", "x"}, {"title", QString(300, 'a')}}, 0, 0, 0, {});
        QCOMPARE(wordy.value("details").toString().size(), 128);
    }

    void roomSaysWhatTheHostPlays()
    {
        const QString room(32, 'a');
        QCOMPARE(Room::linkRoom("drift://join/" + room + "/\n"), room);
        QCOMPARE(Room::linkRoom("drift://join/" + room + "0"), QString());
        QCOMPARE(Room::linkRoom("drift://join/../../etc"), QString());
        QCOMPARE(Room::linkRoom("drift://play/pRpeEdMmmQ0"), QString());

        // As ntfy hands it on: the host's message is the "message" of a line of the stream.
        auto line = [](const QByteArray &said) {
            return QJsonDocument(QJsonObject{{"event", "message"}, {"message", QString::fromUtf8(said)}}).toJson(QJsonDocument::Compact);
        };
        const QVariantMap track{{"id", "pRpeEdMmmQ0"}, {"title", "Waka Waka"}, {"artist", "Shakira"}, {"duration", 211.0}};
        // Heard 5 seconds after it was said, the host is 5 seconds further on; a paused one isn't.
        const Room::State state = Room::parse(line(Room::message(track, 30, true, 1000000)), 1005000);
        QCOMPARE(state.track.id, QString("pRpeEdMmmQ0"));
        QCOMPARE(state.track.title, QString("Waka Waka"));
        QCOMPARE(state.position, 35);
        QVERIFY(state.playing);
        QCOMPARE(Room::parse(line(Room::message(track, 30, false, 1000000)), 1005000).position, 30);

        // Not a track: a keepalive, a host long gone, and an id that is no video's.
        QVERIFY(Room::parse(R"({"event":"keepalive"})", 0).track.id.isEmpty());
        QVERIFY(Room::parse(line(Room::message(track, 30, true, 1000000)), 1000000 + 3600000).track.id.isEmpty());
        QVERIFY(Room::parse(line(Room::message({{"id", "--exec rm -"}}, 0, true, 0)), 0).track.id.isEmpty());
        QVERIFY(Room::parse("nonsense", 0).track.id.isEmpty());
    }

    void followerPlaysTheHostsTrackAndCannotChangeIt()
    {
        Backend backend("/nonexistent/yt-dlp", "/nonexistent/mpv");
        backend.setSeeds({Track{"own", "Own", "Me"}});
        backend.follow(Track{"early", "Early", "Host"}, 0, true); // not in a room: ignored
        QVERIFY(backend.current().isEmpty());

        backend.setFollowing(true);
        backend.follow(Track{"one", "One", "Host"}, 40, true);
        QCOMPARE(backend.current().value("id").toString(), QString("one"));
        QVERIFY(backend.queue().isEmpty());
        backend.next();
        backend.previous();
        backend.dislike();
        QCOMPARE(backend.current().value("id").toString(), QString("one"));

        backend.follow(Track{"two", "Two", "Host"}, 0, true);
        QCOMPARE(backend.current().value("id").toString(), QString("two"));

        // Leaving keeps the track, and a radio of its own is on the way after it.
        backend.unfollow();
        QVERIFY(!backend.following());
        QCOMPARE(backend.current().value("id").toString(), QString("two"));
        backend.addRadio(Track{"two", "Two", "Host"}, {Track{"two", "Two", "Host"}, Track{"three", "Three", "Other"}});
        backend.next();
        QCOMPARE(backend.current().value("id").toString(), QString("three"));
    }

    void updaterReadsAGitHubRelease()
    {
        // @ for the site: moc takes the two slashes of a URL in a raw string for a comment.
        const auto release = Updater::parseRelease(QByteArray(R"json({"tag_name": "v0.4.0",
            "html_url": "@/tag/v0.4.0", "assets": [
            {"name": "Drift-setup-x64.exe", "digest": "sha256:HASH", "browser_download_url": "@/download/v0.4.0/Drift-setup-x64.exe"},
            {"name": "Drift-windows-x64.zip", "digest": "sha256:00", "browser_download_url": "@/download/v0.4.0/Drift-windows-x64.zip"}
        ]})json").replace("@", "https://github.com/Gamawn/drift/releases").replace("HASH", QByteArray(64, 'A')));
        QCOMPARE(release.version, QString("0.4.0"));
        QVERIFY(release.page.toString().endsWith("/tag/v0.4.0"));
        QVERIFY(release.installer.toString().endsWith("/v0.4.0/Drift-setup-x64.exe"));
        QCOMPARE(release.sha256, QByteArray(64, 'a'));

        // Nothing is downloaded from outside Drift's own releases, and a tag never becomes a path.
        const auto odd = Updater::parseRelease(QByteArray(R"json({"tag_name": "v1.0/../evil", "html_url": "@/", "assets": [
            {"name": "Drift-setup-x64.exe", "digest": "sha256:HASH", "browser_download_url": "@/Drift-setup-x64.exe"}
        ]})json").replace("@", "https://example.com").replace("HASH", QByteArray(64, 'a')));
        QCOMPARE(odd.version, QString("1.0"));
        QVERIFY(odd.installer.isEmpty());
        QCOMPARE(odd.page, QUrl("https://github.com/Gamawn/drift/releases/latest"));
        QVERIFY(Updater::parseRelease("rate limited").version.isEmpty());

        // Without GitHub's hash of it the installer isn't taken.
        const auto unhashed = Updater::parseRelease(QByteArray(R"json({"tag_name": "v0.4.0", "assets": [
            {"name": "Drift-setup-x64.exe", "browser_download_url": "@/download/v0.4.0/Drift-setup-x64.exe"}
        ]})json").replace("@", "https://github.com/Gamawn/drift/releases"));
        QVERIFY(unhashed.installer.isEmpty());
    }

    void updaterComparesVersions()
    {
        QVERIFY(Updater::isNewer("0.4.0", "0.3.0"));
        QVERIFY(Updater::isNewer("0.10.0", "0.9.1"));
        QVERIFY(Updater::isNewer("1.0", "0.9.9"));
        QVERIFY(!Updater::isNewer("0.3.0", "0.3.0"));
        QVERIFY(!Updater::isNewer("0.3", "0.3.0"));
        QVERIFY(!Updater::isNewer("0.2.9", "0.3.0"));
        QVERIFY(!Updater::isNewer("", "0.3.0"));
        // A release candidate moves on to its release, and not to itself.
        QVERIFY(Updater::isNewer("0.6.0", "0.6.0-dev"));
        QVERIFY(!Updater::isNewer("0.6.0", "0.6.1-dev"));
        QVERIFY(!Updater::isNewer("0.5.0", "0.6.0-dev"));
    }

    void updaterForgetsAnInstalledUpdate()
    {
        // In a file of its own: on Windows QSettings is the registry, which test mode leaves alone,
        // and it keeps nothing at all for an application without an organization.
        QTemporaryDir dir;
        const auto restore = qScopeGuard([] {
            QSettings::setDefaultFormat(QSettings::NativeFormat);
            QCoreApplication::setOrganizationName(QString());
            QCoreApplication::setApplicationVersion(QString());
        });
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
        QCoreApplication::setOrganizationName("drift-tests");
        QCoreApplication::setApplicationVersion("0.3.0");

        const QString path = dir.filePath("Drift-setup-0.4.0.exe");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        QSettings settings;
        QVERIFY(settings.fileName().startsWith(dir.path()));
        settings.setValue("update/version", "0.4.0");
        settings.setValue("update/installer", path);
        settings.setValue("update/attempted", "0.4.0");
        settings.sync();

        // Tried once already: kept for the button, not run again at launch.
        Updater updater;
        QVERIFY(!updater.installPending());
        QVERIFY(QFile::exists(path));
        QCOMPARE(QSettings().value("update/version").toString(), QString("0.4.0"));

        QCoreApplication::setApplicationVersion("0.4.0");
        QVERIFY(!updater.installPending());
        QVERIFY(!QFile::exists(path));
        QVERIFY(!QSettings().contains("update/version"));
        QVERIFY(!QSettings().contains("update/attempted"));
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

    void playsQueuedTrackAndRebuildsUpNextFromIt()
    {
        QTcpServer proxy;
        int requests = 0;
        failNetwork(proxy, requests);
        Backend backend("/nonexistent/yt-dlp", "/nonexistent/mpv");
        const Track seed{"s", "Seed", "A"};
        backend.setSeeds({seed});
        backend.addRadio(seed, {{"a", "A", "X"}, {"b", "B", "X"}, {"c", "C", "X"}});
        backend.playQueued(1);
        QCOMPARE(backend.current().value("id").toString(), QString("b"));
        QVERIFY(backend.queue().isEmpty());
        QTRY_COMPARE(requests, 1); // the mix for "b"
        // Dropped tracks may come back from another mix; the one playing doesn't.
        backend.addRadio(seed, {{"a", "A", "X"}, {"b", "B", "X"}});
        QCOMPARE(backend.queue().size(), 1);
        QCOMPARE(backend.queue().first().id, QString("a"));
        backend.playQueued(99);
        QCOMPARE(backend.current().value("id").toString(), QString("b"));
    }

    void previousGoesBackAndNextReturns()
    {
        QTcpServer proxy;
        int requests = 0;
        failNetwork(proxy, requests);
        Backend backend("/nonexistent/yt-dlp", "/nonexistent/mpv");
        const Track seed{"s", "Seed", "A"};
        backend.setSeeds({seed});
        backend.addRadio(seed, {{"a", "A", "X"}, {"b", "B", "X"}, {"c", "C", "X"}});
        auto current = [&] { return backend.current().value("id").toString(); };
        backend.previous(); // nothing has played yet
        QVERIFY(backend.current().isEmpty());
        backend.togglePause();
        backend.next();
        QCOMPARE(current(), QString("b"));
        backend.previous();
        QCOMPARE(current(), QString("a"));
        QVERIFY(!backend.hasPrevious());
        backend.next(); // the track it left, then on as before
        QCOMPARE(current(), QString("b"));
        backend.next();
        QCOMPARE(current(), QString("c"));
    }

    void dislikedTrackStaysOutForGood()
    {
        QTcpServer proxy;
        int requests = 0;
        failNetwork(proxy, requests);
        const Track seed{"s", "Seed", "A"};
        {
            Backend backend("/nonexistent/yt-dlp", "/nonexistent/mpv");
            backend.setSeeds({seed});
            backend.addRadio(seed, {{"a", "A", "X"}, {"b", "B", "X"}});
            backend.togglePause();
            backend.dislike();
            QCOMPARE(backend.current().value("id").toString(), QString("b"));
            QVERIFY(!backend.hasPrevious()); // P doesn't lead back to it either
        }
        Backend later("/nonexistent/yt-dlp", "/nonexistent/mpv");
        later.addRadio(seed, {{"a", "A", "X"}, {"c", "C", "X"}});
        QCOMPARE(later.queue().size(), 1);
        QCOMPARE(later.queue().first().id, QString("c"));
        // Picking it by hand is the way back.
        later.playId("a");
        QCOMPARE(later.current().value("id").toString(), QString("a"));
        Backend forgiven("/nonexistent/yt-dlp", "/nonexistent/mpv");
        forgiven.addRadio(seed, {{"a", "A", "X"}});
        QCOMPARE(forgiven.queue().size(), 1);
    }

    void resumesTheLastTrackWhereItStopped()
    {
        if (QStandardPaths::findExecutable("python3").isEmpty())
            QSKIP("needs python3 for the fake mpv");
        QTemporaryDir dir;
        const QString log = dir.filePath("seeks");
        // Starts playing at once and notes where it's told to seek.
        const QString mpv = script(dir.filePath("mpv"), "#!/usr/bin/env python3\n"
            "import json, socket, sys\n"
            "path = [a.split('=', 1)[1] for a in sys.argv if a.startswith('--input-ipc-server=')][0]\n"
            "server = socket.socket(socket.AF_UNIX); server.bind(path); server.listen(1)\n"
            "conn, _ = server.accept()\n"
            "for line in conn.makefile('rb'):\n"
            "    command = json.loads(line).get('command', [''])\n"
            "    if command[0] == 'loadfile':\n"
            "        conn.sendall(b'{\"event\":\"property-change\",\"name\":\"time-pos\",\"data\":0.0}\\n')\n"
            "    if command[0] == 'seek':\n"
            "        open('" + log.toUtf8() + "', 'a').write('%s %s\\n' % (command[1], command[2]))\n"
            "        conn.sendall(b'{\"event\":\"property-change\",\"name\":\"time-pos\",\"data\":%d}\\n' % command[1])\n");
        const QString ytdlp = script(dir.filePath("yt-dlp"),
            "#!/bin/sh\ncase \"$*\" in *flat-playlist*) sleep 1; "
            "echo '{\"entries\": [{\"id\": \"p1\", \"title\": \"Playlist Song\", \"channel\": \"A\"}]}';; *) exit 1;; esac\n");
        QTcpServer proxy;
        int requests = 0;
        failNetwork(proxy, requests);
        auto file = [&] { QFile seeks(log); return seeks.open(QIODevice::ReadOnly) ? seeks.readAll() : QByteArray(); };
        const Track seed{"s", "Seed", "A"};

        // A song's radio: the same song, at the second it had reached.
        {
            Backend backend(ytdlp, mpv);
            backend.setSeeds({seed});
            backend.addRadio(seed, {{"a", "Song, A", "X"}});
            backend.togglePause();
            backend.seekTo(42);
            QTRY_COMPARE(backend.position(), 42);
        }
        QFile::remove(log);
        {
            Backend backend(ytdlp, mpv);
            backend.resume();
            QCOMPARE(backend.current().value("title").toString(), QString("Song, A"));
            QTRY_COMPARE(file(), QByteArray("42 absolute\n"));
        }

        // A playlist's radio: the track plays while the playlist loads, and the playlist follows it.
        QSettings().setValue("playlistUrl", "https://music.youtube.com/playlist?list=PLx");
        QSettings().setValue("last/chain", false);
        Backend backend(ytdlp, mpv);
        backend.resume();
        QCOMPARE(backend.current().value("id").toString(), QString("a"));
        QTRY_COMPARE(backend.queue().size(), 1);
        QCOMPARE(backend.queue().first().id, QString("p1"));
        QCOMPARE(backend.current().value("id").toString(), QString("a"));
    }

#ifdef Q_OS_LINUX
    void mprisControlsTheRadio()
    {
        if (!QDBusConnection::sessionBus().isConnected())
            QSKIP("needs a D-Bus session bus");
        QTcpServer proxy;
        int requests = 0;
        failNetwork(proxy, requests);
        Backend backend("/nonexistent/yt-dlp", "/nonexistent/mpv");
        int raised = 0;
        const QString service = QString("org.mpris.MediaPlayer2.drift_test%1").arg(QCoreApplication::applicationPid());
        Mpris mpris(&backend, [&raised] { ++raised; }, [] {}, service);

        const QString path = "/org/mpris/MediaPlayer2", player = "org.mpris.MediaPlayer2.Player";
        auto get = [&](const QString &interface, const QString &name) {
            QDBusInterface properties(service, path, "org.freedesktop.DBus.Properties");
            return QDBusReply<QDBusVariant>(properties.call("Get", interface, name)).value().variant();
        };
        auto metadata = [&] { return qdbus_cast<QVariantMap>(get(player, "Metadata").value<QDBusArgument>()); };
        QCOMPARE(get("org.mpris.MediaPlayer2", "Identity").toString(), QString("Drift"));
        QCOMPARE(get(player, "PlaybackStatus").toString(), QString("Stopped"));

        const Track seed{"s", "Seed", "A"};
        backend.setSeeds({seed});
        backend.addRadio(seed, {{"a-1", "Song A", "X"}, {"b", "Song B", "Y"}});
        QDBusInterface(service, path, player).call("Play");
        QCOMPARE(get(player, "PlaybackStatus").toString(), QString("Playing"));
        QCOMPARE(metadata().value("xesam:title").toString(), QString("Song A"));
        QCOMPARE(metadata().value("mpris:trackid").value<QDBusObjectPath>().path(), QString("/org/drift/track/a_1"));

        QVERIFY(!get(player, "CanGoPrevious").toBool());
        QDBusInterface(service, path, player).call("Next");
        QCOMPARE(metadata().value("xesam:title").toString(), QString("Song B"));
        QVERIFY(get(player, "CanGoPrevious").toBool());
        QDBusInterface(service, path, player).call("Previous");
        QCOMPARE(metadata().value("xesam:title").toString(), QString("Song A"));
        QDBusInterface(service, path, player).call("Next");
        QCOMPARE(metadata().value("xesam:title").toString(), QString("Song B"));
        QVERIFY(QDBusInterface(service, path, player).setProperty("Volume", 0.3));
        QCOMPARE(backend.volume(), 30);
        QDBusInterface(service, path, "org.mpris.MediaPlayer2").call("Raise");
        QCOMPARE(raised, 1);
    }
#endif

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

    void nextStreamWaitsForCurrentToPlay()
    {
        if (QStandardPaths::findExecutable("python3").isEmpty())
            QSKIP("needs python3 for the fake mpv");
        QTemporaryDir dir;
        const QString log = dir.filePath("resolves");
        const QString ytdlp = script(dir.filePath("yt-dlp"),
            "#!/bin/sh\ncase \"$*\" in *v=second*) echo >> '" + log.toUtf8() + "';; esac\nexit 1\n");
        // Plays a second after each loadfile, as mpv does once its own yt-dlp has found the stream.
        const QString mpv = script(dir.filePath("mpv"), "#!/usr/bin/env python3\n"
            "import json, socket, sys, time\n"
            "path = [a.split('=', 1)[1] for a in sys.argv if a.startswith('--input-ipc-server=')][0]\n"
            "server = socket.socket(socket.AF_UNIX); server.bind(path); server.listen(1)\n"
            "conn, _ = server.accept()\n"
            "for line in conn.makefile('rb'):\n"
            "    if json.loads(line).get('command', [''])[0] == 'loadfile':\n"
            "        time.sleep(1)\n"
            "        conn.sendall(b'{\"event\":\"property-change\",\"name\":\"time-pos\",\"data\":1.5}\\n')\n");
        QTcpServer proxy;
        int requests = 0;
        failNetwork(proxy, requests);

        Backend backend(ytdlp, mpv);
        const Track seed{"s", "Seed", "A"};
        backend.setSeeds({seed});
        backend.addRadio(seed, {{"first", "First", "C"}, {"second", "Second", "C"}});
        backend.togglePause();
        QCOMPARE(backend.current().value("id").toString(), QString("first"));
        QTest::qWait(500);
        QCOMPARE(lines(log), 0); // mpv's own yt-dlp is still finding "first"
        QTRY_COMPARE(lines(log), 1);
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
