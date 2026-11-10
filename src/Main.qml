import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

ApplicationWindow {
    id: win
    width: 720
    height: 560
    minimumWidth: 520
    minimumHeight: 500
    visible: true
    title: backend.current.title ? backend.current.title + " — Drift" : "Drift"
    color: "#0e0e10"

    Material.theme: Material.Dark
    Material.accent: theme.accent
    Material.roundedScale: theme.radius

    property bool helpVisible: false
    // Single-key shortcuts step aside while typing or picking a result.
    readonly property bool keys: !urlField.activeFocus && !resultsView.activeFocus
    readonly property bool searching: backend.results.length > 0

    function clock(seconds) {
        const s = Math.max(0, Math.floor(seconds))
        return Math.floor(s / 60) + ":" + String(s % 60).padStart(2, "0")
    }

    function submit() {
        if (backend.isLink(urlField.text))
            content.forceActiveFocus()
        backend.submit(urlField.text)
    }

    function playResult(index) {
        backend.playSimilar(index)
        content.forceActiveFocus()
    }

    Connections {
        target: backend
        function onResultsChanged() {
            if (backend.results.length > 0) {
                resultsView.currentIndex = 0
                resultsView.forceActiveFocus()
            }
        }
    }

    Shortcut { sequence: "?"; enabled: win.keys; context: Qt.ApplicationShortcut; onActivated: win.helpVisible = !win.helpVisible }
    Shortcut { sequence: "Q"; enabled: win.keys; context: Qt.ApplicationShortcut; onActivated: Qt.quit() }
    Shortcut { sequence: "Space"; enabled: win.keys; context: Qt.ApplicationShortcut; onActivated: backend.togglePause() }
    Shortcut { sequence: "N"; enabled: win.keys; context: Qt.ApplicationShortcut; onActivated: backend.next() }
    Shortcut { sequence: "P"; enabled: win.keys; context: Qt.ApplicationShortcut; onActivated: backend.previous() }
    Shortcut { sequence: "X"; enabled: win.keys; context: Qt.ApplicationShortcut; onActivated: backend.dislike() }
    Shortcut { sequence: "L"; enabled: win.keys; context: Qt.ApplicationShortcut; onActivated: backend.normalize = !backend.normalize }
    Shortcut { sequence: "Left"; enabled: win.keys; context: Qt.ApplicationShortcut; onActivated: backend.seek(-10) }
    Shortcut { sequence: "Right"; enabled: win.keys; context: Qt.ApplicationShortcut; onActivated: backend.seek(10) }
    Shortcut { sequence: "Up"; enabled: win.keys; context: Qt.ApplicationShortcut; onActivated: backend.changeVolume(5) }
    Shortcut { sequence: "Down"; enabled: win.keys; context: Qt.ApplicationShortcut; onActivated: backend.changeVolume(-5) }
    Shortcut { sequence: "D"; enabled: win.keys; context: Qt.ApplicationShortcut; onActivated: discord.enabled = !discord.enabled }
    Shortcut { sequence: "Ctrl+L"; context: Qt.ApplicationShortcut; onActivated: { urlField.forceActiveFocus(); urlField.selectAll() } }
    Shortcut {
        sequence: "Escape"
        context: Qt.ApplicationShortcut
        onActivated: {
            if (win.helpVisible)
                win.helpVisible = false
            else if (win.searching && !urlField.activeFocus)
                backend.clearResults()
            content.forceActiveFocus()
        }
    }

    // Right-to-left languages mirror the window; the Translator sets the
    // application's layout direction.
    Item {
        id: content
        anchors.fill: parent
        focus: true
        LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 24
            spacing: 16

            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                TextField {
                    id: urlField
                    Layout.fillWidth: true
                    placeholderText: qsTr("Playlist link or song search")
                    selectByMouse: true
                    Component.onCompleted: text = backend.playlistUrl
                    onAccepted: win.submit()
                    Keys.onDownPressed: if (win.searching) resultsView.forceActiveFocus()
                }
                Button {
                    text: backend.isLink(urlField.text) ? qsTr("Start radio") : qsTr("Search")
                    Material.background: theme.accent
                    Material.foreground: theme.accentForeground
                    focusPolicy: Qt.NoFocus
                    Material.roundedScale: theme.radius
                    enabled: urlField.text.trim().length > 0
                    onClicked: win.submit()
                }
            }

            Label {
                Layout.fillWidth: true
                visible: backend.error !== ""
                text: backend.error
                color: "#ff6b6b"
                wrapMode: Text.Wrap
            }

            RowLayout {
                Layout.fillWidth: true
                visible: updater.version !== ""
                spacing: 12

                Label {
                    Layout.fillWidth: true
                    text: (updater.ready ? qsTr("Drift %1 is ready to install") : qsTr("Drift %1 is out")).arg(updater.version)
                    opacity: 0.7
                    elide: Text.ElideRight
                }
                Button {
                    text: updater.ready ? qsTr("Restart and update") : qsTr("Download the update")
                    flat: true
                    focusPolicy: Qt.NoFocus
                    Material.roundedScale: theme.radius
                    onClicked: updater.install()
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 20

                // shortcut: the cover keeps square corners when Hyprland rounds windows; mask it if that matters.
                Rectangle {
                    Layout.preferredWidth: 132
                    Layout.preferredHeight: 132
                    color: "#1c1c20"
                    clip: true

                    Image {
                        anchors.fill: parent
                        source: backend.current.id ? "https://i.ytimg.com/vi/" + backend.current.id + "/mqdefault.jpg" : ""
                        sourceSize.width: 320
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                        cache: false
                    }

                    MouseArea {
                        anchors.fill: parent
                        enabled: backend.current.id !== undefined
                        cursorShape: Qt.PointingHandCursor
                        onClicked: backend.togglePause()
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 6

                    Label {
                        visible: backend.current.id !== undefined
                        text: backend.following ? qsTr("Listening along") : backend.current.fromPlaylist ? qsTr("From your playlist") : qsTr("Radio")
                        color: theme.accent
                        font.pixelSize: 13
                    }
                    Label {
                        Layout.fillWidth: true
                        text: backend.current.title || qsTr("Paste a playlist link or search for a song")
                        font.pixelSize: 22
                        font.weight: Font.DemiBold
                        wrapMode: Text.Wrap
                        maximumLineCount: 2
                        elide: Text.ElideRight
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: text !== ""
                        text: backend.current.artist || ""
                        opacity: 0.7
                        elide: Text.ElideRight
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: text !== ""
                        text: backend.status
                        opacity: 0.5
                        elide: Text.ElideRight
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                Label {
                    text: win.clock(slider.pressed ? slider.value : backend.position)
                    opacity: 0.7
                    font.features: { "tnum": 1 }
                }
                Slider {
                    id: slider
                    Layout.fillWidth: true
                    from: 0
                    to: Math.max(1, backend.duration)
                    enabled: backend.duration > 0 && !backend.following // the host's to move
                    focusPolicy: Qt.NoFocus
                    onPressedChanged: if (!pressed) backend.seekTo(value)

                    Binding on value {
                        value: backend.position
                        when: !slider.pressed
                        restoreMode: Binding.RestoreNone
                    }
                }
                Label {
                    text: win.clock(backend.duration)
                    opacity: 0.7
                    font.features: { "tnum": 1 }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                Button {
                    Layout.preferredWidth: 120
                    text: backend.playing ? qsTr("Pause") : qsTr("Play")
                    Material.background: theme.accent
                    Material.foreground: theme.accentForeground
                    focusPolicy: Qt.NoFocus
                    Material.roundedScale: theme.radius
                    onClicked: backend.togglePause()
                }
                Button {
                    // In a room the host picks the tracks, so the way on is the way out.
                    text: backend.following ? qsTr("Stop listening along") : qsTr("Next")
                    focusPolicy: Qt.NoFocus
                    Material.roundedScale: theme.radius
                    enabled: backend.following || backend.current.id !== undefined
                    onClicked: backend.following ? backend.unfollow() : backend.next()
                }
                Item { Layout.fillWidth: true }
                Label {
                    text: qsTr("Volume %1%").arg(backend.volume.toLocaleString(Qt.locale(), 'f', 0))
                    opacity: 0.6
                }
                Label {
                    text: qsTr("? for shortcuts")
                    opacity: 0.4
                }
            }

            Label {
                visible: win.searching
                text: qsTr("Pick a song to play endless radio like it")
                font.pixelSize: 13
                opacity: 0.6
            }

            ListView {
                id: resultsView
                visible: win.searching
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: backend.results
                boundsBehavior: Flickable.StopAtBounds
                highlightMoveDuration: 0
                highlight: Rectangle {
                    color: "#1c1c20"
                    radius: Math.min(4, theme.radius)
                }
                Keys.onReturnPressed: win.playResult(currentIndex)
                Keys.onEnterPressed: win.playResult(currentIndex)
                Keys.onUpPressed: currentIndex > 0 ? decrementCurrentIndex() : urlField.forceActiveFocus()

                delegate: Item {
                    required property var modelData
                    required property int index
                    width: ListView.view.width
                    height: 40

                    MouseArea {
                        anchors.fill: parent
                        onClicked: win.playResult(index)
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 8
                        spacing: 12

                        Label {
                            Layout.fillWidth: true
                            text: modelData.title + "  ·  " + modelData.artist
                            elide: Text.ElideRight
                        }
                        Label {
                            text: modelData.duration ? win.clock(modelData.duration) : ""
                            opacity: 0.5
                            font.features: { "tnum": 1 }
                        }
                    }
                }
            }

            Label {
                visible: !win.searching && backend.upcoming.length > 0
                text: qsTr("Up next")
                font.pixelSize: 13
                opacity: 0.6
            }

            ListView {
                visible: !win.searching
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: backend.upcoming
                boundsBehavior: Flickable.StopAtBounds

                delegate: Item {
                    required property var modelData
                    required property int index
                    width: ListView.view.width
                    height: 36

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: backend.playQueued(index)
                    }

                    RowLayout {
                        anchors.fill: parent
                        spacing: 12

                        Label {
                            Layout.fillWidth: true
                            text: modelData.title + "  ·  " + modelData.artist
                            elide: Text.ElideRight
                            opacity: 0.85
                        }
                        Label {
                            visible: modelData.fromPlaylist
                            text: qsTr("playlist")
                            color: theme.accent
                            font.pixelSize: 12
                        }
                        Label {
                            text: modelData.duration ? win.clock(modelData.duration) : ""
                            opacity: 0.5
                            font.features: { "tnum": 1 }
                        }
                    }
                }
            }
        }

        Rectangle {
            anchors.fill: parent
            visible: win.helpVisible
            color: "#d90e0e10"

            MouseArea {
                anchors.fill: parent
                onClicked: win.helpVisible = false
            }

            Rectangle {
                anchors.centerIn: parent
                width: help.implicitWidth + 48
                height: help.implicitHeight + 48
                radius: theme.radius
                color: "#1c1c20"

                ColumnLayout {
                    id: help
                    anchors.centerIn: parent
                    spacing: 10

                    Repeater {
                        model: [
                            ["Space", qsTr("Play or pause")],
                            ["N  P", qsTr("Next or previous track")],
                            ["X", qsTr("Never play this track again")],
                            ["← →", qsTr("Seek 10 seconds")],
                            ["↑ ↓", qsTr("Volume")],
                            ["L", backend.normalize ? qsTr("Stop leveling the loudness of tracks") : qsTr("Level the loudness of tracks")],
                            ["Ctrl+L", qsTr("Search or change playlist")],
                            ["Enter", qsTr("Search, or play the picked song")],
                            ["Esc", qsTr("Close search results")],
                            ["D", discord.enabled ? qsTr("Hide the track from Discord") : qsTr("Show the track in Discord")],
                            ["?", qsTr("Show shortcuts")],
                            ["Q", qsTr("Quit")]
                        ].concat(Qt.platform.os === "windows" ? [ // global hotkeys, see smtc.cpp
                            ["Alt+X", qsTr("Play or pause, from anywhere")],
                            ["Alt+C", qsTr("Next track, from anywhere")],
                            ["Alt+V  Alt+B", qsTr("Volume down or up, from anywhere")]
                        ] : [])
                        delegate: RowLayout {
                            required property var modelData
                            spacing: 24
                            Label {
                                Layout.preferredWidth: 72
                                text: modelData[0]
                                color: theme.accent
                                font.weight: Font.DemiBold
                            }
                            Label { text: modelData[1] }
                        }
                    }
                }
            }
        }
    }
}
