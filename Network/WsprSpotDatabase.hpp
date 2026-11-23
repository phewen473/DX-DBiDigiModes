#ifndef WSPR_SPOT_DATABASE_HPP_
#define WSPR_SPOT_DATABASE_HPP_

// DX-DBi v2R0: log WSPR / FST4W spots to a MariaDB (or MySQL) database.
//
// WsprSpotDatabase lives in its own thread so a slow or unreachable
// server never stalls the GUI. Spots that cannot be written are kept
// (up to a limit) and retried with the next receive period.
// The static helpers (test / create) block and are meant to be run
// off the GUI thread (the settings dialog uses QtConcurrent).

#include <QObject>
#include <QString>
#include <QStringList>
#include <QDateTime>
#include <QVector>
#include <QMetaType>

class QSettings;

struct WsprDbSettings
{
  bool enabled {false};          // log spots while receiving
  bool logFileDecodes {false};   // also log decodes of files opened with File > Open
  QString host {"localhost"};   // default server; change it in Database > WSPR Database Settings
  int port {3306};
  QString database {"wsprphe"};
  QString user;
  QString password;
  bool rememberPassword {true};
  // false: allow a server without TLS (home LAN); TLS is still used, unverified,
  // when the server offers it. true: MariaDB Connector/C 3.4 default - the
  // connection must be TLS with a verifiable server certificate.
  bool requireTls {false};

  static WsprDbSettings load (QSettings *);
  void save (QSettings *) const;
  QString describe () const;     // user@host:port/database
  bool complete () const { return !host.isEmpty () && !user.isEmpty () && validDatabaseName (database); }
  static bool validDatabaseName (QString const&);
};

struct WsprSpot
{
  QDateTime periodStartUtc;      // start of the receive period (UTC)
  QString mode;                  // WSPR-2, FST4W-120, FST4W-300 ...
  QString band;                  // 30m ...
  qint64 dialFreqHz {0};
  double freqMHz {0};            // RF frequency of the received signal
  int snrDb {0};
  double dtSec {0};
  int driftHz {0};
  bool hasSync {false};
  double sync {0};
  QString message;               // as decoded, e.g. "K1ABC FN42 37"
  int messageType {0};           // 1: call grid4 dBm, 2: compound call dBm, 3: <call> grid6 dBm
  QString txCall;
  QString txGrid;
  int txPowerDbm {-999};         // -999 = unknown
  QString rxCall;
  QString rxGrid;
  int distanceKm {-1};
  int azimuthDeg {-1};
  bool hasDecoderDetail {false}; // fields below come from wsprd
  int cycles {0};
  int jitter {0};
  int blockSize {0};
  int decodeType {0};            // 0 Fano, 1 OSD
  int hardErrors {0};
  int pass {0};
  quint32 metric {0};
  bool fromFile {false};
  QString radio;                 // DX-DBi v2R2: radio that received it (own name from the Database menu, else rig model)
  QDateTime insertedAtUtc;       // DX-DBi v2R2: when the program logged it (UTC, whole seconds) - the same in every log

  // split a WSPR message into call / grid / power; returns the type (0 = unknown)
  static int parseMessage (QString const& message, QString& call, QString& grid, int& dbm);
};

struct WsprPeriod
{
  bool valid {false};
  QDateTime startUtc;
  QString mode;
  QString band;
  qint64 dialFreqHz {0};
  QString rxCall;
  QString rxGrid;
  int decodes {0};
  bool fromFile {false};
  QString radio;                 // DX-DBi v2R2: radio that listened in this period
  QDateTime insertedAtUtc;       // DX-DBi v2R2: when the program logged it (UTC, whole seconds) - the same in every log
};

