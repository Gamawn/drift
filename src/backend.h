#pragma once

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QSet>
#include <QVariant>

#include "mpv.h"

struct Track {
    QString id;
    QString title;
    QString artist;
    double duration = 0;
    bool fromPlaylist = false;

    QVariantMap toMap() const;
};

// Endless radio from a playlist: each song from the playlist takes a turn as
// the seed, followed by a few tracks from YouTube's own radio mix for it, so
// the stream stays close to the playlist's taste and never runs out.
class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString playlistUrl READ playlistUrl NOTIFY playlistUrlChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY playingChanged)
    Q_PROPERTY(QVariantMap current READ current NOTIFY currentChanged)
    Q_PROPERTY(QVariantList upcoming READ upcoming NOTIFY upcomingChanged)
    Q_PROPERTY(QVariantList results READ results NOTIFY resultsChanged)
    Q_PROPERTY(int position READ position NOTIFY positionChanged)
    Q_PROPERTY(int duration READ duration NOTIFY durationChanged)
    Q_PROPERTY(int volume READ volume NOTIFY volumeChanged)
    Q_PROPERTY(bool normalize READ normalize WRITE setNormalize NOTIFY normalizeChanged)

public:
    explicit Backend(const QString &ytdlp = "yt-dlp", const QString &mpv = "mpv", QObject *parent = nullptr);
    ~Backend() override;

    QString playlistUrl() const { return m_playlistUrl; }
    QString status() const { return m_status; }
    QString error() const { return m_error; }
    bool playing() const { return !m_current.id.isEmpty() && !m_paused; }
    QVariantMap current() const { return m_current.toMap(); }
    QVariantList upcoming() const;
    QVariantList results() const;
    int position() const { return m_position; }
    int duration() const { return m_duration; }
    int volume() const { return m_volume; }
    bool normalize() const { return m_normalize; }
    // Evens out the loudness between tracks, which YouTube leaves to its own player.
    void setNormalize(bool normalize);
    bool hasPrevious() const { return !m_history.isEmpty(); }

    // A playlist link starts its radio; anything else searches for songs.
    Q_INVOKABLE void submit(const QString &text);
    Q_INVOKABLE void start(const QString &url);
    Q_INVOKABLE void search(const QString &query);
    Q_INVOKABLE void clearResults();
    // Endless radio of songs like this search result.
    Q_INVOKABLE void playSimilar(int index);
    // The same radio for a track known only by its id, as a drift:// link gives it.
    void playId(const QString &id);
    // Plays the track the last run ended on, from where it stopped, and its radio after it.
    void resume();
    Q_INVOKABLE void togglePause();
    Q_INVOKABLE void next();
    Q_INVOKABLE void previous();
    // Skips the current track and keeps it out of every radio from now on.
    Q_INVOKABLE void dislike();
    // Plays this track from Up next now and rebuilds Up next from its radio.
    Q_INVOKABLE void playQueued(int index);
    Q_INVOKABLE void seek(int seconds);
    Q_INVOKABLE void seekTo(int seconds);
    Q_INVOKABLE void changeVolume(int delta);
    void setVolume(int volume);

    // The radio's queue, public so tests can drive it without the network.
    static QList<Track> parseEntries(const QByteArray &json);
    static QList<Track> parseRadio(const QByteArray &json);
    static QList<Track> parseSearch(const QByteArray &json);
    Q_INVOKABLE static bool isLink(const QString &text);
    static QString songKey(const Track &track);
    // The video id in a drift://play/<id> link, or nothing for anything else.
    static QString linkId(const QString &link);
    void setSeeds(const QList<Track> &seeds);
    Track nextSeed();
    void addRadio(const Track &seed, const QList<Track> &mix);
    QList<Track> queue() const { return m_queue; }

signals:
    void playlistUrlChanged();
    void statusChanged();
    void errorChanged();
    void playingChanged();
    void currentChanged();
    void upcomingChanged();
    void resultsChanged();
    void positionChanged();
    void durationChanged();
    void volumeChanged();
    void normalizeChanged();

private:
    void playlistLoaded(int code, QProcess::ExitStatus status);
    void mixLoaded();
    void playSong(const Track &song);
    void reset();
    void refill();
    void fetchMix(const Track &seed);
    void abortMix();
    void queueChanged();
    void playNext();
    void resolveNext();
    void trackEnded(bool failed);
    void setStatus(const QString &status);
    void setError(const QString &error, bool clearsOnPlay = false);

    QString m_ytdlp;
    Mpv m_mpv;
    QProcess m_fetch;
    bool m_loadingPlaylist = false;
    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_mixReply, m_searchReply;
    QList<Track> m_results;
    QProcess m_resolve;
    QString m_resolvingId, m_streamId, m_streamUrl;

    QString m_playlistUrl, m_status, m_error;
    bool m_errorClearsOnPlay = false;
    QList<Track> m_seeds;
    QList<int> m_seedOrder;
    int m_seedCursor = 0, m_cycle = -1, m_failures = 0, m_playFailures = 0, m_emptyMixes = 0;
    Track m_mixSeed;
    // A song's radio has a single seed and follows the mixes from it.
    bool m_chain = false;
    Track m_chainSeed;
    QList<Track> m_queue;
    QSet<QString> m_seen;
    // Video ids, not song keys: a key is only a title, too wide a net to ban for good.
    QSet<QString> m_disliked;
    Track m_current;
    QList<Track> m_history;
    bool m_waiting = false, m_paused = false, m_loadedStream = false, m_resuming = false, m_normalize = true;
    int m_position = 0, m_duration = 0, m_volume = 80, m_resumeAt = 0;
};
