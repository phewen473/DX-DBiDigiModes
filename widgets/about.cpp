#include "about.h"

#include <QCoreApplication>
#include <QString>

#include "revision_utils.hpp"

#include "ui_about.h"

CAboutDlg::CAboutDlg(QWidget *parent) :
  QDialog(parent),
  ui(new Ui::CAboutDlg)
{
  ui->setupUi(this);

  // DX-DBi: the About box leads with this build's name and version (DX-DBiDigiModes V2R3, from the name
  // and version set in CMakeLists.txt), what it is based on, who modified it and what it adds.
  // The WSJT-X authors' copyright, credits and GPL notice below are kept unchanged.
  setWindowTitle ("About " + QString {DXDBI_NAME " " DXDBI_VERSION}); // dialog title: About DX-DBiDigiModes V2R3
  ui->labelTxt->setText ("<h2>" + QString {DXDBI_NAME " " DXDBI_VERSION} + "</h2>" // heading: build name and version
    "Based on " + QString {"WSJT-X v"                                                                // say what it is based on
                          + QCoreApplication::applicationVersion ()                                  // upstream version number
                          + " " + display_revision ()}.simplified () + ".<br />"                     // upstream revision, if any
    "Personal build, modified by P. H. Ewen, KO4WSC.<br />"                                          // who made the changes
    "Adds WSPR and all-mode decode logging to MariaDB, CSV and Microsoft Access,<br />"              // what this build adds: logging
    "and rig antenna tuner (ATU) control by CAT.<br />"                                              // what this build adds: ATU
    "Not an official WSJT-X release - do not report issues to the WSJT-X team.<br /><br />"         // keep problem reports away from upstream

    "WSJT-X implements a number of digital modes designed for <br />"
    "weak-signal Amateur Radio communication.  <br /><br />"
    "&copy; 2001-2026 by Joe Taylor, K1JT, Bill Somerville, G4WJS, <br />"
    "Steve Franke, K9AN, Nico Palermo, IV3NWV, <br />"
    "Uwe Risse, DG2YCB, Brian Moran, N9ADG, <br />"
    "Roger Rehr, W3SZ, John Nelson, G4KLA, <br />"
    "Charlie Suckling, DL3WDG, and Terrell Deppe, KJ5HST <br /><br />"
    "We gratefully acknowledge contributions from AC6SL, AE4JY,<br />"
    "DF2ET, DJ0OT, DL3WDG, EA4AC, G4KLA, IW3RAB, JA7UDE,<br />"
    "K3WYC, KA1GT, KA6MAL, KA9Q, KB1ZMX, KD6EKQ, KG4IYS, KI7MT,<br />"
    "KK1D, ND0B, PY1ZRJ, PY2SDR, VE1SKY, VK3ACF, VK4BDJ,<br />"
    "VK7MO, VR2UPU, W3DJS, W4TI, W4TV, and W9MDB.<br /><br />"
    "WSJT-X is licensed under the terms of Version 3 <br />"
    "of the GNU General Public License (GPL) <br /><br />"
    "<a href=" TO_STRING__ (PROJECT_HOMEPAGE) ">"
    "<img src=\":/icon_128x128.png\" /></a>"
    "<a href=\"https://www.gnu.org/licenses/gpl-3.0.txt\">"
    "<img src=\":/gpl-v3-logo.svg\" height=\"80\" /><br />"
    "https://www.gnu.org/licenses/gpl-3.0.txt</a>");
}

CAboutDlg::~CAboutDlg()
{
}