// DX-DBi v2R2: one decode of any other mode (FT8, FT4, JT65, JT9, Q65, MSK144, FST4 ...),
// logged to the "decodes" table / file. WSPR and FST4W keep using the WSPR tables above.
struct DecodeRecord
{
  QDateTime timeUtc;             // start of the receive period (UTC); for fast modes the time of the decode
  QString mode;                  // FT8, FT4, JT65, JT9, Q65-30A, MSK144 ...
  QString band;                  // 20m ...; empty when not known (decodes of a recording)
  qint64 dialFreqHz {0};         // dial frequency; 0 for decodes of a recording
  int audioFreqHz {0};           // audio frequency of the signal (offset from the dial)
  int snrDb {0};                 // signal to noise ratio as reported by the decoder
  double dtSec {0};              // time offset in seconds
  QString message;               // the decoded message, e.g. "CQ K1ABC FN42"
  QString deCall;                // sending station when it can be found in the message
  QString deGrid;                // sending station's grid when present in the message
  QString toCall;                // station called ("CQ", "QRZ" or a call sign) - the first word
  QString rxCall;                // our call sign
  QString rxGrid;                // our grid
  int distanceKm {-1};           // distance to the sender's grid, -1 = unknown
  int azimuthDeg {-1};           // beam heading to the sender's grid, -1 = unknown
  bool lowConfidence {false};    // decoder marked it with "?" (a-priori / low confidence)
  bool fromFile {false};         // decoded from a recording opened with File > Open
  QString radio;                 // radio that received it (own name from the Database menu, else rig model)
  QDateTime insertedAtUtc;       // when the program logged it (UTC, whole seconds) - the same in every log
};

// DX-DBi v2R2: the moment to store in inserted_at - the stamp set by the program, or now if it was not set
inline QDateTime insertedAtOrNow (QDateTime const& stamp)
{
  auto const t = stamp.isValid () ? stamp.toUTC () : QDateTime::currentDateTimeUtc (); // the stamp, else now
  return QDateTime {t.date (), QTime {t.time ().hour (), t.time ().minute (), t.time ().second ()}, Qt::UTC}; // whole seconds, UTC
}

Q_DECLARE_METATYPE (DecodeRecord);                // allows sending a DecodeRecord between threads
Q_DECLARE_METATYPE (QVector<DecodeRecord>);       // allows sending a list of them between threads

Q_DECLARE_METATYPE (WsprDbSettings);
Q_DECLARE_METATYPE (WsprSpot);
Q_DECLARE_METATYPE (QVector<WsprSpot>);
Q_DECLARE_METATYPE (WsprPeriod);

class WsprSpotDatabase final
  : public QObject
{
  Q_OBJECT

public:
  explicit WsprSpotDatabase (QObject * parent = nullptr);
  ~WsprSpotDatabase ();

  struct Result
  {
    bool ok {false};
    QString message;
  };

  // blocking helpers (safe to call from any thread, each uses its own connection)
  static Result testConnection (WsprDbSettings const&);
  static Result createDatabaseAndTables (WsprDbSettings const&);
  static bool driverAvailable (QString * why = nullptr);
  static void registerTypes ();
  static void setClientPluginDir ();   // point MariaDB Connector/C at the bundled auth plugins

  // the stored password is protected with Windows DPAPI (current user)
  static QString protectPassword (QString const&);
  static QString unprotectPassword (QString const&);

  Q_SLOT void configure (WsprDbSettings const&);
  Q_SLOT void logSpots (QVector<WsprSpot> const&, WsprPeriod const&);
  Q_SLOT void logDecodes (QVector<DecodeRecord> const&);   // DX-DBi v2R2: decodes of all other modes
  Q_SLOT void shutdown ();

  Q_SIGNAL void status (QString const& message, bool error);

private:
  bool ensureOpen (QString * error);
  void closeConnection ();
  bool flush (QString * error);

  WsprDbSettings settings_;
  QString connection_name_;
  bool open_ {false};
  bool last_failed_ {false};
  int last_inserted_ {0};
  QVector<WsprSpot> pending_spots_;
  QVector<WsprPeriod> pending_periods_;
  bool flushDecodes (int * inserted, QString * error);     // DX-DBi v2R2: write the waiting decodes
  QVector<DecodeRecord> pending_decodes_;                  // DX-DBi v2R2: decodes not yet written
  bool decodes_table_checked_ {false};                     // DX-DBi v2R2: tables brought up to date on this connection
  bool ensureSchema (QString * error);                     // DX-DBi v2R2: create "decodes", add "radio" columns to older tables
};

#endif
