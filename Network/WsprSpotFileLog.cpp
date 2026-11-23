// DX-DBi v2R1: log WSPR / FST4W spots to a CSV file and / or a Microsoft Access file
// (see WsprSpotFileLog.hpp)

#include "WsprSpotFileLog.hpp"                  // our own declarations

#include <QSettings>                            // reading and writing the settings file
#include <QSqlDatabase>                         // Qt SQL connection (used through ODBC for Access)
#include <QSqlQuery>                            // running SQL statements
#include <QSqlError>                            // SQL error text
#include <QSqlRecord>                           // DX-DBi v2R2: column list of an existing table
#include <QTimer>                               // DX-DBi v2R3: closes the Access file after a quiet minute
#include <QVariant>                             // values bound to SQL statements
#include <QFile>                                // the CSV file
#include <QFileInfo>                            // file existence and folder
#include <QDir>                                 // folder creation and native path form
#include <QTextStream>                          // writing text lines to the CSV file
#include <QCoreApplication>                     // program name and version for the "program" column
#include <QAtomicInt>                           // counter for unique connection names
#include <QStringList>                          // lists of text
#include <string>                               // std::wstring for the Windows ODBC calls
#include <cwchar>                               // wcslen for walking the driver name list

#ifdef Q_OS_WIN                                 // the Access part only exists on Windows
#include <windows.h>                            // Windows base types
#include <odbcinst.h>                           // ODBC installer API: list drivers, create a database file
#endif                                          // end of Windows-only includes

#include "moc_WsprSpotFileLog.cpp"              // Qt generated signal / slot code for this class

namespace                                       // helpers private to this file
{
  QAtomicInt connection_counter {0};            // makes each connection name unique
  int const max_pending_spots {20000};          // keep at most this many unsaved spots per file
  int const max_pending_periods {2000};         // keep at most this many unsaved receive periods
  char const * const settings_group = "WSJTX-PHE-WsprFileLog";                  // settings file section
  char const * const access_driver = "Microsoft Access Driver (*.mdb, *.accdb)"; // name of the ODBC driver

  // column names, shared by the CSV header and (in the same order) the Access table
  char const * const csv_header =
    "spot_time_utc,mode,band,dial_freq_hz,freq_mhz,audio_freq_hz,snr_db,dt_s,drift_hz,sync_quality,"
    "message,message_type,tx_call,tx_grid,tx_power_dbm,rx_call,rx_grid,distance_km,azimuth_deg,"
    "decoder_cycles,jitter,block_size,decode_type,hard_errors,decoder_pass,fano_metric,from_file,"
    "program,radio,inserted_at_utc,inserted_at_local"; // DX-DBi v2R2: radio and the two logging-time columns

  // DX-DBi v2R2: column names of the decodes CSV (decodes of all other modes)
  char const * const csv_decodes_header =
    "decode_time_utc,mode,band,dial_freq_hz,audio_freq_hz,snr_db,dt_s,message,to_call,de_call,de_grid,"
    "rx_call,rx_grid,distance_km,azimuth_deg,low_confidence,from_file,program,radio,inserted_at_utc,inserted_at_local";

  // DX-DBi v2R2: before appending, make sure an existing CSV has the same columns. A file written by an
  // older version (no radio column) is renamed to "<name>_until_<date_time>.csv" and a new file is started,
  // so the columns of every line always match the header. Returns true when the header must be written.
  // DX-DBi v2R3: *refused is set (and nothing is touched) when the file exists but is not one of our CSV logs.
  // v2R2 renamed ANY file with a different header - in one setup the CSV location had been set to WSJT-X's
  // own lotw-user-activity.csv, which was renamed away and replaced by a spot log.
  bool prepare_csv (QString const& path, QString const& header, QString * note, QString * refused)
  {
    QFileInfo const info {path};                // the CSV file
    if (!info.exists () || 0 == info.size ()) return true;                // new or empty file: write the header
    QFile file {path};                          // open it to read the first line
    if (!file.open (QIODevice::ReadOnly | QIODevice::Text)) return false; // cannot read it: just append (the write reports any error)
    auto const first = QString::fromUtf8 (file.readLine ()).trimmed ();  // its header line
    file.close ();                              // done reading
    if (first == header) return false;          // same columns: keep appending
    if (first.section (',', 0, 0) != header.section (',', 0, 0))          // DX-DBi v2R3: first column differs - not our log at all
      {
        *refused = QObject::tr ("%1 is not a DX-DBi log file (its first line is \"%2\"); it was left untouched - "
                                "choose another file under Database > CSV File Location")
          .arg (QDir::toNativeSeparators (path), first.left (40));       // say which file and why
        return false;                           // caller writes nothing
      }
    auto const old_name = info.absoluteDir ().absoluteFilePath (info.completeBaseName () + "_until_"
                          + QDateTime::currentDateTimeUtc ().toString ("yyyyMMdd_HHmmss") + '.' + info.suffix ()); // new name for the old file
    if (QFile::rename (path, old_name))         // keep the old file under its new name
      {
        *note = QObject::tr ("older CSV kept as %1").arg (QDir::toNativeSeparators (old_name)); // tell the user
        return true;                            // start the new file with the header
      }
    return false;                               // could not rename (open in Excel?): append and let the write report it
  }

  // one CSV field: quoted when it holds a comma, a quote or a line break
  QString csv_field (QString const& text)
  {
    if (!text.contains (',') && !text.contains ('"') && !text.contains ('\n') && !text.contains ('\r'))
      {
        return text;                            // plain text needs no quoting
      }
    QString quoted {text};                      // copy we can change
    quoted.replace ("\"", "\"\"");              // a quote inside a field is written twice
    return "\"" + quoted + "\"";                // wrap the field in quotes
  }

  // a date and time as text, e.g. 2026-11-14 18:02:00
  QString time_text (QDateTime const& when)
  {
    return when.toString ("yyyy-MM-dd HH:mm:ss"); // ISO style, sorts correctly as text
  }

  // a date and time without milliseconds (the Access ODBC driver rejects fractions of a second)
  QDateTime whole_seconds (QDateTime const& when)
  {
    auto const& t = when.time ();               // the time of day part
    return QDateTime {when.date (), QTime {t.hour (), t.minute (), t.second ()}, Qt::UTC}; // same moment, no ms
  }

  // the program that logged the spot, e.g. "WSJT-X 3.0.2-devel" (same text as the MariaDB logger)
  QString program_text ()
  {
    return QCoreApplication::applicationName () + ' ' + QCoreApplication::applicationVersion (); // name and version
  }

