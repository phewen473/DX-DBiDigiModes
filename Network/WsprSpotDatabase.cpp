// DX-DBi v2R0: log WSPR / FST4W spots to MariaDB (see WsprSpotDatabase.hpp)

#include "WsprSpotDatabase.hpp"

#include <QSettings>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include <QRegularExpression>
#include <QAtomicInt>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QtMath>

#ifdef Q_OS_WIN
#include <windows.h>
#include <wincrypt.h>
#endif

#include "moc_WsprSpotDatabase.cpp"

namespace
{
  QAtomicInt connection_counter {0};
  int const max_pending_spots {20000};
  int const max_pending_periods {2000};
  char const * const driver_name = "QMYSQL";
  char const * const settings_group = "WSJTX-PHE-WsprDatabase";
  // keep a dead server from hanging the logger thread for long
  char const * const connect_options =
    "MYSQL_OPT_CONNECT_TIMEOUT=5;MYSQL_OPT_READ_TIMEOUT=20;MYSQL_OPT_WRITE_TIMEOUT=20";

  QString new_connection_name (QString const& tag)
  {
    return QString {"wsprphe_%1_%2"}.arg (tag).arg (connection_counter.fetchAndAddRelaxed (1));
  }

  // removes a named connection when it goes out of scope; declare it
  // before the QSqlDatabase / QSqlQuery objects that use the connection
  struct ScopedConnection
  {
    QString name;
    ~ScopedConnection ()
    {
      if (QSqlDatabase::contains (name))
        {
          {
            auto db = QSqlDatabase::database (name, false);
            if (db.isOpen ()) db.close ();
          }
          QSqlDatabase::removeDatabase (name);
        }
    }
  };

  QSqlDatabase make_db (WsprDbSettings const& s, QString const& name, bool with_database)
  {
    // MariaDB Connector/C 3.4 insists on TLS with a verified certificate and
    // Qt 5's driver cannot pass MYSQL_OPT_SSL_* options; since C/C 3.4.4 this
    // environment variable (read at connect time) relaxes that, which is
    // needed for a LAN server without TLS
    if (s.requireTls)
      qunsetenv ("MARIADB_TLS_DISABLE_PEER_VERIFICATION");
    else
      qputenv ("MARIADB_TLS_DISABLE_PEER_VERIFICATION", "1");
    auto db = QSqlDatabase::addDatabase (driver_name, name);
    db.setHostName (s.host.trimmed ());
    db.setPort (s.port);
    db.setUserName (s.user);
    db.setPassword (s.password);
    if (with_database) db.setDatabaseName (s.database);
    db.setConnectOptions (connect_options);
    return db;
  }

  QString error_text (QSqlError const& e)
  {
    auto t = e.text ().trimmed ();
    return t.isEmpty () ? QString {"unknown error"} : t;
  }

  QString connect_hint (QString const& error)
  {
    if (error.contains ("Access denied", Qt::CaseInsensitive))
      return QObject::tr ("Check the user name and password, and that the user is allowed to "
                          "connect from this computer (e.g. 'user'@'%' or 'user'@'192.168.%').");
    if (error.contains ("Unknown database", Qt::CaseInsensitive))
      return QObject::tr ("The database does not exist yet - use \"Create Database & Tables\".");
    if (error.contains ("Can't connect", Qt::CaseInsensitive)
        || error.contains ("timed out", Qt::CaseInsensitive)
        || error.contains ("Unknown server host", Qt::CaseInsensitive)
        || error.contains ("Unknown MySQL server host", Qt::CaseInsensitive))
      return QObject::tr ("Check the host name and port, that MariaDB is running, that its "
                          "bind-address allows network connections, and the firewall.");
    if (error.contains ("SSL", Qt::CaseInsensitive) || error.contains ("TLS", Qt::CaseInsensitive))
      return QObject::tr ("The server does not offer TLS, or its certificate could not be verified. "
                          "For a server on your own network, untick \"Require verified TLS\".");
    return {};
  }

  QString with_hint (QString const& error)
  {
    auto const& hint = connect_hint (error);
    return hint.isEmpty () ? error : error + "\n" + hint;
  }

