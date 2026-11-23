// DX-DBi v2R0: WSPR spot database settings dialog (see WsprDbDialog.hpp)

#include "WsprDbDialog.hpp"

#include <QFormLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QLineEdit>
#include <QSpinBox>
#include <QCheckBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QLabel>
#include <QRegularExpressionValidator>
#include <QtConcurrent/QtConcurrentRun>
#include <QApplication>

#include "moc_WsprDbDialog.cpp"

WsprDbDialog::WsprDbDialog (WsprDbSettings const& s, QWidget * parent)
  : QDialog {parent}
{
  setWindowTitle (tr ("WSPR Database Settings"));
  setWindowFlags (windowFlags () & ~Qt::WindowContextHelpButtonHint);

  auto intro = new QLabel {tr ("Saves every WSPR and FST4W spot this station decodes, and each "
                               "receive period (including periods with no decodes), to a MariaDB "
                               "or MySQL database.")};
  intro->setWordWrap (true);

  // connection
  auto server_box = new QGroupBox {tr ("Server")};
  auto form = new QFormLayout {server_box};
  host_ = new QLineEdit {s.host};
  host_->setPlaceholderText ("localhost, a host name or 192.168.x.x");
  port_ = new QSpinBox;
  port_->setRange (1, 65535);
  port_->setValue (s.port);
  database_ = new QLineEdit {s.database};
  database_->setValidator (new QRegularExpressionValidator {QRegularExpression {"[A-Za-z0-9_]{1,64}"}, database_});
  user_ = new QLineEdit {s.user};
  password_ = new QLineEdit {s.password};
  password_->setEchoMode (QLineEdit::Password);
  show_password_ = new QCheckBox {tr ("Show")};
  auto pw_row = new QHBoxLayout;
  pw_row->addWidget (password_, 1);
  pw_row->addWidget (show_password_);
  remember_password_ = new QCheckBox {tr ("Remember password (encrypted for this Windows user)")};
  remember_password_->setChecked (s.rememberPassword);
  require_tls_ = new QCheckBox {tr ("Require verified TLS (server must have a trusted certificate)")};
  require_tls_->setChecked (s.requireTls);
  require_tls_->setToolTip (tr ("Leave unticked for a MariaDB server on your own network without TLS. "
                                "TLS is still used when the server offers it, but its certificate is not checked."));
  form->addRow (tr ("Host:"), host_);
  form->addRow (tr ("Port:"), port_);
  form->addRow (tr ("Database:"), database_);
  form->addRow (tr ("User name:"), user_);
  form->addRow (tr ("Password:"), pw_row);
  form->addRow (QString {}, remember_password_);
  form->addRow (QString {}, require_tls_);

  // logging
  auto log_box = new QGroupBox {tr ("Logging")};
  auto log_layout = new QVBoxLayout {log_box};
  enabled_ = new QCheckBox {tr ("Log WSPR / FST4W spots to this database")};
  enabled_->setChecked (s.enabled);
  log_file_decodes_ = new QCheckBox {tr ("Also log decodes of recordings opened with File > Open")};
  log_file_decodes_->setChecked (s.logFileDecodes);
  log_layout->addWidget (enabled_);
  log_layout->addWidget (log_file_decodes_);

  // actions
  test_button_ = new QPushButton {tr ("&Test Connection")};
  create_button_ = new QPushButton {tr ("&Create Database && Tables")};
  create_button_->setToolTip (tr ("Creates the database if needed and the tables wspr_spots, "
                                   "wspr_rx_periods and wsprphe_schema if they are missing. "
                                   "Existing tables and data are never changed."));
  busy_label_ = new QLabel;
  auto action_row = new QHBoxLayout;
  action_row->addWidget (test_button_);
  action_row->addWidget (create_button_);
  action_row->addWidget (busy_label_, 1);

  results_ = new QPlainTextEdit;
  results_->setReadOnly (true);
  results_->setMinimumHeight (110);
  results_->setPlaceholderText (tr ("Results of Test Connection / Create appear here."));

  buttons_ = new QDialogButtonBox {QDialogButtonBox::Save | QDialogButtonBox::Cancel};

  auto layout = new QVBoxLayout {this};
  layout->addWidget (intro);
  layout->addWidget (server_box);
  layout->addWidget (log_box);
  layout->addLayout (action_row);
  layout->addWidget (results_, 1);
  layout->addWidget (buttons_);
  resize (520, sizeHint ().height ());

  connect (show_password_, &QCheckBox::toggled, this, [this] (bool on) {
      password_->setEchoMode (on ? QLineEdit::Normal : QLineEdit::Password);
    });
  connect (test_button_, &QPushButton::clicked, this, [this] { run (false); });
  connect (create_button_, &QPushButton::clicked, this, [this] { run (true); });
  connect (&watcher_, &QFutureWatcher<WsprSpotDatabase::Result>::finished, this, &WsprDbDialog::finished);
  connect (buttons_, &QDialogButtonBox::accepted, this, [this] {
      auto const& s = settings ();
      if (s.enabled && !s.complete ())
        {
          show_result (false, tr ("To turn logging on, enter the host, a user name and a valid "
                                  "database name (letters, digits and _)."));
          return;
        }
      accept ();
    });
  connect (buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);

  QString why;
  if (!WsprSpotDatabase::driverAvailable (&why))
    {
      show_result (false, why);
    }
}

WsprDbDialog::~WsprDbDialog ()
{
  if (busy_) QApplication::restoreOverrideCursor ();
  // a test still running finishes on its own; it holds only a copy of the settings
}

WsprDbSettings WsprDbDialog::settings () const
{
  WsprDbSettings s;
  s.enabled = enabled_->isChecked ();
  s.logFileDecodes = log_file_decodes_->isChecked ();
  s.host = host_->text ().trimmed ();
  s.port = port_->value ();
  s.database = database_->text ().trimmed ();
  s.user = user_->text ().trimmed ();
  s.password = password_->text ();
  s.rememberPassword = remember_password_->isChecked ();
  s.requireTls = require_tls_->isChecked ();
  return s;
}

void WsprDbDialog::run (bool create)
{
  if (busy_) return;
  auto const s = settings ();
  setBusy (true);
  busy_label_->setText (create ? tr ("Creating...") : tr ("Connecting to %1...").arg (s.host));
  if (create)
    {
      watcher_.setFuture (QtConcurrent::run ([s] { return WsprSpotDatabase::createDatabaseAndTables (s); }));
    }
  else
    {
      watcher_.setFuture (QtConcurrent::run ([s] { return WsprSpotDatabase::testConnection (s); }));
    }
}

void WsprDbDialog::finished ()
{
  auto const& r = watcher_.result ();
  setBusy (false);
  busy_label_->clear ();
  show_result (r.ok, r.message);
}

void WsprDbDialog::setBusy (bool busy)
{
  if (busy == busy_) return;
  busy_ = busy;
  test_button_->setEnabled (!busy);
  create_button_->setEnabled (!busy);
  buttons_->button (QDialogButtonBox::Save)->setEnabled (!busy);
  if (busy)
    QApplication::setOverrideCursor (Qt::BusyCursor);
  else
    QApplication::restoreOverrideCursor ();
}

void WsprDbDialog::show_result (bool ok, QString const& text)
{
  results_->setPlainText ((ok ? tr ("OK") : tr ("FAILED")) + "\n" + text);
  auto pal = results_->palette ();
  pal.setColor (QPalette::Text, ok ? QColor {0, 110, 0} : QColor {170, 0, 0});
  results_->setPalette (pal);
}