  // values for SQL binding that are either a number / text or a typed NULL (ODBC needs the type)
  QVariant int_or_null (bool is_null, int value)
  {
    return is_null ? QVariant {QVariant::Int} : QVariant {value};          // NULL of integer type, or the number
  }
  QVariant double_or_null (bool is_null, double value)
  {
    return is_null ? QVariant {QVariant::Double} : QVariant {value};       // NULL of floating type, or the number
  }
  QVariant text_or_null (bool is_null, QString const& value)
  {
    return is_null ? QVariant {QVariant::String} : QVariant {value};       // NULL of text type, or the text
  }

  QString error_text (QSqlError const& e)       // readable text of an SQL error
  {
    auto t = e.text ().trimmed ();              // driver and database messages
    return t.isEmpty () ? QString {"unknown error"} : t; // never return an empty message
  }

  // the Access tables; names and meaning match the MariaDB tables of v2R0
  QStringList access_table_statements (QString const& table)
  {
    if (table == "wspr_spots")                  // the spots table
      {
        return {
          "CREATE TABLE wspr_spots ("
          "[id] COUNTER PRIMARY KEY, [spot_time_utc] DATETIME NOT NULL, [mode] VARCHAR(10) NOT NULL, "
          "[band] VARCHAR(8), [dial_freq_hz] DOUBLE, [freq_mhz] DOUBLE NOT NULL, [audio_freq_hz] DOUBLE, "
          "[snr_db] INTEGER, [dt_s] DOUBLE, [drift_hz] INTEGER, [sync_quality] DOUBLE, "
          "[message] VARCHAR(40), [message_type] INTEGER, [tx_call] VARCHAR(16) NOT NULL, "
          "[tx_grid] VARCHAR(6), [tx_power_dbm] INTEGER, [rx_call] VARCHAR(16) NOT NULL, "
          "[rx_grid] VARCHAR(6), [distance_km] INTEGER, [azimuth_deg] INTEGER, [decoder_cycles] INTEGER, "
          "[jitter] INTEGER, [block_size] INTEGER, [decode_type] INTEGER, [hard_errors] INTEGER, "
          "[decoder_pass] INTEGER, [fano_metric] DOUBLE, [from_file] INTEGER, [program] VARCHAR(40), "
          "[radio] VARCHAR(40), [inserted_at_utc] DATETIME, [inserted_at_local] DATETIME)", // one row per decoded spot (radio: DX-DBi v2R2)
          "CREATE UNIQUE INDEX uq_spot ON wspr_spots "
          "([spot_time_utc], [mode], [rx_call], [tx_call], [freq_mhz])", // the same spot is never stored twice
          "CREATE INDEX ix_time ON wspr_spots ([spot_time_utc])",        // fast look-up by time
          "CREATE INDEX ix_tx_call ON wspr_spots ([tx_call])",           // fast look-up by transmitting station
        };
      }
    if (table == "decodes")                     // DX-DBi v2R2: decodes of all other modes
      {
        return {
          "CREATE TABLE decodes ("
          "[id] COUNTER PRIMARY KEY, [decode_time_utc] DATETIME NOT NULL, [mode] VARCHAR(12) NOT NULL, "
          "[band] VARCHAR(8), [dial_freq_hz] DOUBLE, [audio_freq_hz] INTEGER NOT NULL, [snr_db] INTEGER, "
          "[dt_s] DOUBLE, [message] VARCHAR(64) NOT NULL, [to_call] VARCHAR(16), [de_call] VARCHAR(16), "
          "[de_grid] VARCHAR(6), [rx_call] VARCHAR(16) NOT NULL, [rx_grid] VARCHAR(6), [distance_km] INTEGER, "
          "[azimuth_deg] INTEGER, [low_confidence] INTEGER, [from_file] INTEGER, [program] VARCHAR(40), "
          "[radio] VARCHAR(40), [inserted_at_utc] DATETIME, [inserted_at_local] DATETIME)", // one row per decode
          "CREATE UNIQUE INDEX uq_decode ON decodes "
          "([decode_time_utc], [mode], [rx_call], [audio_freq_hz], [message])", // the same decode is never stored twice
          "CREATE INDEX ix_dtime ON decodes ([decode_time_utc])",            // fast look-up by time
          "CREATE INDEX ix_de_call ON decodes ([de_call])",                   // fast look-up by sending station
        };
      }
    return {                                    // otherwise: the receive periods table
      "CREATE TABLE wspr_rx_periods ("
      "[id] COUNTER PRIMARY KEY, [period_start_utc] DATETIME NOT NULL, [mode] VARCHAR(10) NOT NULL, "
      "[band] VARCHAR(8), [dial_freq_hz] DOUBLE NOT NULL, [rx_call] VARCHAR(16) NOT NULL, "
      "[rx_grid] VARCHAR(6), [decodes] INTEGER, [from_file] INTEGER, [radio] VARCHAR(40), [inserted_at_utc] DATETIME, [inserted_at_local] DATETIME)", // one row per receive period (radio: v2R2)
      "CREATE UNIQUE INDEX uq_period ON wspr_rx_periods "
      "([period_start_utc], [mode], [rx_call], [dial_freq_hz])",         // the same period is never stored twice
    };
  }

  // create an empty Access database file with the ODBC installer API
  bool create_access_file (QString const& path, QString * error)
  {
#ifdef Q_OS_WIN
    auto const native = QDir::toNativeSeparators (path);                 // Windows style path with backslashes
    // CREATE_DBV4 makes an Access 2000-2003 .mdb, CREATE_DBV12 an Access 2007+ .accdb
    QString const keyword {path.endsWith (".mdb", Qt::CaseInsensitive) ? "CREATE_DBV4" : "CREATE_DBV12"};
    std::wstring attributes = QString {keyword + "=\"" + native + "\" General"}.toStdWString (); // attribute list text
    attributes.push_back (L'\0');               // the list must end with two zero characters
    attributes.push_back (L'\0');               // second terminating zero
    auto const driver = QString {access_driver}.toStdWString ();          // driver name as wide text
    if (SQLConfigDataSourceW (nullptr, ODBC_ADD_DSN, driver.c_str (), attributes.c_str ())) // ask the driver to create the file
      {
        return true;                            // file created
      }
    DWORD code {0};                             // installer error number
    wchar_t text[512] = L"";                    // installer error text
    WORD length {0};                            // length of the error text
    SQLInstallerErrorW (1, &code, text, 512, &length);                   // fetch the reason for the failure
    *error = QObject::tr ("Could not create the Access file %1: %2").arg (native, QString::fromWCharArray (text)); // report it
    return false;                               // creation failed
#else
    Q_UNUSED (path);                            // not used off Windows
    *error = QObject::tr ("Microsoft Access files are only supported on Windows."); // explain why
    return false;                               // cannot create the file here
#endif
  }
}