  // DX-DBi v2R2: table for decodes of all other modes (FT8, FT4, JT65, JT9, Q65, MSK144, FST4 ...)
  QString decodes_table_statement ()
  {
    return R"(CREATE TABLE IF NOT EXISTS decodes (
  id               BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  decode_time_utc  DATETIME        NOT NULL COMMENT 'start of the receive period, UTC',
  mode             VARCHAR(12)     NOT NULL COMMENT 'FT8, FT4, JT65, Q65-30A ...',
  band             VARCHAR(8)      NULL,
  dial_freq_hz     BIGINT          NOT NULL COMMENT '0 for decodes of a recording',
  audio_freq_hz    INT             NOT NULL,
  snr_db           SMALLINT        NOT NULL,
  dt_s             DECIMAL(5,2)    NOT NULL,
  message          VARCHAR(64)     NOT NULL,
  to_call          VARCHAR(16)     NULL     COMMENT 'first word: CQ, QRZ or the station called',
  de_call          VARCHAR(16)     NULL     COMMENT 'sending station, when found',
  de_grid          VARCHAR(6)      NULL,
  rx_call          VARCHAR(16)     NOT NULL,
  rx_grid          VARCHAR(6)      NULL,
  distance_km      INT             NULL,
  azimuth_deg      SMALLINT        NULL     COMMENT 'from receiver to sender',
  low_confidence   TINYINT(1)      NOT NULL DEFAULT 0 COMMENT 'decoder marked it with ?',
  from_file        TINYINT(1)      NOT NULL DEFAULT 0,
  program          VARCHAR(40)     NULL,
  radio            VARCHAR(40)     NULL     COMMENT 'radio that received it',
  inserted_at_utc  DATETIME        NULL     COMMENT 'when the program logged it, UTC (same value in the CSV and Access logs)',
  inserted_at_local DATETIME       NULL     COMMENT 'the same moment in the logging PC local time',
  PRIMARY KEY (id),
  UNIQUE KEY uq_decode (decode_time_utc, mode, rx_call, audio_freq_hz, message),
  KEY ix_time (decode_time_utc),
  KEY ix_band_time (band, decode_time_utc),
  KEY ix_de_call (de_call)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4)";    // one row per decode; the unique key stops the same decode being stored twice
  }

  // tables, kept compatible with MariaDB 10.2+ and MySQL 5.7+
  QStringList table_statements ()
  {
    return {
      R"(CREATE TABLE IF NOT EXISTS wspr_spots (
  id               BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  spot_time_utc    DATETIME        NOT NULL COMMENT 'start of the receive period, UTC',
  mode             VARCHAR(10)     NOT NULL COMMENT 'WSPR-2, FST4W-120, FST4W-300 ...',
  band             VARCHAR(8)      NULL,
  dial_freq_hz     BIGINT          NOT NULL,
  freq_mhz         DECIMAL(12,7)   NOT NULL COMMENT 'RF frequency of the received signal',
  audio_freq_hz    DECIMAL(9,2)    NULL     COMMENT 'freq_mhz minus dial frequency',
  snr_db           SMALLINT        NOT NULL COMMENT 'in 2500 Hz bandwidth',
  dt_s             DECIMAL(5,2)    NOT NULL,
  drift_hz         SMALLINT        NULL,
  sync_quality     DECIMAL(5,2)    NULL,
  message          VARCHAR(40)     NOT NULL,
  message_type     TINYINT         NULL     COMMENT '1 call+grid4+dBm, 2 compound call+dBm, 3 <call>+grid6+dBm',
  tx_call          VARCHAR(16)     NOT NULL,
  tx_grid          VARCHAR(6)      NULL,
  tx_power_dbm     SMALLINT        NULL,
  tx_power_w       DOUBLE GENERATED ALWAYS AS (POWER(10, (tx_power_dbm - 30) / 10)) VIRTUAL,
  rx_call          VARCHAR(16)     NOT NULL,
  rx_grid          VARCHAR(6)      NULL,
  distance_km      INT             NULL,
  azimuth_deg      SMALLINT        NULL     COMMENT 'from receiver to transmitter',
  decoder_cycles   INT             NULL     COMMENT 'Fano cycles per bit (wsprd)',
  jitter           SMALLINT        NULL,
  block_size       TINYINT         NULL,
  decode_type      TINYINT         NULL     COMMENT '0 Fano, 1 OSD',
  hard_errors      SMALLINT        NULL,
  decoder_pass     TINYINT         NULL,
  fano_metric      INT UNSIGNED    NULL,
  from_file        TINYINT(1)      NOT NULL DEFAULT 0,
  program          VARCHAR(40)     NULL,
  radio            VARCHAR(40)     NULL     COMMENT 'radio that received it (DX-DBi v2R2)',
  inserted_at_utc  DATETIME        NULL     COMMENT 'when the program logged it, UTC (same value in the CSV and Access logs)',
  inserted_at_local DATETIME       NULL     COMMENT 'the same moment in the logging PC local time',
  PRIMARY KEY (id),
  UNIQUE KEY uq_spot (spot_time_utc, mode, rx_call, tx_call, freq_mhz),
  KEY ix_time (spot_time_utc),
  KEY ix_band_time (band, spot_time_utc),
  KEY ix_tx_call (tx_call),
  KEY ix_tx_grid (tx_grid)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4)",

      R"(CREATE TABLE IF NOT EXISTS wspr_rx_periods (
  id               BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  period_start_utc DATETIME        NOT NULL,
  mode             VARCHAR(10)     NOT NULL,
  band             VARCHAR(8)      NULL,
  dial_freq_hz     BIGINT          NOT NULL,
  rx_call          VARCHAR(16)     NOT NULL,
  rx_grid          VARCHAR(6)      NULL,
  decodes          SMALLINT        NOT NULL COMMENT 'spots decoded in this period (0 = band listened, nothing heard)',
  from_file        TINYINT(1)      NOT NULL DEFAULT 0,
  radio            VARCHAR(40)     NULL     COMMENT 'radio that listened (DX-DBi v2R2)',
  inserted_at_utc  DATETIME        NULL     COMMENT 'when the program logged it, UTC (same value in the CSV and Access logs)',
  inserted_at_local DATETIME       NULL     COMMENT 'the same moment in the logging PC local time',
  PRIMARY KEY (id),
  UNIQUE KEY uq_period (period_start_utc, mode, rx_call, dial_freq_hz),
  KEY ix_time (period_start_utc)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4)",

      R"(CREATE TABLE IF NOT EXISTS wsprphe_schema (
  name  VARCHAR(32) NOT NULL,
  value VARCHAR(64) NOT NULL,
  PRIMARY KEY (name)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4)",

      decodes_table_statement (),   // DX-DBi v2R2: decodes of all other modes

      // DX-DBi v2R2: tables made by v2R0 / v2R1 (or earlier v2R2 builds) get the new "radio" column
      "ALTER TABLE wspr_spots ADD COLUMN IF NOT EXISTS radio VARCHAR(40) NULL COMMENT 'radio that received it'",
      "ALTER TABLE wspr_rx_periods ADD COLUMN IF NOT EXISTS radio VARCHAR(40) NULL COMMENT 'radio that listened'",
      "ALTER TABLE decodes ADD COLUMN IF NOT EXISTS radio VARCHAR(40) NULL COMMENT 'radio that received it'",

      R"(INSERT INTO wsprphe_schema (name, value) VALUES ('schema_version', '2')
  ON DUPLICATE KEY UPDATE value = VALUES(value))",   // DX-DBi v2R2: schema 2 = schema 1 plus the decodes table and radio columns
    };
  }

  // DX-DBi v2R2: run once per connection before writing, so an existing database is brought up to date
  // without the user having to run Create Database & Tables (ADD COLUMN IF NOT EXISTS needs MariaDB 10.0.2+)
  QStringList upgrade_statements ()
  {
    return {
      decodes_table_statement (),                                                                     // decodes table, if missing
      "ALTER TABLE wspr_spots ADD COLUMN IF NOT EXISTS radio VARCHAR(40) NULL COMMENT 'radio that received it'",  // radio column on spots
      "ALTER TABLE wspr_rx_periods ADD COLUMN IF NOT EXISTS radio VARCHAR(40) NULL COMMENT 'radio that listened'", // radio column on periods
      "ALTER TABLE decodes ADD COLUMN IF NOT EXISTS radio VARCHAR(40) NULL COMMENT 'radio that received it'",     // radio column on decodes
      // the two logging-time columns, the same in every log (UTC and the logging PC's local time)
      "ALTER TABLE wspr_spots ADD COLUMN IF NOT EXISTS inserted_at_utc DATETIME NULL COMMENT 'when the program logged it, UTC'",          // spots
      "ALTER TABLE wspr_spots ADD COLUMN IF NOT EXISTS inserted_at_local DATETIME NULL COMMENT 'the same moment in local time'",         // spots
      "ALTER TABLE wspr_rx_periods ADD COLUMN IF NOT EXISTS inserted_at_utc DATETIME NULL COMMENT 'when the program logged it, UTC'",     // periods
      "ALTER TABLE wspr_rx_periods ADD COLUMN IF NOT EXISTS inserted_at_local DATETIME NULL COMMENT 'the same moment in local time'",    // periods
      "ALTER TABLE decodes ADD COLUMN IF NOT EXISTS inserted_at_utc DATETIME NULL COMMENT 'when the program logged it, UTC'",             // decodes
      "ALTER TABLE decodes ADD COLUMN IF NOT EXISTS inserted_at_local DATETIME NULL COMMENT 'the same moment in local time'",            // decodes
    };
  }

  // DX-DBi v2R2: tables made by older versions have one "inserted_at" TIMESTAMP set by the server, which
  // MariaDB shows in the server's time zone. Fill the new inserted_at_utc / inserted_at_local columns of those
  // older rows from it: a TIMESTAMP is read in the session time zone, so read it once in the server's own
  // zone (local time) and once in UTC. The old column is left as it is.
  bool backfill_inserted_at (QSqlQuery& q, QString * error)
  {
    // DX-DBi v2R3: the fill-in only needs to happen once per database - the UPDATEs scan whole tables, so
    // remember in wsprphe_schema that it is done and skip it on later connections
    if (q.exec ("SELECT value FROM wsprphe_schema WHERE name = 'inserted_at_backfill'") && q.next ()
        && q.value (0).toString () == "done") return true;   // already done earlier: nothing to scan
    for (auto const& table : QStringList {"wspr_spots", "wspr_rx_periods", "decodes"})   // tables that may have the old column
      {
        q.prepare ("SELECT COUNT(*) FROM information_schema.COLUMNS "
                   "WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = ? AND COLUMN_NAME = 'inserted_at'"); // old column there?
        q.addBindValue (table);                 // which table
        if (!q.exec () || !q.next () || q.value (0).toInt () == 0) continue; // no old column: nothing to fill
        QStringList const steps {
          "SET time_zone = 'SYSTEM'",           // read the old TIMESTAMP in the server's local time
          QString {"UPDATE `%1` SET inserted_at_local = inserted_at WHERE inserted_at_local IS NULL"}.arg (table), // local copy
          "SET time_zone = '+00:00'",           // read it in UTC
          QString {"UPDATE `%1` SET inserted_at_utc = inserted_at WHERE inserted_at_utc IS NULL"}.arg (table),     // UTC copy
        };
        for (auto const& sql : steps)           // run the four steps
          {
            if (!q.exec (sql))                  // a step failed
              {
                *error = error_text (q.lastError ()); // report why
                return false;                   // stop: try again on the next connection
              }
          }
      }
    q.exec ("INSERT INTO wsprphe_schema (name, value) VALUES ('inserted_at_backfill', 'done') "
            "ON DUPLICATE KEY UPDATE value = VALUES(value)");  // DX-DBi v2R3: remember it (a failure here only means it runs again)
    return true;                                // older rows filled
  }

  QStringList const our_tables {"wspr_spots", "wspr_rx_periods", "wsprphe_schema", "decodes"}; // DX-DBi v2R2: decodes added

  QVariant null_if (bool is_null, QVariant const& v)
  {
    return is_null ? QVariant {} : v;
  }

  // DX-DBi v2R2: the logging moment as text for the two DATETIME columns (text, so no driver time zone shifts)
  QString utc_text (QDateTime const& stamp)
  {
    return insertedAtOrNow (stamp).toString ("yyyy-MM-dd HH:mm:ss");               // UTC
  }
  QString local_text (QDateTime const& stamp)
  {
    return insertedAtOrNow (stamp).toLocalTime ().toString ("yyyy-MM-dd HH:mm:ss"); // the logging PC's local time
  }
}

