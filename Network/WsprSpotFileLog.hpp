#ifndef WSPR_SPOT_FILE_LOG_HPP_                 // include guard: only read this header once
#define WSPR_SPOT_FILE_LOG_HPP_                 // include guard marker

// DX-DBi v2R1: log WSPR / FST4W spots to a CSV file and / or a Microsoft Access
// database file (.accdb or .mdb), as they are decoded. No database server is needed.
//
// WsprSpotFileLog runs in the same worker thread as WsprSpotDatabase, so a locked
// file (for example the CSV open in Excel) never stalls the GUI. Spots that cannot
// be written are kept (up to a limit) and retried with the next receive period.
// It receives exactly the same WsprSpot / WsprPeriod data as the MariaDB logger.

#include <QObject>                              // base class for the logger object
#include <QString>                              // text values
#include <QVector>                              // lists of spots and periods

#include "Network/WsprSpotDatabase.hpp"         // WsprSpot and WsprPeriod are defined there

class QSettings;                                // forward declaration: settings store
class QTimer;                                   // DX-DBi v2R3: forward declaration: idle-close timer

// what the user has switched on, and where the files are
struct WsprFileLogSettings
{
  bool csvEnabled {false};                      // write each spot to the CSV file
  QString csvPath;                              // full path of the CSV file
  bool accessEnabled {false};                   // write each spot to the Access database file
  QString accessPath;                           // full path of the .accdb / .mdb file

  static WsprFileLogSettings load (QSettings *, QString const& default_folder); // read from the settings file
  void save (QSettings *) const;                // write to the settings file
  bool anyEnabled () const { return csvEnabled || accessEnabled; } // true when either file log is on
  QString csvDecodesPath () const;              // DX-DBi v2R2: CSV file for decodes of all other modes (next to the WSPR CSV)
};

class WsprSpotFileLog final                     // the logger; lives in the WSPR database worker thread
  : public QObject                              // QObject so it can have slots, signals and a thread
{
  Q_OBJECT                                      // Qt meta-object support (signals and slots)

public:
  explicit WsprSpotFileLog (QObject * parent = nullptr); // constructor
  ~WsprSpotFileLog ();                          // destructor: closes the Access file

  // true when the Qt ODBC plugin and the Microsoft Access ODBC driver are both installed
  static bool accessDriverAvailable (QString * why = nullptr);

  Q_SLOT void configure (WsprFileLogSettings const&);                 // take new settings
  Q_SLOT void logSpots (QVector<WsprSpot> const&, WsprPeriod const&); // write spots of one receive period
  Q_SLOT void logDecodes (QVector<DecodeRecord> const&);              // DX-DBi v2R2: write decodes of all other modes
  Q_SLOT void shutdown ();                                            // close files before the thread stops

  Q_SIGNAL void status (QString const& message, bool error);          // text for the main window status bar

private:
  bool writeCsv (QString * error);              // append the waiting spots to the CSV file
  bool writeAccess (int * inserted, QString * error); // insert the waiting spots into the Access file
  bool openAccess (QString * error);            // open (and if needed create) the Access file and its tables
  void closeAccess ();                          // close the Access connection

  WsprFileLogSettings settings_;                // current settings
  QString access_connection_;                   // name of the open Qt SQL connection, empty when closed
  QVector<WsprSpot> csv_pending_;               // spots waiting to be written to the CSV file
  QVector<WsprSpot> access_pending_spots_;      // spots waiting to be written to the Access file
  QVector<WsprPeriod> access_pending_periods_;  // receive periods waiting to be written to the Access file
  bool writeCsvDecodes (QString * error);       // DX-DBi v2R2: append the waiting decodes to the decodes CSV file
  bool writeAccessDecodes (int * inserted, QString * error); // DX-DBi v2R2: insert the waiting decodes into the Access file
  QVector<DecodeRecord> csv_pending_decodes_;   // DX-DBi v2R2: decodes waiting for the CSV file
  QVector<DecodeRecord> access_pending_decodes_; // DX-DBi v2R2: decodes waiting for the Access file
  void accessBatchDone (bool ok);               // DX-DBi v2R3: after a batch keep the file open (ok) or close it (failed)
  QTimer * access_idle_timer_;                  // DX-DBi v2R3: closes the Access file after a quiet minute
};

#endif                                          // end of include guard