//
// settings
//
WsprFileLogSettings WsprFileLogSettings::load (QSettings * settings, QString const& default_folder)
{
  WsprFileLogSettings s;                        // start from the defaults (both logs off)
  QDir const folder {default_folder};           // folder used when no file has been chosen yet
  settings->beginGroup (settings_group);        // our own section of the settings file
  s.csvEnabled = settings->value ("CsvEnabled", s.csvEnabled).toBool ();                                    // CSV log on / off
  s.csvPath = settings->value ("CsvPath", folder.absoluteFilePath ("wspr_spots.csv")).toString ();          // CSV file location
  s.accessEnabled = settings->value ("AccessEnabled", s.accessEnabled).toBool ();                           // Access log on / off
  s.accessPath = settings->value ("AccessPath", folder.absoluteFilePath ("wspr_spots.accdb")).toString ();  // Access file location
  settings->endGroup ();                        // leave our section
  return s;                                     // hand the settings back
}

void WsprFileLogSettings::save (QSettings * settings) const
{
  settings->beginGroup (settings_group);        // our own section of the settings file
  settings->setValue ("CsvEnabled", csvEnabled);        // CSV log on / off
  settings->setValue ("CsvPath", csvPath);              // CSV file location
  settings->setValue ("AccessEnabled", accessEnabled);  // Access log on / off
  settings->setValue ("AccessPath", accessPath);        // Access file location
  settings->endGroup ();                        // leave our section
}

// DX-DBi v2R2: decodes of all other modes go to "decodes_all_modes.csv" in the same folder as the WSPR CSV
QString WsprFileLogSettings::csvDecodesPath () const
{
  return QFileInfo {csvPath}.absoluteDir ().absoluteFilePath ("decodes_all_modes.csv"); // same folder, fixed name
}

//
// logger (runs in the WSPR database worker thread)
//
WsprSpotFileLog::WsprSpotFileLog (QObject * parent)
  : QObject {parent}                            // pass the parent on to QObject
  , access_idle_timer_ {new QTimer {this}}      // DX-DBi v2R3: child object, so it moves to the worker thread with us
{
  // DX-DBi v2R3: opening and closing the Access file costs about a second each time (measured with
  // a replay test), so it now stays open between batches and is closed after a minute without writes.
  // That still releases the file soon after logging stops, and on shutdown / settings change at once.
  access_idle_timer_->setSingleShot (true);     // one shot, restarted after every batch
  access_idle_timer_->setInterval (60 * 1000);  // a quiet minute
  connect (access_idle_timer_, &QTimer::timeout, this, &WsprSpotFileLog::closeAccess); // then release the file
}

// DX-DBi v2R3: called at the end of every Access batch, in the worker thread
void WsprSpotFileLog::accessBatchDone (bool ok)
{
  if (ok) access_idle_timer_->start ();         // success: keep the file open, close it after a quiet minute
  else                                          // failure: start again from a fresh connection next time
    {
      access_idle_timer_->stop ();              // no idle close pending
      closeAccess ();                           // release the file now
    }
}

WsprSpotFileLog::~WsprSpotFileLog ()
{
  closeAccess ();                               // make sure the Access file is released
}

bool WsprSpotFileLog::accessDriverAvailable (QString * why)
{
  if (!QSqlDatabase::isDriverAvailable ("QODBC"))        // Qt's ODBC plugin (qsqlodbc) must be present
    {
      if (why) *why = QObject::tr ("The Qt ODBC driver (qsqlodbc) is not installed with this program."); // explain
      return false;                             // cannot use Access
    }
#ifdef Q_OS_WIN
  wchar_t names[16384] = L"";                   // receives the list of installed ODBC drivers
  WORD used {0};                                // number of characters returned
  if (SQLGetInstalledDriversW (names, 16384, &used))     // ask Windows for the driver list
    {
      for (wchar_t const * p = names; *p; p += wcslen (p) + 1) // names are separated by zero characters
        {
          if (QString::fromWCharArray (p) == access_driver) return true; // the Access driver is installed
        }
    }
  if (why) *why = QObject::tr ("The 64-bit \"%1\" is not installed on this computer. It comes with 64-bit "
                               "Microsoft Access / Office, or with the free Microsoft Access Database Engine.")
             .arg (access_driver);              // explain what is missing and where to get it
  return false;                                 // driver not found
#else
  if (why) *why = QObject::tr ("Microsoft Access files are only supported on Windows."); // explain
  return false;                                 // no Access off Windows
#endif
}

void WsprSpotFileLog::configure (WsprFileLogSettings const& s)
{
  bool const access_changed = s.accessPath != settings_.accessPath || s.accessEnabled != settings_.accessEnabled; // file or switch changed?
  settings_ = s;                                // remember the new settings
  if (access_changed) closeAccess ();           // release the old Access file
  if (!settings_.csvEnabled)                    // CSV log off: nothing left to write there
    {
      csv_pending_.clear ();                    // forget waiting spots
      csv_pending_decodes_.clear ();            // DX-DBi v2R2: forget waiting decodes
    }
  if (!settings_.accessEnabled)                 // Access log off: nothing left to write there
    {
      access_pending_spots_.clear ();           // forget waiting spots
      access_pending_periods_.clear ();         // forget waiting periods
      access_pending_decodes_.clear ();         // DX-DBi v2R2: forget waiting decodes
    }
}

void WsprSpotFileLog::shutdown ()
{
  access_idle_timer_->stop ();                  // DX-DBi v2R3: stop the idle timer in its own thread before the thread ends
  closeAccess ();                               // release the Access file before the thread ends
}

void WsprSpotFileLog::closeAccess ()
{
  if (!access_connection_.isEmpty ())           // only when a connection was made
    {
      if (QSqlDatabase::contains (access_connection_))  // is it still registered with Qt?
        {
          {                                     // inner block: the handle must be gone before removal
            auto db = QSqlDatabase::database (access_connection_, false); // get the handle without opening it
            if (db.isOpen ()) db.close ();      // close the file
          }
          QSqlDatabase::removeDatabase (access_connection_); // drop the connection
        }
      access_connection_.clear ();              // mark as closed
    }
}