//
// settings
//
bool WsprDbSettings::validDatabaseName (QString const& name)
{
  static QRegularExpression const re {R"(^[A-Za-z0-9_]{1,64}$)"};
  return re.match (name).hasMatch ();
}

WsprDbSettings WsprDbSettings::load (QSettings * settings)
{
  WsprDbSettings s;
  settings->beginGroup (settings_group);
  s.enabled = settings->value ("Enabled", s.enabled).toBool ();
  s.logFileDecodes = settings->value ("LogFileDecodes", s.logFileDecodes).toBool ();
  s.host = settings->value ("Host", s.host).toString ();
  s.port = settings->value ("Port", s.port).toInt ();
  s.database = settings->value ("Database", s.database).toString ();
  s.user = settings->value ("User", s.user).toString ();
  s.rememberPassword = settings->value ("RememberPassword", s.rememberPassword).toBool ();
  s.requireTls = settings->value ("RequireTls", s.requireTls).toBool ();
  if (s.rememberPassword)
    {
      s.password = WsprSpotDatabase::unprotectPassword (settings->value ("Password").toString ());
    }
  settings->endGroup ();
  return s;
}

void WsprDbSettings::save (QSettings * settings) const
{
  settings->beginGroup (settings_group);
  settings->setValue ("Enabled", enabled);
  settings->setValue ("LogFileDecodes", logFileDecodes);
  settings->setValue ("Host", host);
  settings->setValue ("Port", port);
  settings->setValue ("Database", database);
  settings->setValue ("User", user);
  settings->setValue ("RememberPassword", rememberPassword);
  settings->setValue ("RequireTls", requireTls);
  if (rememberPassword)
    {
      settings->setValue ("Password", WsprSpotDatabase::protectPassword (password));
    }
  else
    {
      settings->remove ("Password");
    }
  settings->endGroup ();
}

