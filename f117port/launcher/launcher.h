// launcher.h -- the launcher window: choose the game folder and the display and sound options, then Play.
#pragma once

#include <wx/dialog.h>

#include "game.h"
#include "keys.h"

class wxButton;
class wxCheckBox;
class wxChoice;
class wxSpinCtrl;
class wxStaticBitmap;
class wxStaticText;
class wxTextCtrl;

extern const char* const APP_TITLE;

class LauncherDialog : public wxDialog {
public:
    LauncherDialog();
    // Headless check (--print-command FILE): writes the program and its arguments, one per line, as Play would run them.
    bool PrintCommand(const wxString& path) const;

private:
    // Checks the folder and the program and enables Play to match.
    void UpdateState();
    // Relabels the window sizes for the chosen aspect.
    void FillScales();
    void BrowseFolder();
    void BrowseProgram();
    GameOptions Options() const;
    void Play();
    void About();
    void EditKeys();
    void Save();
    // Greys the Enhanced options out while Classic is checked.
    void UpdateEnhanced();

    wxTextCtrl* folder_ = nullptr;
    wxTextCtrl* program_ = nullptr;
    wxStaticBitmap* statusIcon_ = nullptr;
    wxStaticText* statusNote_ = nullptr;
    wxChoice* aspect_ = nullptr;
    wxChoice* scale_ = nullptr;
    wxCheckBox* fullscreen_ = nullptr;
    wxCheckBox* sound_ = nullptr;
    wxSpinCtrl* speed_ = nullptr;
    wxStaticText* renderLabel_ = nullptr;
    wxChoice* renderScale_ = nullptr;
    wxStaticText* frameLabel_ = nullptr;
    wxChoice* frameRate_ = nullptr;
    wxCheckBox* wide_ = nullptr;
    wxCheckBox* smooth_ = nullptr;
    wxCheckBox* night_ = nullptr;
    wxCheckBox* classic_ = nullptr;
    wxButton* play_ = nullptr;
    keys::Bindings bindings_;  // the game's keys (Key Bindings...), saved with the other options
    bool loading_ = true;  // no edits are recorded while the window is built
};