bool WsprSpotFileLog::openAccess (QString * error)
{
  if (!access_connection_.isEmpty () && QSqlDatabase::contains (access_connection_)
      && QSqlDatabase::database (access_connection_, false).isOpen ())
    {
      return true;                              // already open
    }
  closeAccess ();                               // clear any half-open state
  QString why;                                  // reason when the driver is missing
  if (!accessDriverAvailable (&why)) { *error = why; return false; }   // no driver: give up with the reason
  if (settings_.accessPath.trimmed ().isEmpty ())                      // no file chosen
    {
      *error = tr ("no Access file chosen (Database > Access File Location)"); // say what to do
      return false;                             // cannot open
    }
  QFileInfo const info {settings_.accessPath};  // details of the chosen file
  if (!info.exists ())                          // first use: the file has to be created
    {
      QDir {}.mkpath (info.absolutePath ());    // make sure the folder exists
      if (!create_access_file (info.absoluteFilePath (), error)) return false; // create the empty database
    }
  access_connection_ = QString {"wsprphe_access_%1"}.arg (connection_counter.fetchAndAddRelaxed (1)); // unique name
  bool ok {true};                               // becomes false on the first failure
  {                                             // inner block: handle and query must be gone before closeAccess ()
    auto db = QSqlDatabase::addDatabase ("QODBC", access_connection_); // new ODBC connection
    // DX-DBi v2R2: Exclusive=0 - always open the file shared, so Microsoft Access can have it open at the same time
    db.setDatabaseName (QString {"Driver={%1};DBQ=%2;Exclusive=0;"}
                        .arg (access_driver, QDir::toNativeSeparators (info.absoluteFilePath ()))); // connect straight to the file
    if (!db.open ())                            // open the file
      {
        *error = error_text (db.lastError ());  // report why it failed
        ok = false;                             // remember the failure
      }
    else                                        // file is open: make sure our tables are there
      {
        QStringList present;                    // table names already in the file, lower case
        for (auto const& name : db.tables ()) present << name.toLower (); // collect them
        QSqlQuery q {db};                       // statement runner
        for (auto const& table : QStringList {"wspr_spots", "wspr_rx_periods", "decodes"}) // our tables (decodes: DX-DBi v2R2)
          {
            if (present.contains (table))       // already there
              {
                // DX-DBi v2R2: a file made by an older version lacks the radio and the two logging-time columns
                auto const columns = db.record (table);                // the table's columns as they are now
                QStringList const wanted {"radio", "inserted_at_utc", "inserted_at_local"}; // columns added in v2R2
                QStringList const types {"VARCHAR(40)", "DATETIME", "DATETIME"};          // their Access types
                for (int c = 0; ok && c < wanted.size (); ++c) // each new column
                  {
                    if (columns.contains (wanted[c])) continue;        // already there
                    if (!q.exec (QString {"ALTER TABLE [%1] ADD COLUMN [%2] %3"}.arg (table, wanted[c], types[c]))) // add it
                      {
                        *error = tr ("could not add column %1 to %2: %3").arg (wanted[c], table, error_text (q.lastError ())); // report
                        ok = false;             // remember the failure
                      }
                  }
                // older rows: fill the two new columns from the old UTC "inserted_at" column (kept as it is)
                if (ok && columns.contains ("inserted_at"))
                  {
                    if (!q.exec (QString {"UPDATE [%1] SET [inserted_at_utc] = [inserted_at] "
                                          "WHERE [inserted_at_utc] IS NULL"}.arg (table)))      // UTC copy
                      {
                        *error = tr ("could not fill inserted_at_utc in %1: %2").arg (table, error_text (q.lastError ())); // report
                        ok = false;             // remember the failure
                      }
                    QSqlQuery rows {db};        // second query: the rows still without a local time
                    if (ok && rows.exec (QString {"SELECT [id], [inserted_at] FROM [%1] "
                                                  "WHERE [inserted_at_local] IS NULL AND [inserted_at] IS NOT NULL"}.arg (table)))
                      {
                        QSqlQuery set {db};     // third query: writes the local time
                        set.prepare (QString {"UPDATE [%1] SET [inserted_at_local] = ? WHERE [id] = ?"}.arg (table)); // one row at a time
                        while (ok && rows.next ())                   // each older row
                          {
                            auto const t = rows.value (1).toDateTime (); // its old UTC time as stored
                            QDateTime const utc {t.date (), t.time (), Qt::UTC}; // read it as UTC
                            set.addBindValue (utc.toLocalTime ());   // the same moment in local time
                            set.addBindValue (rows.value (0));       // which row
                            if (!set.exec ())                        // write it
                              {
                                *error = tr ("could not fill inserted_at_local in %1: %2").arg (table, error_text (set.lastError ())); // report
                                ok = false;     // remember the failure
                              }
                          }
                      }
                  }
                continue;                       // otherwise leave the table alone
              }
            for (auto const& sql : access_table_statements (table))    // table plus its indexes
              {
                if (ok && !q.exec (sql))        // create it
                  {
                    *error = tr ("could not create table %1: %2").arg (table, error_text (q.lastError ())); // report
                    ok = false;                 // remember the failure
                  }
              }
          }
      }
  }
  if (!ok) closeAccess ();                      // failed: release the file again
  return ok;                                    // true when the file is open and ready
}