QString WsprDbSettings::describe () const
{
  return QString {"%1@%2:%3/%4"}.arg (user, host).arg (port).arg (database);
}

int WsprSpot::parseMessage (QString const& message, QString& call, QString& grid, int& dbm)
{
  static QRegularExpression const type1 {R"(^([A-Z0-9]{3,7})\s+([A-R]{2}[0-9]{2}(?:[A-X]{2})?)\s+([0-9]{1,2})$)"};
  static QRegularExpression const type2 {R"(^([A-Z0-9/]{3,13})\s+([0-9]{1,2})$)"};
  static QRegularExpression const type3 {R"(^<([A-Z0-9/.]{3,13})>\s+([A-R]{2}[0-9]{2}[A-X]{2})\s+([0-9]{1,2})$)"};
  auto const& m = message.simplified ().toUpper ();
  call.clear (); grid.clear (); dbm = -999;
  auto r = type1.match (m);
  if (r.hasMatch ())
    {
      call = r.captured (1); grid = r.captured (2); dbm = r.captured (3).toInt ();
      return 1;
    }
  r = type2.match (m);
  if (r.hasMatch ())
    {
      call = r.captured (1); dbm = r.captured (2).toInt ();
      return 2;
    }
  r = type3.match (m);
  if (r.hasMatch ())
    {
      call = r.captured (1); grid = r.captured (2); dbm = r.captured (3).toInt ();
      return 3;
    }
  call = m.section (' ', 0, 0).remove ('<').remove ('>');
  return 0;
}

//
// password protection
//
QString WsprSpotDatabase::protectPassword (QString const& plain)
{
  if (plain.isEmpty ()) return {};
#ifdef Q_OS_WIN
  QByteArray in = plain.toUtf8 ();
  DATA_BLOB in_blob {static_cast<DWORD> (in.size ()), reinterpret_cast<BYTE *> (in.data ())};
  DATA_BLOB out_blob {0, nullptr};
  if (CryptProtectData (&in_blob, L"DX-DBi WSPR database", nullptr, nullptr, nullptr,
                        CRYPTPROTECT_UI_FORBIDDEN, &out_blob))
    {
      QByteArray out {reinterpret_cast<char const *> (out_blob.pbData), static_cast<int> (out_blob.cbData)};
      LocalFree (out_blob.pbData);
      return "dpapi:" + QString::fromLatin1 (out.toBase64 ());
    }
#endif
  // not Windows (or DPAPI failed): obfuscated only
  return "b64:" + QString::fromLatin1 (plain.toUtf8 ().toBase64 ());
}

QString WsprSpotDatabase::unprotectPassword (QString const& stored)
{
  if (stored.startsWith ("dpapi:"))
    {
#ifdef Q_OS_WIN
      QByteArray in = QByteArray::fromBase64 (stored.mid (6).toLatin1 ());
      DATA_BLOB in_blob {static_cast<DWORD> (in.size ()), reinterpret_cast<BYTE *> (in.data ())};
      DATA_BLOB out_blob {0, nullptr};
      if (CryptUnprotectData (&in_blob, nullptr, nullptr, nullptr, nullptr,
                              CRYPTPROTECT_UI_FORBIDDEN, &out_blob))
        {
          auto plain = QString::fromUtf8 (reinterpret_cast<char const *> (out_blob.pbData),
                                          static_cast<int> (out_blob.cbData));
          SecureZeroMemory (out_blob.pbData, out_blob.cbData);
          LocalFree (out_blob.pbData);
          return plain;
        }
#endif
      return {};                // other user / other machine: ask again
    }
  if (stored.startsWith ("b64:"))
    {
      return QString::fromUtf8 (QByteArray::fromBase64 (stored.mid (4).toLatin1 ()));
    }
  return {};
}

//
// static helpers
//
void WsprSpotDatabase::registerTypes ()
{
  qRegisterMetaType<WsprDbSettings> ("WsprDbSettings");
  qRegisterMetaType<WsprSpot> ("WsprSpot");
  qRegisterMetaType<QVector<WsprSpot>> ("QVector<WsprSpot>");
  qRegisterMetaType<WsprPeriod> ("WsprPeriod");
  qRegisterMetaType<DecodeRecord> ("DecodeRecord");                  // DX-DBi v2R2: one decode of another mode
  qRegisterMetaType<QVector<DecodeRecord>> ("QVector<DecodeRecord>"); // DX-DBi v2R2: a batch of them
}

void WsprSpotDatabase::setClientPluginDir ()
{
  // the installer puts MariaDB Connector/C's authentication plugins in
  // mariadb_plugin/ next to the program; a build-tree run uses the
  // compiled-in MSYS2 path instead
  if (qEnvironmentVariableIsSet ("MARIADB_PLUGIN_DIR")) return;
  QDir dir {QCoreApplication::applicationDirPath ()};
  if (dir.exists ("mariadb_plugin"))
    {
      qputenv ("MARIADB_PLUGIN_DIR", QDir::toNativeSeparators (dir.absoluteFilePath ("mariadb_plugin")).toLocal8Bit ());
    }
}

bool WsprSpotDatabase::driverAvailable (QString * why)
{
  if (QSqlDatabase::isDriverAvailable (driver_name)) return true;
  if (why)
    {
      *why = QObject::tr ("The Qt MariaDB/MySQL driver (qsqlmysql) is not installed. Available drivers: %1")
        .arg (QSqlDatabase::drivers ().join (", "));
    }
  return false;
}

