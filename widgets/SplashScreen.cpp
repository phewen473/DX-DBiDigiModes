#include "SplashScreen.hpp"

#include <QPixmap>
#include <QVBoxLayout>
#include <QCheckBox>
#include <QCoreApplication>

#include "revision_utils.hpp"
#include "pimpl_impl.hpp"

#include "moc_SplashScreen.cpp"

class SplashScreen::impl
{
public:
  impl ()
    : checkbox_ {"Do not show this again"}
  {
    main_layout_.addStretch ();
    main_layout_.addWidget (&checkbox_, 0, Qt::AlignRight);
  }

  QVBoxLayout main_layout_;
  QCheckBox checkbox_;
};

SplashScreen::SplashScreen ()
  : QSplashScreen {QPixmap {":/splash.png"}, Qt::WindowStaysOnTopHint}
{
  setLayout (&m_->main_layout_);
  // DX-DBi: the splash shows this build's name and version, e.g. "DX-DBiDigiModes V2R3", taken from the
  // build name and version set in CMakeLists.txt so it follows the version automatically.
  // The upstream "send issue reports to wsjtx.groups.io" text is removed: this is a personal build.
  showMessage ("<h2>" + QString {DXDBI_NAME " " DXDBI_VERSION} + "</h2>" // heading: build name and version
    "Based on WSJT-X v" + QCoreApplication::applicationVersion () + ".<br />"                    // say what it is based on
    "Personal build, modified by P. H. Ewen, KO4WSC.<br />"                                       // who made the changes
    "Not an official WSJT-X release - do not report issues to the WSJT-X team.<br /><br />"       // keep problem reports away from upstream
    "<img src=\":/icon_128x128.png\" />"
    "<img src=\":/gpl-v3-logo.svg\" height=\"80\" />", Qt::AlignCenter);
  connect (&m_->checkbox_, &QCheckBox::stateChanged, [this] (int s) {
      if (Qt::Checked == s) Q_EMIT disabled ();
    });
}

SplashScreen::~SplashScreen ()
{
}