void WsprSpotFileLog::logSpots (QVector<WsprSpot> const& spots, WsprPeriod const& period)
{
  if (!settings_.anyEnabled ()) return;         // both file logs are off
  QStringList saved;                            // what was written, for the status bar
  QStringList failed;                           // what went wrong, for the status bar
  if (settings_.csvEnabled)                     // CSV log is on
    {
      csv_pending_ += spots;                    // add the new spots to the waiting list
      if (csv_pending_.size () > max_pending_spots)                   // too many waiting?
        {
          csv_pending_.remove (0, csv_pending_.size () - max_pending_spots); // drop the oldest
        }
      auto const count = csv_pending_.size ();  // how many we are about to write
      QString error;                            // reason if the write fails
      if (writeCsv (&error))                    // append them to the file
        {
          if (count) saved << tr ("%1 to CSV").arg (count);            // report success
        }
      else                                      // could not write (e.g. file open in Excel)
        {
          failed << tr ("CSV (%1 spots waiting): %2").arg (csv_pending_.size ()).arg (error); // report, keep the spots
        }
    }
  if (settings_.accessEnabled)                  // Access log is on
    {
      access_pending_spots_ += spots;           // add the new spots to the waiting list
      if (period.valid) access_pending_periods_ << period;             // add the receive period too
      if (access_pending_spots_.size () > max_pending_spots)           // too many waiting?
        {
          access_pending_spots_.remove (0, access_pending_spots_.size () - max_pending_spots); // drop the oldest
        }
      if (access_pending_periods_.size () > max_pending_periods)       // too many periods waiting?
        {
          access_pending_periods_.remove (0, access_pending_periods_.size () - max_pending_periods); // drop the oldest
        }
      auto const count = access_pending_spots_.size ();                // how many we are about to write
      int inserted {0};                         // how many were really new
      QString error;                            // reason if the write fails
      if (writeAccess (&inserted, &error))      // insert them into the file
        {
          if (count && inserted == count) saved << tr ("%1 to Access").arg (count);   // all new
          else if (count) saved << tr ("%1 to Access (%2 already stored)").arg (inserted).arg (count - inserted); // some duplicates
        }
      else                                      // could not write
        {
          failed << tr ("Access (%1 spots waiting): %2").arg (access_pending_spots_.size ()).arg (error); // report, keep the spots
        }
    }
  if (failed.size ())                           // something went wrong
    {
      Q_EMIT status (tr ("WSPR file log error - %1").arg (failed.join ("; ")), true);  // show it as an error
    }
  else if (saved.size ())                       // everything written
    {
      Q_EMIT status (tr ("WSPR file log: spot(s) saved - %1").arg (saved.join (", ")), false); // show what was saved
    }
}

bool WsprSpotFileLog::writeCsv (QString * error)
{
  if (csv_pending_.isEmpty ()) return true;     // nothing to write
  if (settings_.csvPath.trimmed ().isEmpty ())  // no file chosen
    {
      *error = tr ("no CSV file chosen (Database > CSV File Location)"); // say what to do
      return false;                             // cannot write
    }
  QFileInfo const info {settings_.csvPath};     // details of the chosen file
  QDir {}.mkpath (info.absolutePath ());        // make sure the folder exists
  QString note;                                 // DX-DBi v2R2: set when an older file was renamed
  QString refused;                              // DX-DBi v2R3: set when the file is not one of our logs
  bool const fresh = prepare_csv (info.absoluteFilePath (), csv_header, &note, &refused); // header needed? (old-format file renamed)
  if (refused.size ()) { *error = refused; return false; }                       // DX-DBi v2R3: never write into someone else's file
  if (note.size ()) Q_EMIT status (tr ("WSPR CSV: %1").arg (note), false);       // tell the user where the old file went
  QFile file {info.absoluteFilePath ()};        // the CSV file
  if (!file.open (QIODevice::Append | QIODevice::Text))                // open for adding lines (CR LF line ends on Windows)
    {
      *error = file.errorString ();             // e.g. the file is open in Excel
      return false;                             // keep the spots for next time
    }
  QTextStream out {&file};                      // text writer on the file
  out.setCodec ("UTF-8");                       // write UTF-8 text
  if (fresh) out << csv_header << '\n';         // first line: the column names
  auto const program = program_text ();         // which program wrote them
  for (auto const& s : csv_pending_)            // one line per spot
    {
      bool const nd = !s.hasDecoderDetail;      // true when the wsprd detail columns are unknown
      QStringList f;                            // the fields of this line, in header order
      f << time_text (s.periodStartUtc);        // spot_time_utc
      f << csv_field (s.mode);                  // mode
      f << csv_field (s.band);                  // band
      f << QString::number (s.dialFreqHz);      // dial_freq_hz
      f << QString::number (s.freqMHz, 'f', 7); // freq_mhz
      f << QString::number (s.freqMHz * 1e6 - s.dialFreqHz, 'f', 2);   // audio_freq_hz
      f << QString::number (s.snrDb);           // snr_db
      f << QString::number (s.dtSec, 'f', 2);   // dt_s
      f << QString::number (s.driftHz);         // drift_hz
      f << (s.hasSync ? QString::number (s.sync, 'f', 2) : QString {}); // sync_quality (blank when unknown)
      f << csv_field (s.message);               // message
      f << (s.messageType ? QString::number (s.messageType) : QString {}); // message_type (blank when unknown)
      f << csv_field (s.txCall);                // tx_call
      f << csv_field (s.txGrid);                // tx_grid
      f << (s.txPowerDbm == -999 ? QString {} : QString::number (s.txPowerDbm)); // tx_power_dbm (blank when unknown)
      f << csv_field (s.rxCall);                // rx_call
      f << csv_field (s.rxGrid);                // rx_grid
      f << (s.distanceKm < 0 ? QString {} : QString::number (s.distanceKm));     // distance_km (blank when unknown)
      f << (s.azimuthDeg < 0 ? QString {} : QString::number (s.azimuthDeg));     // azimuth_deg (blank when unknown)
      f << (nd ? QString {} : QString::number (s.cycles));      // decoder_cycles
      f << (nd ? QString {} : QString::number (s.jitter));      // jitter
      f << (nd ? QString {} : QString::number (s.blockSize));   // block_size
      f << (nd ? QString {} : QString::number (s.decodeType));  // decode_type
      f << (nd ? QString {} : QString::number (s.hardErrors));  // hard_errors
      f << (nd ? QString {} : QString::number (s.pass));        // decoder_pass
      f << (nd ? QString {} : QString::number (s.metric));      // fano_metric
      f << QString::number (s.fromFile ? 1 : 0);                // from_file
      f << csv_field (program);                 // program
      f << csv_field (s.radio);                 // DX-DBi v2R2: radio
      f << time_text (insertedAtOrNow (s.insertedAtUtc));                // inserted_at_utc (same stamp as MariaDB / Access)
      f << time_text (insertedAtOrNow (s.insertedAtUtc).toLocalTime ()); // inserted_at_local
      out << f.join (',') << '\n';              // write the line
    }
  out.flush ();                                 // push everything to the file
  if (QTextStream::Ok != out.status () || QFileDevice::NoError != file.error ()) // did the write work?
    {
      *error = file.errorString ();             // e.g. disk full
      return false;                             // keep the spots for next time
    }
  csv_pending_.clear ();                        // all written
  return true;                                  // success
}