auto WsprSpotDatabase::testConnection (WsprDbSettings const& s) -> Result
{
  Result result;
  QString why;
  if (!driverAvailable (&why)) { result.message = why; return result; }
  if (s.host.trimmed ().isEmpty () || s.user.isEmpty ())
    {
      result.message = QObject::tr ("Enter the host and user name.");
      return result;
    }
  ScopedConnection guard {new_connection_name ("test")};
  auto db = make_db (s, guard.name, false);
  if (!db.open ())
    {
      result.message = QObject::tr ("Connection failed: %1").arg (with_hint (error_text (db.lastError ())));
      return result;
    }
  QStringList lines;
  QSqlQuery q {db};
  if (q.exec ("SELECT VERSION(), CURRENT_USER()") && q.next ())
    {
      lines << QObject::tr ("Connected to %1:%2 - server %3, logged in as %4")
        .arg (s.host).arg (s.port).arg (q.value (0).toString (), q.value (1).toString ());
    }
  if (!WsprDbSettings::validDatabaseName (s.database))
    {
      lines << QObject::tr ("Database name \"%1\" is not valid (letters, digits and _ only).").arg (s.database);
      result.ok = true;
      result.message = lines.join ('\n');
      return result;
    }
  q.prepare ("SELECT COUNT(*) FROM information_schema.SCHEMATA WHERE SCHEMA_NAME = ?");
  q.addBindValue (s.database);
  bool db_exists = q.exec () && q.next () && q.value (0).toInt () > 0;
  if (!db_exists)
    {
      lines << QObject::tr ("Database \"%1\" does not exist yet (or this user cannot see it) - "
                            "use \"Create Database & Tables\".").arg (s.database);
    }
  else
    {
      q.prepare ("SELECT TABLE_NAME FROM information_schema.TABLES WHERE TABLE_SCHEMA = ?");
      q.addBindValue (s.database);
      QSet<QString> present;
      if (q.exec ()) while (q.next ()) present << q.value (0).toString ().toLower ();
      QStringList missing;
      for (auto const& t : our_tables) if (!present.contains (t)) missing << t;
      if (missing.isEmpty ())
        {
          QString count = "?", last = "-";
          if (q.exec (QString {"SELECT COUNT(*), MAX(spot_time_utc) FROM `%1`.wspr_spots"}.arg (s.database)) && q.next ())
            {
              count = q.value (0).toString ();
              if (!q.value (1).isNull ()) last = q.value (1).toDateTime ().toString ("yyyy-MM-dd hh:mm") + " UTC";
            }
          lines << QObject::tr ("Database \"%1\" is ready: all tables present, %2 spots stored, latest %3.")
            .arg (s.database, count, last);
        }
      else
        {
          lines << QObject::tr ("Database \"%1\" exists but these tables are missing: %2 - "
                                "use \"Create Database & Tables\".").arg (s.database, missing.join (", "));
        }
    }
  result.ok = true;
  result.message = lines.join ('\n');
  return result;
}

auto WsprSpotDatabase::createDatabaseAndTables (WsprDbSettings const& s) -> Result
{
  Result result;
  QString why;
  if (!driverAvailable (&why)) { result.message = why; return result; }
  if (!WsprDbSettings::validDatabaseName (s.database))
    {
      result.message = QObject::tr ("Database name \"%1\" is not valid (letters, digits and _ only, up to 64).").arg (s.database);
      return result;
    }
  ScopedConnection guard {new_connection_name ("create")};
  auto db = make_db (s, guard.name, false);
  if (!db.open ())
    {
      result.message = QObject::tr ("Connection failed: %1").arg (with_hint (error_text (db.lastError ())));
      return result;
    }
  QStringList lines;
  QSqlQuery q {db};
  q.prepare ("SELECT COUNT(*) FROM information_schema.SCHEMATA WHERE SCHEMA_NAME = ?");
  q.addBindValue (s.database);
  bool existed = q.exec () && q.next () && q.value (0).toInt () > 0;
  if (!existed)
    {
      if (!q.exec (QString {"CREATE DATABASE IF NOT EXISTS `%1` CHARACTER SET utf8mb4"}.arg (s.database)))
        {
          result.message = QObject::tr ("Could not create database \"%1\": %2\n"
                                        "The user needs the CREATE privilege, or ask the database "
                                        "administrator to create it and grant this user ALL on it.")
            .arg (s.database, error_text (q.lastError ()));
          return result;
        }
      lines << QObject::tr ("Created database \"%1\".").arg (s.database);
    }
  else
    {
      lines << QObject::tr ("Database \"%1\" already exists.").arg (s.database);
    }
  if (!q.exec (QString {"USE `%1`"}.arg (s.database)))
    {
      result.message = QObject::tr ("Cannot use database \"%1\": %2").arg (s.database, error_text (q.lastError ()));
      return result;
    }
  QSet<QString> present;
  if (q.exec ("SHOW TABLES")) while (q.next ()) present << q.value (0).toString ().toLower ();
  for (auto const& sql : table_statements () + upgrade_statements ())   // DX-DBi v2R2: also bring older tables up to date
    {
      if (!q.exec (sql))
        {
          result.message = lines.join ('\n') + '\n'
            + QObject::tr ("Failed: %1\n%2").arg (error_text (q.lastError ()), sql.section ('(', 0, 0).simplified ());
          return result;
        }
    }
  {                                                       // DX-DBi v2R2: fill the new inserted_at columns of older rows
    QString why;                                          // reason if it fails
    if (!backfill_inserted_at (q, &why))                  // copy the old server time stamps into UTC and local columns
      {
        result.message = lines.join ('\n') + '\n' + QObject::tr ("Failed to fill inserted_at_utc / inserted_at_local: %1").arg (why); // report
        return result;                                    // stop here
      }
  }
  QStringList created, kept;
  for (auto const& t : our_tables) (present.contains (t) ? kept : created) << t;
  if (created.size ()) lines << QObject::tr ("Created tables: %1").arg (created.join (", "));
  if (kept.size ()) lines << QObject::tr ("Tables already present (left unchanged): %1").arg (kept.join (", "));
  lines << QObject::tr ("Ready to log spots to %1.").arg (s.describe ());
  result.ok = true;
  result.message = lines.join ('\n');
  return result;
}

