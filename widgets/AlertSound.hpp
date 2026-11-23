#ifndef WSJTX_ALERT_SOUND_HPP__
#define WSJTX_ALERT_SOUND_HPP__

// DX-DBi v1R0: one shared player for the audio alerts (Windows).
//
// The original code created a new QAudioOutput and up to 12 QFile objects,
// all parented to a window, every time an alert was checked or played, and
// never deleted them, so memory and audio handles grew for the whole
// session.  play_alert_wav() keeps at most one player and one file alive:
// starting a new sound stops and frees the previous one.

#include <QString>

#ifdef WIN32
#include <QAudio>
#include <QAudioFormat>
#include <QAudioOutput>
#include <QCoreApplication>
#include <QFile>

inline void play_alert_wav (QString const& path)
{
  static QAudioOutput * audio {nullptr};
  static QFile * file {nullptr};
  if (audio)
    {
      audio->stop ();
      audio->deleteLater ();
      audio = nullptr;
    }
  if (file)
    {
      file->close ();
      file->deleteLater ();
      file = nullptr;
    }
  if (path.isEmpty ()) return;
  file = new QFile {path, qApp};
  if (!file->open (QIODevice::ReadOnly))
    {
      file->deleteLater ();
      file = nullptr;
      return;
    }
  QAudioFormat format;
  format.setCodec ("audio/pcm");
  format.setSampleRate (48000);
  format.setChannelCount (1);
  format.setSampleSize (16);
  format.setSampleType (QAudioFormat::SignedInt);
  audio = new QAudioOutput {format, qApp};
  auto * player = audio;
  QObject::connect (player, &QAudioOutput::stateChanged, [player] (QAudio::State state) {
      if (QAudio::IdleState == state) player->stop (); // release the device when done
    });
  audio->start (file);
}
#endif

#endif