bool WsprSpotFileLog::writeAccess (int * inserted, QString * error)
{
  *inserted = 0;                                // nothing inserted yet
  if (access_pending_spots_.isEmpty () && access_pending_periods_.isEmpty ()) return true; // nothing to write
  if (!openAccess (error)) return false;        // open (or create) the file and tables
  bool ok {true};                               // becomes false on a real failure
  {                                             // inner block: handle and query must be gone before closeAccess ()
    auto db = QSqlDatabase::database (access_connection_, false);      // the open connection
    auto const program = program_text ();       // value for the program column
    ok = db.transaction ();                     // write everything as one unit
    if (!ok) *error = error_text (db.lastError ());                    // report if the transaction could not start
    QSqlQuery q {db};                           // statement runner
    if (ok && access_pending_spots_.size ())    // spots to write
      {
        QString const spot_sql {"INSERT INTO wspr_spots ([spot_time_utc], [mode], [band], [dial_freq_hz], [freq_mhz], "
                        "[audio_freq_hz], [snr_db], [dt_s], [drift_hz], [sync_quality], [message], [message_type], "
                        "[tx_call], [tx_grid], [tx_power_dbm], [rx_call], [rx_grid], [distance_km], [azimuth_deg], "
                        "[decoder_cycles], [jitter], [block_size], [decode_type], [hard_errors], [decoder_pass], "
                        "[fano_metric], [from_file], [program], [radio], [inserted_at_utc], [inserted_at_local]) "
                        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"}; // 31 columns, 31 values (v2R2)
        ok = q.prepare (spot_sql);                                   // check the statement once
        if (!ok) *error = error_text (q.lastError ());                 // report a statement the driver rejects
        for (int i = 0; ok && i < access_pending_spots_.size (); ++i)  // one insert per spot
          {
            auto const& s = access_pending_spots_[i];                  // the spot being written
            if (i && !(ok = q.prepare (spot_sql)))                     // DX-DBi v2R2: fresh statement per row (Access driver crash)
              {
                *error = error_text (q.lastError ());                  // report why
                break;                                                 // stop and roll back
              }
            bool const nd = !s.hasDecoderDetail;                       // true when the wsprd detail columns are unknown
            q.addBindValue (whole_seconds (s.periodStartUtc));         // spot_time_utc
            q.addBindValue (s.mode.left (10));                         // mode
            q.addBindValue (text_or_null (s.band.isEmpty (), s.band.left (8)));           // band
            q.addBindValue (static_cast<double> (s.dialFreqHz));       // dial_freq_hz (stored as a double)
            q.addBindValue (s.freqMHz);                                // freq_mhz
            q.addBindValue (s.freqMHz * 1e6 - s.dialFreqHz);           // audio_freq_hz
            q.addBindValue (s.snrDb);                                  // snr_db
            q.addBindValue (s.dtSec);                                  // dt_s
            q.addBindValue (s.driftHz);                                // drift_hz
            q.addBindValue (double_or_null (!s.hasSync, s.sync));      // sync_quality
            q.addBindValue (s.message.left (40));                      // message
            q.addBindValue (int_or_null (!s.messageType, s.messageType));                 // message_type
            q.addBindValue (s.txCall.left (16));                       // tx_call
            q.addBindValue (text_or_null (s.txGrid.isEmpty (), s.txGrid.left (6)));       // tx_grid
            q.addBindValue (int_or_null (s.txPowerDbm == -999, s.txPowerDbm));            // tx_power_dbm
            q.addBindValue (s.rxCall.left (16));                       // rx_call
            q.addBindValue (text_or_null (s.rxGrid.isEmpty (), s.rxGrid.left (6)));       // rx_grid
            q.addBindValue (int_or_null (s.distanceKm < 0, s.distanceKm));                // distance_km
            q.addBindValue (int_or_null (s.azimuthDeg < 0, s.azimuthDeg));                // azimuth_deg
            q.addBindValue (int_or_null (nd, s.cycles));               // decoder_cycles
            q.addBindValue (int_or_null (nd, s.jitter));               // jitter
            q.addBindValue (int_or_null (nd, s.blockSize));            // block_size
            q.addBindValue (int_or_null (nd, s.decodeType));           // decode_type
            q.addBindValue (int_or_null (nd, s.hardErrors));           // hard_errors
            q.addBindValue (int_or_null (nd, s.pass));                 // decoder_pass
            q.addBindValue (double_or_null (nd, static_cast<double> (s.metric)));         // fano_metric
            q.addBindValue (s.fromFile ? 1 : 0);                       // from_file
            q.addBindValue (program.left (40));                        // program
            q.addBindValue (text_or_null (s.radio.isEmpty (), s.radio.left (40)));         // radio (DX-DBi v2R2)
            q.addBindValue (insertedAtOrNow (s.insertedAtUtc));        // inserted_at_utc (same stamp in every log)
            q.addBindValue (insertedAtOrNow (s.insertedAtUtc).toLocalTime ()); // inserted_at_local
            if (q.exec ())                                             // run the insert
              {
                ++*inserted;                                           // a new row was stored
              }
            else if (!error_text (q.lastError ()).contains ("duplicate", Qt::CaseInsensitive)) // not just "already stored"?
              {
                *error = error_text (q.lastError ());                  // a real error: report it
                ok = false;                                            // stop and roll back
              }
          }
      }
    if (ok && access_pending_periods_.size ())  // receive periods to write
      {
        QString const period_sql {"INSERT INTO wspr_rx_periods ([period_start_utc], [mode], [band], [dial_freq_hz], "
                        "[rx_call], [rx_grid], [decodes], [from_file], [radio], [inserted_at_utc], [inserted_at_local]) "
                        "VALUES (?,?,?,?,?,?,?,?,?,?,?)"};             // 11 columns, 11 values (v2R2)
        ok = q.prepare (period_sql);                                 // check the statement once
        if (!ok) *error = error_text (q.lastError ());                 // report a statement the driver rejects
        for (int i = 0; ok && i < access_pending_periods_.size (); ++i) // one insert per period
          {
            auto const& p = access_pending_periods_[i];                // the period being written
            if (i && !(ok = q.prepare (period_sql)))                   // DX-DBi v2R2: fresh statement per row (Access driver crash)
              {
                *error = error_text (q.lastError ());                  // report why
                break;                                                 // stop and roll back
              }
            q.addBindValue (whole_seconds (p.startUtc));               // period_start_utc
            q.addBindValue (p.mode.left (10));                         // mode
            q.addBindValue (text_or_null (p.band.isEmpty (), p.band.left (8)));           // band
            q.addBindValue (static_cast<double> (p.dialFreqHz));       // dial_freq_hz (stored as a double)
            q.addBindValue (p.rxCall.left (16));                       // rx_call
            q.addBindValue (text_or_null (p.rxGrid.isEmpty (), p.rxGrid.left (6)));       // rx_grid
            q.addBindValue (p.decodes);                                // decodes
            q.addBindValue (p.fromFile ? 1 : 0);                       // from_file
            q.addBindValue (text_or_null (p.radio.isEmpty (), p.radio.left (40)));         // radio (DX-DBi v2R2)
            q.addBindValue (insertedAtOrNow (p.insertedAtUtc));        // inserted_at_utc (same stamp in every log)
            q.addBindValue (insertedAtOrNow (p.insertedAtUtc).toLocalTime ()); // inserted_at_local
            if (!q.exec () && !error_text (q.lastError ()).contains ("duplicate", Qt::CaseInsensitive)) // real error?
              {
                *error = error_text (q.lastError ());                  // report it
                ok = false;                                            // stop and roll back
              }
          }
      }
    if (ok)                                     // everything accepted
      {
        ok = db.commit ();                      // make it permanent
        if (!ok) *error = error_text (db.lastError ());                // report a failed commit
      }
    if (!ok) db.rollback ();                    // undo a partly written batch
  }
  // DX-DBi v2R3: keep the file open between batches (the v2R2 close-after-every-batch was aimed at a crash
  // whose real cause was the re-used INSERT, fixed in v2R2); closed after a quiet minute or on failure
  accessBatchDone (ok);                         // restart the idle timer, or close after a failure
  if (ok)                                       // batch stored
    {
      access_pending_spots_.clear ();           // nothing waiting any more
      access_pending_periods_.clear ();         // nothing waiting any more
      return true;                              // success
    }
  *inserted = 0;                                // rolled back: nothing was stored
  return false;                                 // keep the spots for next time
}

//
// DX-DBi v2R2: decodes of all other modes (FT8, FT4, JT65, JT9, Q65, MSK144, FST4 ...)
//
void WsprSpotFileLog::logDecodes (QVector<DecodeRecord> const& decodes)
{
  if (!settings_.anyEnabled () || decodes.isEmpty ()) return;          // both file logs off, or nothing to write
  QStringList saved;                            // what was written, for the status bar
  QStringList failed;                           // what went wrong, for the status bar
  if (settings_.csvEnabled)                     // CSV log is on
    {
      csv_pending_decodes_ += decodes;          // add the new decodes to the waiting list
      if (csv_pending_decodes_.size () > max_pending_spots)            // too many waiting?
        {
          csv_pending_decodes_.remove (0, csv_pending_decodes_.size () - max_pending_spots); // drop the oldest
        }
      auto const count = csv_pending_decodes_.size ();                 // how many we are about to write
      QString error;                            // reason if the write fails
      if (writeCsvDecodes (&error))             // append them to the file
        {
          saved << tr ("%1 to CSV").arg (count);                       // report success
        }
      else                                      // could not write (e.g. file open in Excel)
        {
          failed << tr ("CSV (%1 decodes waiting): %2").arg (csv_pending_decodes_.size ()).arg (error); // report, keep them
        }
    }
  if (settings_.accessEnabled)                  // Access log is on
    {
      access_pending_decodes_ += decodes;       // add the new decodes to the waiting list
      if (access_pending_decodes_.size () > max_pending_spots)         // too many waiting?
        {
          access_pending_decodes_.remove (0, access_pending_decodes_.size () - max_pending_spots); // drop the oldest
        }
      auto const count = access_pending_decodes_.size ();              // how many we are about to write
      int inserted {0};                         // how many were really new
      QString error;                            // reason if the write fails
      if (writeAccessDecodes (&inserted, &error)) // insert them into the file
        {
          if (inserted == count) saved << tr ("%1 to Access").arg (count); // all new
          else saved << tr ("%1 to Access (%2 already stored)").arg (inserted).arg (count - inserted); // some duplicates
        }
      else                                      // could not write
        {
          failed << tr ("Access (%1 decodes waiting): %2").arg (access_pending_decodes_.size ()).arg (error); // report, keep them
        }
    }
  if (failed.size ())                           // something went wrong
    {
      Q_EMIT status (tr ("Decode file log error - %1").arg (failed.join ("; ")), true);   // show it as an error
    }
  else if (saved.size ())                       // everything written
    {
      Q_EMIT status (tr ("Decode file log: decode(s) saved - %1").arg (saved.join (", ")), false); // show what was saved
    }
}

bool WsprSpotFileLog::writeCsvDecodes (QString * error)
{
  if (csv_pending_decodes_.isEmpty ()) return true;                    // nothing to write
  if (settings_.csvPath.trimmed ().isEmpty ())  // no CSV location chosen, so no folder for the decodes file
    {
      *error = tr ("no CSV file chosen (Database > CSV File Location)"); // say what to do
      return false;                             // cannot write
    }
  QFileInfo const info {settings_.csvDecodesPath ()};                  // the decodes CSV file
  QDir {}.mkpath (info.absolutePath ());        // make sure the folder exists
  QString note;                                 // set when an older file was renamed
  QString refused;                              // DX-DBi v2R3: set when the file is not one of our logs
  bool const fresh = prepare_csv (info.absoluteFilePath (), csv_decodes_header, &note, &refused); // header needed? (old-format file renamed)
  if (refused.size ()) { *error = refused; return false; }                       // DX-DBi v2R3: never write into someone else's file
  if (note.size ()) Q_EMIT status (tr ("Decodes CSV: %1").arg (note), false);    // tell the user where the old file went
  QFile file {info.absoluteFilePath ()};        // the file
  if (!file.open (QIODevice::Append | QIODevice::Text))                // open for adding lines
    {
      *error = file.errorString ();             // e.g. the file is open in Excel
      return false;                             // keep the decodes for next time
    }
  QTextStream out {&file};                      // text writer on the file
  out.setCodec ("UTF-8");                       // write UTF-8 text
  if (fresh) out << csv_decodes_header << '\n'; // first line: the column names
  auto const program = program_text ();         // which program wrote them
  for (auto const& d : csv_pending_decodes_)    // one line per decode
    {
      QStringList f;                            // the fields of this line, in header order
      f << time_text (d.timeUtc);               // decode_time_utc
      f << csv_field (d.mode);                  // mode
      f << csv_field (d.band);                  // band
      f << QString::number (d.dialFreqHz);      // dial_freq_hz
      f << QString::number (d.audioFreqHz);     // audio_freq_hz
      f << QString::number (d.snrDb);           // snr_db
      f << QString::number (d.dtSec, 'f', 2);   // dt_s
      f << csv_field (d.message);               // message
      f << csv_field (d.toCall);                // to_call
      f << csv_field (d.deCall);                // de_call
      f << csv_field (d.deGrid);                // de_grid
      f << csv_field (d.rxCall);                // rx_call
      f << csv_field (d.rxGrid);                // rx_grid
      f << (d.distanceKm < 0 ? QString {} : QString::number (d.distanceKm)); // distance_km (blank when unknown)
      f << (d.azimuthDeg < 0 ? QString {} : QString::number (d.azimuthDeg)); // azimuth_deg (blank when unknown)
      f << QString::number (d.lowConfidence ? 1 : 0);                  // low_confidence
      f << QString::number (d.fromFile ? 1 : 0);                       // from_file
      f << csv_field (program);                 // program
      f << csv_field (d.radio);                 // radio
      f << time_text (insertedAtOrNow (d.insertedAtUtc));                // inserted_at_utc (same stamp as MariaDB / Access)
      f << time_text (insertedAtOrNow (d.insertedAtUtc).toLocalTime ()); // inserted_at_local
      out << f.join (',') << '\n';              // write the line
    }
  out.flush ();                                 // push everything to the file
  if (QTextStream::Ok != out.status () || QFileDevice::NoError != file.error ()) // did the write work?
    {
      *error = file.errorString ();             // e.g. disk full
      return false;                             // keep the decodes for next time
    }
  csv_pending_decodes_.clear ();                // all written
  return true;                                  // success
}

bool WsprSpotFileLog::writeAccessDecodes (int * inserted, QString * error)
{
  *inserted = 0;                                // nothing inserted yet
  if (access_pending_decodes_.isEmpty ()) return true;                 // nothing to write
  if (!openAccess (error)) return false;        // open (or create) the file and tables
  bool ok {true};                               // becomes false on a real failure
  {                                             // inner block: handle and query must be gone before closeAccess ()
    auto db = QSqlDatabase::database (access_connection_, false);      // the open connection
    auto const program = program_text ();       // value for the program column
    ok = db.transaction ();                     // write everything as one unit
    if (!ok) *error = error_text (db.lastError ());                    // report if the transaction could not start
    QSqlQuery q {db};                           // statement runner
    QString decode_sql;                         // the INSERT text, prepared again for every row
    if (ok)
      {
        decode_sql = "INSERT INTO decodes ([decode_time_utc], [mode], [band], [dial_freq_hz], [audio_freq_hz], "
                        "[snr_db], [dt_s], [message], [to_call], [de_call], [de_grid], [rx_call], [rx_grid], "
                        "[distance_km], [azimuth_deg], [low_confidence], [from_file], [program], [radio], [inserted_at_utc], [inserted_at_local]) "
                        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";  // 21 columns, 21 values
        ok = q.prepare (decode_sql);                                 // check the statement once
        if (!ok) *error = error_text (q.lastError ());                 // report a statement the driver rejects
      }
    for (int i = 0; ok && i < access_pending_decodes_.size (); ++i)    // one insert per decode
      {
        auto const& d = access_pending_decodes_[i];                    // the decode being written
        if (i && !(ok = q.prepare (decode_sql)))                       // DX-DBi v2R2: fresh statement per row (Access driver crash)
          {
            *error = error_text (q.lastError ());                      // report why
            break;                                                     // stop and roll back
          }
        q.addBindValue (whole_seconds (d.timeUtc));                    // decode_time_utc
        q.addBindValue (d.mode.left (12));                             // mode
        q.addBindValue (text_or_null (d.band.isEmpty (), d.band.left (8)));            // band
        q.addBindValue (static_cast<double> (d.dialFreqHz));           // dial_freq_hz (stored as a double)
        q.addBindValue (d.audioFreqHz);                                // audio_freq_hz
        q.addBindValue (d.snrDb);                                      // snr_db
        q.addBindValue (d.dtSec);                                      // dt_s
        q.addBindValue (d.message.left (64));                          // message
        q.addBindValue (text_or_null (d.toCall.isEmpty (), d.toCall.left (16)));       // to_call
        q.addBindValue (text_or_null (d.deCall.isEmpty (), d.deCall.left (16)));       // de_call
        q.addBindValue (text_or_null (d.deGrid.isEmpty (), d.deGrid.left (6)));        // de_grid
        q.addBindValue (d.rxCall.left (16));                           // rx_call
        q.addBindValue (text_or_null (d.rxGrid.isEmpty (), d.rxGrid.left (6)));        // rx_grid
        q.addBindValue (int_or_null (d.distanceKm < 0, d.distanceKm)); // distance_km
        q.addBindValue (int_or_null (d.azimuthDeg < 0, d.azimuthDeg)); // azimuth_deg
        q.addBindValue (d.lowConfidence ? 1 : 0);                      // low_confidence
        q.addBindValue (d.fromFile ? 1 : 0);                           // from_file
        q.addBindValue (program.left (40));                            // program
        q.addBindValue (text_or_null (d.radio.isEmpty (), d.radio.left (40)));         // radio
        q.addBindValue (insertedAtOrNow (d.insertedAtUtc));            // inserted_at_utc (same stamp in every log)
        q.addBindValue (insertedAtOrNow (d.insertedAtUtc).toLocalTime ()); // inserted_at_local
        if (q.exec ())                                                 // run the insert
          {
            ++*inserted;                                               // a new row was stored
          }
        else if (!error_text (q.lastError ()).contains ("duplicate", Qt::CaseInsensitive)) // not just "already stored"?
          {
            *error = error_text (q.lastError ());                      // a real error: report it
            ok = false;                                                // stop and roll back
          }
      }
    if (ok)                                     // everything accepted
      {
        ok = db.commit ();                      // make it permanent
        if (!ok) *error = error_text (db.lastError ());                // report a failed commit
      }
    if (!ok && db.isOpen ()) db.rollback ();    // undo a partly written batch
  }
  accessBatchDone (ok);                         // DX-DBi v2R3: keep the file open (idle close after a minute), or close after a failure
  if (ok)                                       // batch stored
    {
      access_pending_decodes_.clear ();         // nothing waiting any more
      return true;                              // success
    }
  *inserted = 0;                                // rolled back: nothing was stored
  return false;                                 // keep the decodes for next time
}
