#include <QDir>
#include <QFile>
#include <QSet>
#include <QStandardPaths>
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
        QTemporaryDir dir;
        const QString response = dir.filePath("response.json"), command = dir.filePath("hyprctl");
        auto write = [](const QString &path, const QByteArray &data) {
            QFile file(path);
            return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
        };
        QVERIFY(write(command, "#!/bin/sh\ncat '" + response.toUtf8() + "'\n"));
        QVERIFY(QFile::setPermissions(command, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QVERIFY(write(response, "{\"int\":0}"));

        Theme theme(dir.path(), nullptr, command);
        QCOMPARE(theme.radius(), 0);
        QVERIFY(write(response, "{\"int\":6}"));
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
    static void write(const QString &path, const QByteArray &contents)
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(contents);
    }
};

QTEST_MAIN(DriftTests)
#include "drift_tests.moc"
