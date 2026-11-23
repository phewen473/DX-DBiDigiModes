#ifndef WSPR_DB_DIALOG_HPP_
#define WSPR_DB_DIALOG_HPP_

// DX-DBi v2R0: Database > WSPR Database Settings...
// host, port, database, user, password; Test Connection; Create Database & Tables

#include <QDialog>
#include <QFutureWatcher>

#include "Network/WsprSpotDatabase.hpp"

class QLineEdit;
class QSpinBox;
class QCheckBox;
class QPlainTextEdit;
class QPushButton;
class QDialogButtonBox;
class QLabel;

class WsprDbDialog final
  : public QDialog
{
  Q_OBJECT

public:
  WsprDbDialog (WsprDbSettings const& current, QWidget * parent = nullptr);
  ~WsprDbDialog ();

  WsprDbSettings settings () const;   // what the user entered

private:
  void run (bool create);
  void finished ();
  void setBusy (bool);
  void show_result (bool ok, QString const&);

  QCheckBox * enabled_;
  QCheckBox * log_file_decodes_;
  QLineEdit * host_;
  QSpinBox * port_;
  QLineEdit * database_;
  QLineEdit * user_;
  QLineEdit * password_;
  QCheckBox * show_password_;
  QCheckBox * remember_password_;
  QCheckBox * require_tls_;
  QPushButton * test_button_;
  QPushButton * create_button_;
  QDialogButtonBox * buttons_;
  QLabel * busy_label_;
  QPlainTextEdit * results_;
  QFutureWatcher<WsprSpotDatabase::Result> watcher_;
  bool busy_ {false};
};

#endif