//
// logger (runs in its own thread)
//
WsprSpotDatabase::WsprSpotDatabase (QObject * parent)
  : QObject {parent}
{
}

WsprSpotDatabase::~WsprSpotDatabase ()
{
  closeConnection ();
}

void WsprSpotDatabase::configure (WsprDbSettings const& s)
{
  bool changed = s.host != settings_.host || s.port != settings_.port || s.database != settings_.database
    || s.user != settings_.user || s.password != settings_.password || s.requireTls != settings_.requireTls;
  bool was_enabled = settings_.enabled;
  settings_ = s;
  if (changed) closeConnection ();
  last_failed_ = false;
  if (!settings_.enabled)
    {
      if (was_enabled && (pending_spots_.size () || pending_periods_.size ()))
        {
          Q_EMIT status (tr ("WSPR database logging off - %1 unsaved spots discarded").arg (pending_spots_.size ()), false);
        }
      pending_spots_.clear ();
      pending_periods_.clear ();
      pending_decodes_.clear ();               // DX-DBi v2R2: logging off, forget waiting decodes too
    }
  else if (pending_spots_.size () || pending_periods_.size ())
    {
      QString error;
      flush (&error);
    }
}

void WsprSpotDatabase::shutdown ()
{
  closeConnection ();
}

void WsprSpotDatabase::closeConnection ()
{
  if (!connection_name_.isEmpty ())
    {
      if (QSqlDatabase::contains (connection_name_))
        {
          {
            auto db = QSqlDatabase::database (connection_name_, false);
            if (db.isOpen ()) db.close ();
          }
          QSqlDatabase::removeDatabase (connection_name_);
        }
      connection_name_.clear ();
    }
  open_ = false;
  decodes_table_checked_ = false;              // DX-DBi v2R2: check / create the decodes table again on the next connection
}

bool WsprSpotDatabase::ensureOpen (QString * error)
{
  if (open_ && QSqlDatabase::contains (connection_name_)
      && QSqlDatabase::database (connection_name_, false).isOpen ())
    {
      return true;
    }
  closeConnection ();
  QString why;
  if (!driverAvailable (&why)) { *error = why; return false; }
  if (!settings_.complete ())
    {
      *error = tr ("settings incomplete (Database > WSPR Database Settings)");
      return false;
    }
  connection_name_ = new_connection_name ("log");
  auto db = make_db (settings_, connection_name_, true);
  if (!db.open ())
    {
      *error = error_text (db.lastError ());
      return false;
    }
  open_ = true;
  return true;
}

void WsprSpotDatabase::logSpots (QVector<WsprSpot> const& spots, WsprPeriod const& period)
{
  if (!settings_.enabled) return;
  pending_spots_ += spots;
  if (period.valid) pending_periods_ << period;
  if (pending_spots_.size () > max_pending_spots)
    {
      pending_spots_.remove (0, pending_spots_.size () - max_pending_spots);   // drop the oldest
    }
  if (pending_periods_.size () > max_pending_periods)
    {
      pending_periods_.remove (0, pending_periods_.size () - max_pending_periods);
    }
  QString error;
  auto spot_count = pending_spots_.size ();
  if (flush (&error))
    {
      if (spot_count || last_failed_)
        {
          if (last_inserted_ == spot_count)
            Q_EMIT status (tr ("WSPR database: %1 spot(s) saved").arg (spot_count), false);
          else
            Q_EMIT status (tr ("WSPR database: %1 new spot(s) saved, %2 already stored")
                           .arg (last_inserted_).arg (spot_count - last_inserted_), false);
        }
      last_failed_ = false;
    }
  else
    {
      Q_EMIT status (tr ("WSPR database error (%1 spots waiting): %2")
                     .arg (pending_spots_.size ()).arg (error), true);
      last_failed_ = true;
    }
}

