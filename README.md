# DX-DBiDigiModes V2R3

**DX-DBiDigiModes** is a personal build of the amateur radio weak-signal program
**WSJT-X 3.0.2** (FT8, FT4, WSPR, FST4W, JT65, Q65 and the other WSJT-X modes) for 64-bit Windows.
It is based on WSJT-X and modified by **P. H. Ewen, KO4WSC**.

> This is **not** an official WSJT-X release and is not supported or endorsed by the WSJT Development
> Team. Please do not report problems with this build to the WSJT-X team. "WSJT-X" is a trademark of
> the WSJT Development Team (see [TRADEMARK.md](TRADEMARK.md)); it is used here only to say what this
> program is based on.

## What this build adds

### Logging to databases and files
Everything received can be logged as it is decoded, to any combination of three places:

- **MariaDB (or MySQL) database**
  - *Database > WSPR Database Settings...*: server, port, database name, user name and password
    (the password can be remembered, encrypted with Windows DPAPI for the current Windows user).
  - **Test Connection** shows the server version, whether the database and tables exist, the number of
    spots stored and the latest one.
  - **Create Database & Tables** creates whatever is missing; it never changes existing tables or data.
  - Tables: `wspr_spots` (every WSPR / FST4W spot, with all fields from the decoder, transmitter call,
    grid, power, distance and bearing), `wspr_rx_periods` (one row per receive period, including periods
    with no decodes) and `decodes` (decodes of all other modes).
  - The logger runs in its own low-priority thread: a slow or unreachable server never holds up decoding.
    Spots that cannot be written are kept (up to 20,000) and retried. Replaying the same recording does
    not store duplicates.
- **CSV files** (*Database > Log WSPR / FST4W spots to CSV file*): `wspr_spots.csv` and
  `decodes_all_modes.csv`, UTF-8, one header line, readable by Excel or any spreadsheet.
- **Microsoft Access file** (*Database > Log WSPR / FST4W spots to Microsoft Access file*): `.accdb` or
  `.mdb`, created on first use, with the same tables as the database and a unique index so the same spot
  is not stored twice. Needs the 64-bit "Microsoft Access Driver (*.mdb, *.accdb)", which comes with
  64-bit Office or the free Microsoft Access Database Engine. The file stays open between receive
  periods and closes after a minute without writes.
- **All modes** (*Database > Also log decodes of all other modes*): FT8, FT4, JT65, JT9, Q65 and the
  other modes are logged too (only decodes that pass WSJT-X's own false-decode filters).
- Every row records the receiving radio (*Database > Radio Name for Logs...*, or the rig model from
  Settings > Radio) and the time it was logged in both UTC and local time - the same values in every log.
- Decodes of recordings opened with *File > Open* are logged only when you choose to.

### Antenna tuner (ATU) control
- **ATU button** next to *Tune*: sends the rig's own CAT antenna-tuner start command
  (Hamlib `RIG_OP_TUNE`, for example `AC002;` on a Yaesu FTDX10). The rig makes its own carrier;
  the program sends no tone and no PTT.
- **Tune on band change** checkbox on the main window: starts the rig's tuner after every band change
  (band selector, band buttons, the rig's own dial or band hopping).
- **Band hopping schedule**: a new **ATU** row - tick a band to start the rig's tuner each time the
  schedule hops to it.

### Faster decoding and other changes
- WSPR decoder (wsprd) about 2.5x faster at the deepest setting, with identical output.
- Faster OSD decoding for FT8, FT4, FST4 and FST4W; multithreaded FT8 decoding cleaned up, with
  several thread-safety bugs fixed.
- Lighter GUI work per decode (waterfall drawing, ALL.TXT writing, regular expressions, filters).
- No program updates are downloaded from the upstream project (the Settings > Radio "Update Hamlib"
  button is disabled).

See [CHANGES-DX-DBi.md](CHANGES-DX-DBi.md) for the full list. Every changed line in the source is marked
with a `DX-DBi` comment.

## Where things are
| Path | What it is |
|------|------------|
| `Network/WsprSpotDatabase.*` | MariaDB logger, settings, test and create-tables code |
| `Network/WsprSpotFileLog.*` | CSV and Microsoft Access logger |
| `widgets/WsprDbDialog.*` | the WSPR Database Settings dialog |
| `widgets/mainwindow.*` | Database menu, ATU button, Tune on band change |
| `WSPR/WSPRBandHopping.*` | band hopping schedule, including the ATU row |
| `widgets/about.cpp`, `widgets/SplashScreen.cpp` | About box and splash screen |
| `CMakeLists.txt` | build name and version (`DXDBI_NAME`, `DXDBI_VERSION`) |
| `setup_msys2_build.sh` | one-time build environment setup for Windows |
| `README-WSJT-X.md` | the original WSJT-X README |

## How to build (Windows, MSYS2)
1. Install [MSYS2](https://www.msys2.org/) and open the **MSYS2 MINGW64** shell.
2. Change to this source folder and run `./setup_msys2_build.sh`. It installs the packages
   (including the MariaDB client), builds libusb and Hamlib 4.7.0 into a private folder, and runs the
   first CMake configure into `<source folder>-build` beside the source.
3. Build: `cmake --build ../<source folder>-build -j`.
4. Installer (optional): `cpack` in the build folder makes `DX-DBiDigiModes-V2R3-...-win64.exe`.

The program is `DX-DBiDigiModes-V2R3.exe`. Settings are shared with WSJT-X (it still uses the
WSJT-X settings folder), so run only one of the two at a time.

## Setting up a MariaDB user
On the database server, for example:
```sql
CREATE USER 'wspr'@'192.168.1.%' IDENTIFIED BY 'choose-a-password';
GRANT ALL ON wsprphe.* TO 'wspr'@'192.168.1.%';
```
Then enter the server, user and password in *Database > WSPR Database Settings...*, press
**Test Connection**, then **Create Database & Tables**. The default database name is `wsprphe`.

## License
DX-DBiDigiModes is free software, licensed under the GNU General Public License version 3, the same
license as WSJT-X - see [COPYING](COPYING). The original WSJT-X copyright and credits are kept in the
program's About box and in the source.
