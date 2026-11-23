# DX-DBiDigiModes - changes from WSJT-X 3.0.2

DX-DBiDigiModes is based on WSJT-X 3.0.2 and modified by P. H. Ewen, KO4WSC.
Every change in the source is marked with a `DX-DBi` comment (with the version it came in, e.g. `DX-DBi v2R0`).
Built with MSYS2 MINGW64 (GCC, Qt 5.15, Boost, Hamlib 4.7.0, libusb 1.0.30, MariaDB Connector/C 3.4).

## V2R3 - name, performance and fixes
- **New name**: DX-DBiDigiModes V2R3 (program `DX-DBiDigiModes-V2R3.exe`). Title bar, splash screen, About
  box and installer show the new name, "based on WSJT-X 3.0.2" and "modified by P. H. Ewen, KO4WSC".
  The About box lists what this build adds. The WSJT-X authors' copyright, credits and GPL notice are unchanged.
- **Access log kept open between batches** (closed after 60 s without writes, on errors, on a settings change
  and at shutdown). Opening and closing the file cost about 1 s per batch; logging is now 5-10x faster.
- **Separate threads**: the CSV / Access logger has its own low-priority thread, so a slow MariaDB server no
  longer delays the file logs, and the reverse.
- **MariaDB**: the one-time fill-in of the logging-time columns runs once per database, not on every connection.
- **Fix**: the CSV logger could rename a CSV file that was not its own (for example WSJT-X's
  `lotw-user-activity.csv`) if that file was chosen as the log location. It now only renames older logs of its
  own; any other file is left alone and the status bar says why.
- Default MariaDB server is now `localhost`.

## V2R2 - all modes, Tune on band change, radio and time columns
- **Tune on band change** checkbox on the main window (under CQ / BP). Every band change starts the rig's
  CAT antenna tuner 1.5 s later - not at start-up, out of band, without rig control, while transmitting or
  while tuning.
- Band hopping schedule: the per-band row is named **ATU**. With the main checkbox on, every band is tuned;
  the two never tune twice.
- **Decodes of all modes** (*Database > Also log decodes of all other modes*): FT8, FT4, JT65, JT9, Q65 and
  others go to MariaDB (`decodes` table), CSV (`decodes_all_modes.csv`) and Access (`decodes` table).
  Only decodes that pass WSJT-X's false-decode filters are logged.
- **Radio column** in every table and file: the name set in *Database > Radio Name for Logs...*, or the rig
  model from Settings > Radio.
- **`inserted_at_utc` and `inserted_at_local`** in every log, one time stamp per batch, the same in all logs.
- Older tables and files are upgraded automatically (columns added; an older CSV is renamed
  `<name>_until_<date>.csv` and a new one started).
- **Fix**: crash inside the Microsoft Access ODBC driver when a number column got a value after a NULL.
- **Fix**: choosing a band from the band list did not start the tuner.

## V2R1 - ATU, CSV and Access logs
- **ATU button** next to Tune: sends the rig's CAT antenna-tuner start command (Hamlib `RIG_OP_TUNE`,
  e.g. `AC002;` on a Yaesu FTDX10). Greyed for 6 s while tuning; ignored while transmitting.
- **Band hopping schedule**: new row to start the rig's tuner on each hop to a ticked band.
- Hamlib `do_tune`: sends the command whenever the rig supports a TUNE operation.
- **WSPR spots to a CSV file and a Microsoft Access file** (*Database* menu), each on its own or together
  with MariaDB. Spots that cannot be written (file open in Excel, file locked) are kept and written later.
- Own splash screen and About box; the Settings > Radio "Update Hamlib" download from the upstream project
  is disabled.

## V2R0 - WSPR spot database and faster decoding
- **Database menu**: *WSPR Database Settings...* (server, port, database, user, password with optional
  DPAPI-encrypted storage, *Require verified TLS*) with **Test Connection** and **Create Database & Tables**,
  and a switch to log WSPR / FST4W spots to MariaDB or MySQL.
- Tables `wspr_spots` (all decoder fields, transmitter call / grid / power, distance and bearing),
  `wspr_rx_periods` (every receive period, including ones with no decodes) and `wsprphe_schema`.
- wsprd writes every field of every decode to `wspr_decodes_db.txt` for the program to log.
- The installer includes Qt's MySQL driver, the MariaDB client library and its authentication plugins.
- **wsprd about 2.5x faster** at the deepest setting with byte-identical output (faster OSD, coarse search
  and demodulation; candidate search in parallel with OpenMP).
- **Faster OSD** for FST4 / FST4W and FT4 / FT8, verified bit-identical against the previous code.

## V1R0 - performance and bug fixes
- **Multithreaded FT8**: one OpenMP parallel region; symbol spectra and the long FFT computed once per pass;
  race conditions fixed in signal subtraction, FFT plan lookup and OSD; fixed a typo that gave one thread an
  empty slice with 11 or 12 threads.
- jt9 waits on an event from the GUI instead of polling every 10 ms.
- GUI: smaller decode hand-off, ALL.TXT kept open, faster waterfall drawing, pre-compiled regular
  expressions, faster "worked today" filters and highlight requests.
- **Fixes**: ALLCALL7.TXT matched callsign fragments; audio alerts leaked objects; the waterfall read past its
  buffer; `grid.dat` loading had no bounds checks.