bool WsprSpotDatabase::flush (QString * error)
{
  if (!pending_spots_.size () && !pending_periods_.size ()) return true;
  for (int attempt = 0; attempt < 2; ++attempt)   // second try after a dropped connection
    {
      error->clear ();                          // DX-DBi v2R2: report only this attempt's error
      if (!ensureOpen (error)) return false;
      bool ok;
      int inserted {0};
      {                         // db handle must be gone before closeConnection ()
      auto db = QSqlDatabase::database (connection_name_, false);
      ok = ensureSchema (error);                // DX-DBi v2R2: radio columns (and decodes table) present
      if (ok) ok = db.transaction ();           // DX-DBi v2R2: only start writing once the tables are up to date
      {
        QSqlQuery q {db};
        if (ok && pending_spots_.size ())
          {
            ok = q.prepare (R"(INSERT IGNORE INTO wspr_spots
 (spot_time_utc, mode, band, dial_freq_hz, freq_mhz, audio_freq_hz, snr_db, dt_s, drift_hz,
  sync_quality, message, message_type, tx_call, tx_grid, tx_power_dbm, rx_call, rx_grid,
  distance_km, azimuth_deg, decoder_cycles, jitter, block_size, decode_type, hard_errors,
  decoder_pass, fano_metric, from_file, program, radio, inserted_at_utc, inserted_at_local)
 VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?))");   // DX-DBi v2R2: 31 columns (radio, inserted_at_utc / _local added)
            for (int i = 0; ok && i < pending_spots_.size (); ++i)
              {
                auto const& s = pending_spots_[i];
                bool const nd = !s.hasDecoderDetail;
                q.addBindValue (s.periodStartUtc);
                q.addBindValue (s.mode);
                q.addBindValue (null_if (s.band.isEmpty (), s.band));
                q.addBindValue (s.dialFreqHz);
                q.addBindValue (QString::number (s.freqMHz, 'f', 7));
                q.addBindValue (QString::number ((s.freqMHz * 1e6 - s.dialFreqHz), 'f', 2));
                q.addBindValue (s.snrDb);
                q.addBindValue (QString::number (s.dtSec, 'f', 2));
                q.addBindValue (s.driftHz);
                q.addBindValue (null_if (!s.hasSync, QString::number (s.sync, 'f', 2)));
                q.addBindValue (s.message.left (40));
                q.addBindValue (null_if (!s.messageType, s.messageType));
                q.addBindValue (s.txCall.left (16));
                q.addBindValue (null_if (s.txGrid.isEmpty (), s.txGrid.left (6)));
                q.addBindValue (null_if (s.txPowerDbm == -999, s.txPowerDbm));
                q.addBindValue (s.rxCall.left (16));
                q.addBindValue (null_if (s.rxGrid.isEmpty (), s.rxGrid.left (6)));
                q.addBindValue (null_if (s.distanceKm < 0, s.distanceKm));
                q.addBindValue (null_if (s.azimuthDeg < 0, s.azimuthDeg));
                q.addBindValue (null_if (nd, s.cycles));
                q.addBindValue (null_if (nd, s.jitter));
                q.addBindValue (null_if (nd, s.blockSize));
                q.addBindValue (null_if (nd, s.decodeType));
                q.addBindValue (null_if (nd, s.hardErrors));
                q.addBindValue (null_if (nd, s.pass));
                q.addBindValue (null_if (nd, s.metric));
                q.addBindValue (s.fromFile ? 1 : 0);
                q.addBindValue (QCoreApplication::applicationName () + ' ' + QCoreApplication::applicationVersion ());
                q.addBindValue (null_if (s.radio.isEmpty (), s.radio.left (40)));   // DX-DBi v2R2: radio
                q.addBindValue (utc_text (s.insertedAtUtc));                        // DX-DBi v2R2: inserted_at_utc
                q.addBindValue (local_text (s.insertedAtUtc));                      // DX-DBi v2R2: inserted_at_local
                ok = q.exec ();
                if (ok && q.numRowsAffected () > 0) inserted += q.numRowsAffected ();
              }
          }
        if (ok && pending_periods_.size ())
          {
            ok = q.prepare (R"(INSERT IGNORE INTO wspr_rx_periods
 (period_start_utc, mode, band, dial_freq_hz, rx_call, rx_grid, decodes, from_file, radio, inserted_at_utc, inserted_at_local)
 VALUES (?,?,?,?,?,?,?,?,?,?,?))");         // DX-DBi v2R2: 11 columns (radio, inserted_at_utc / _local added)
            for (int i = 0; ok && i < pending_periods_.size (); ++i)
              {
                auto const& p = pending_periods_[i];
                q.addBindValue (p.startUtc);
                q.addBindValue (p.mode);
                q.addBindValue (null_if (p.band.isEmpty (), p.band));
                q.addBindValue (p.dialFreqHz);
                q.addBindValue (p.rxCall.left (16));
                q.addBindValue (null_if (p.rxGrid.isEmpty (), p.rxGrid.left (6)));
                q.addBindValue (p.decodes);
                q.addBindValue (p.fromFile ? 1 : 0);
                q.addBindValue (null_if (p.radio.isEmpty (), p.radio.left (40)));   // DX-DBi v2R2: radio
                q.addBindValue (utc_text (p.insertedAtUtc));                        // DX-DBi v2R2: inserted_at_utc
                q.addBindValue (local_text (p.insertedAtUtc));                      // DX-DBi v2R2: inserted_at_local
                ok = q.exec ();
              }
          }
        if (!ok && error->isEmpty ())           // DX-DBi v2R2: keep the message from ensureSchema () when that failed
          {
            *error = q.lastError ().isValid () && q.lastError ().type () != QSqlError::NoError
              ? error_text (q.lastError ()) : error_text (db.lastError ());
          }
      }
      if (ok)
        {
          ok = db.commit ();
          if (!ok) *error = error_text (db.lastError ());
        }
      if (!ok) db.rollback ();
      }
      if (ok)
        {
          last_inserted_ = inserted;
          pending_spots_.clear ();
          pending_periods_.clear ();
          return true;
        }
      // table missing or similar is not going to fix itself; a lost
      // connection might, so reconnect once and try again
      bool const connection_problem = error->contains ("gone away", Qt::CaseInsensitive)
        || error->contains ("Lost connection", Qt::CaseInsensitive)
        || error->contains ("Can't connect", Qt::CaseInsensitive);
      closeConnection ();
      if (!connection_problem) break;
    }
  if (error->contains ("doesn't exist", Qt::CaseInsensitive))
    {
      *error += ' ' + tr ("- use Database > WSPR Database Settings > Create Database & Tables");
    }
  return false;
}

//
// DX-DBi v2R2: decodes of all other modes (FT8, FT4, JT65, JT9, Q65, MSK144, FST4 ...)
//
void WsprSpotDatabase::logDecodes (QVector<DecodeRecord> const& decodes)
{
  if (!settings_.enabled || decodes.isEmpty ()) return;  // MariaDB logging off, or nothing to write
  pending_decodes_ += decodes;                           // add them to the waiting list
  if (pending_decodes_.size () > max_pending_spots)      // too many waiting (server down for a long time)?
    {
      pending_decodes_.remove (0, pending_decodes_.size () - max_pending_spots); // drop the oldest
    }
  auto const count = pending_decodes_.size ();           // how many we are about to write
  int inserted {0};                                      // how many were really new
  QString error;                                         // reason if the write fails
  if (flushDecodes (&inserted, &error))                  // write them
    {
      if (inserted == count)                             // all new
        Q_EMIT status (tr ("Database: %1 decode(s) saved").arg (count), false);
      else                                               // some were already stored
        Q_EMIT status (tr ("Database: %1 new decode(s) saved, %2 already stored").arg (inserted).arg (count - inserted), false);
    }
  else                                                   // could not write: keep them for the next period
    {
      Q_EMIT status (tr ("Database error (%1 decodes waiting): %2").arg (pending_decodes_.size ()).arg (error), true);
    }
}

