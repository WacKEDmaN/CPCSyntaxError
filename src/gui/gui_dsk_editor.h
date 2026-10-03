// CPCSyntaxError GUI — the DSK editor: a disc image's files, tracks and sectors.
//
// Discs come from a file, from drive A or B (shared: the CPC and the editor see one disc),
// or are made new in any of the CPC's formats or a geometry of one's own. Files: AMSDOS /
// CP/M directory of any format, import (with an AMSDOS header if wanted), export, delete,
// rename, user, read-only / system / archived, and a look at one -- hex, BASIC listing,
// text or disassembly. Tracks & sectors: every track's header, every sector's ID, status
// bytes and copies (weak sectors), its bytes in a hex editor, sectors added and removed,
// tracks formatted. Map: which file owns each block and each sector. It saves as the
// extended format, or the standard one where the disc fits it.
#pragma once
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "emuhost.h"
#include "gui_widgets.h"
#include "core/dsk.h"
#include "core/dsk_edit.h"

namespace cpcse {

class DskEditorWindow {
public:
    DskEditorWindow(EmuHost& host, FileBrowser& browser, SaveDialog& saver);
    void draw(bool* open);
    bool focused = false;
    int requestTab = -1;             // a tab to bring to the front (0 Files .. 3 Disc), then -1

    // what the menus and the checks drive
    void newDisc(const DskGeometry& g);
    bool openFile(const std::string& path);
    bool saveFile(const std::string& path);
    void takeFromDrive(int unit);
    bool insertIntoDrive(int unit);
    std::shared_ptr<Disk> disc() const { return disk; }
    std::optional<DskGeometry> filesystem() const;   // the format chosen, or the one detected
    std::string status;

private:
    friend struct DskEditorCheck;
    EmuHost& host;
    FileBrowser& browser;
    SaveDialog& saver;
    std::shared_ptr<Disk> disk;
    std::string path, name;          // the file it came from / goes to, its display name
    bool saveStandard = false;       // save as the standard format when the disc fits it
    std::vector<DskGeometry> presets;
    int fsChoice = -1;               // index into presets; -1 = as the disc says

    // files
    std::string selectedKey;         // user + name of the file looked at
    int viewMode = 0;                // 0 hex, 1 BASIC, 2 text, 3 disassembly
    bool exportStrip = true;
    int importUser = 0, importType = 2, importLoad = 0x4000, importExec = 0x4000;
    bool importHeader = true;
    char renameBuf[16] = {}; int renameUser = 0;
    std::vector<std::string> viewLines;
    std::string viewKey;             // what viewLines were made from
    // tracks & sectors
    int selCyl = 0, selSide = 0, selSector = 0, selCopy = 0;
    int hexEditing = -1; char hexBuf[4] = {};
    DskGeometry formatGeometry, newGeometry;
    int newPreset = 0;
    char newInterleave[64] = {}, formatInterleave[64] = {};
    bool openNewPopup = false, openFormatPopup = false, openRenamePopup = false;
    int resizeTracks = 40, resizeSides = 1;
    char creatorBuf[16] = {};

    void toolbar();
    void tabFiles();
    void tabTracks();
    void tabMap();
    void tabInfo();
    void popups();
    void geometryFields(DskGeometry& g, char* interleave, size_t interleaveSize, bool withFilesystem);
    void importFile(const std::string& hostPath);
    void makeView(const DskFsFile& f, const Bytes& data);
    std::string displayName() const;
};

} // namespace cpcse