bool WsprSpotDatabase::flushDecodes (int * inserted, QString * error)
{
  *inserted = 0;                                         // nothing written yet
  if (pending_decodes_.isEmpty ()) return true;          // nothing to write
  for (int attempt = 0; attempt < 2; ++attempt)          // second try after a dropped connection
    {
      error->clear ();                                   // report only this attempt's error
      if (!ensureOpen (error)) return false;             // connect (or reuse the open connection)
      bool ok {true};                                    // becomes false on the first failure
      {                                                  // inner block: handle and query must be gone before closeConnection ()
        auto db = QSqlDatabase::database (connection_name_, false); // the open connection
        QSqlQuery q {db};                                // statement runner
        ok = ensureSchema (error);                       // decodes table and radio columns present (checked once per connection)
        if (ok) ok = db.transaction ();                  // write everything as one unit
        if (ok)
          {
            ok = q.prepare (R"(INSERT IGNORE INTO decodes
 (decode_time_utc, mode, band, dial_freq_hz, audio_freq_hz, snr_db, dt_s, message, to_call, de_call,
  de_grid, rx_call, rx_grid, distance_km, azimuth_deg, low_confidence, from_file, program, radio,
  inserted_at_utc, inserted_at_local)
 VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?))");  // 21 columns; INSERT IGNORE skips decodes already stored
            for (int i = 0; ok && i < pending_decodes_.size (); ++i) // one insert per decode
              {
                auto const& d = pending_decodes_[i];     // the decode being written
                q.addBindValue (d.timeUtc);              // decode_time_utc
                q.addBindValue (d.mode.left (12));       // mode
                q.addBindValue (null_if (d.band.isEmpty (), d.band.left (8)));          // band
                q.addBindValue (d.dialFreqHz);           // dial_freq_hz
                q.addBindValue (d.audioFreqHz);          // audio_freq_hz
                q.addBindValue (d.snrDb);                // snr_db
                q.addBindValue (QString::number (d.dtSec, 'f', 2));                     // dt_s
                q.addBindValue (d.message.left (64));    // message
                q.addBindValue (null_if (d.toCall.isEmpty (), d.toCall.left (16)));     // to_call
                q.addBindValue (null_if (d.deCall.isEmpty (), d.deCall.left (16)));     // de_call
                q.addBindValue (null_if (d.deGrid.isEmpty (), d.deGrid.left (6)));      // de_grid
                q.addBindValue (d.rxCall.left (16));     // rx_call
                q.addBindValue (null_if (d.rxGrid.isEmpty (), d.rxGrid.left (6)));      // rx_grid
                q.addBindValue (null_if (d.distanceKm < 0, d.distanceKm));              // distance_km
                q.addBindValue (null_if (d.azimuthDeg < 0, d.azimuthDeg));              // azimuth_deg
                q.addBindValue (d.lowConfidence ? 1 : 0); // low_confidence
                q.addBindValue (d.fromFile ? 1 : 0);     // from_file
                q.addBindValue (QCoreApplication::applicationName () + ' ' + QCoreApplication::applicationVersion ()); // program
                q.addBindValue (null_if (d.radio.isEmpty (), d.radio.left (40)));        // radio
                q.addBindValue (utc_text (d.insertedAtUtc));                             // inserted_at_utc
                q.addBindValue (local_text (d.insertedAtUtc));                           // inserted_at_local
                ok = q.exec ();                          // run the insert
                if (ok && q.numRowsAffected () > 0) *inserted += q.numRowsAffected (); // count new rows
              }
            if (!ok) *error = error_text (q.lastError ()); // report the failing statement
            if (ok)                                      // everything accepted
              {
                ok = db.commit ();                       // make it permanent
                if (!ok) *error = error_text (db.lastError ()); // report a failed commit
              }
            if (!ok) db.rollback ();                     // undo a partly written batch
          }
        else if (error->isEmpty ())                      // the transaction could not start
          {
            *error = error_text (db.lastError ());       // report why
          }
      }
      if (ok)                                            // batch stored
        {
          pending_decodes_.clear ();                     // nothing waiting any more
          return true;                                   // success
        }
      *inserted = 0;                                     // rolled back: nothing was stored
      bool const connection_problem = error->contains ("gone away", Qt::CaseInsensitive)
        || error->contains ("Lost connection", Qt::CaseInsensitive)
        || error->contains ("Can't connect", Qt::CaseInsensitive); // a lost connection might come back
      closeConnection ();                                // start fresh next time
      if (!connection_problem) break;                    // other errors will not fix themselves: stop
    }
  return false;                                          // keep the decodes for the next period
}

// DX-DBi v2R2: once per connection, bring an existing database up to date - create the decodes table and
// add the radio column to tables made by older versions. Needs the CREATE and ALTER privileges.
bool WsprSpotDatabase::ensureSchema (QString * error)
{
  if (decodes_table_checked_) return true;               // already done on this connection
  auto db = QSqlDatabase::database (connection_name_, false); // the open connection
  QSqlQuery q {db};                                      // statement runner
  for (auto const& sql : upgrade_statements ())          // each upgrade step (all safe to repeat)
    {
      if (!q.exec (sql))                                 // run it
        {
          *error = error_text (q.lastError ()) + ' '
            + tr ("- could not update the tables (the user needs the CREATE and ALTER privileges); "
                  "or use Database > WSPR Database Settings > Create Database & Tables"); // explain what to do
          return false;                                  // do not write until the tables are right
        }
    }
  if (!backfill_inserted_at (q, error))                  // older rows: fill inserted_at_utc / inserted_at_local from inserted_at
    {
      *error += ' ' + tr ("- could not fill the new inserted_at columns of older rows (the user needs the UPDATE privilege)"); // explain
      return false;                                      // do not write until the tables are right
    }
  decodes_table_checked_ = true;                         // do not check again on this connection
  return true;                                           // tables are up to date
}
