// launcher.cpp -- the launcher window.
#include "launcher.h"

#include <wx/artprov.h>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/dirdlg.h>
#include <wx/ffile.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/hyperlink.h>
#include <wx/msgdlg.h>
#include <wx/settings.h>
#include <wx/spinctrl.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#ifdef __WXMSW__
#include <wx/msw/wrapcctl.h>
#include <shellapi.h>
#endif

#include <initializer_list>

#include "game.h"
#include "icon.h"
#include "keys.h"
#include "keysdialog.h"
#include "settings.h"
#include "version.h"

const char* const APP_TITLE = "F-117A Enhanced";

namespace {

const char* const WEBSITE = "https://kkania.com";
const char* const SUPPORT = "https://buymeacoffee.com/krzysztofkania";
const char* const SECTION = "Game";

const int MIN_SCALE = 1, MAX_SCALE = 6, DEFAULT_SCALE = 3;
// The game's speed: retrace ticks (70.09 a second) a frame at least in flight (--frame-ticks; 0 = the original pacing only).
const int MAX_RENDER = 8;  // --render-scale 1..8 (Auto = 0)
const int MIN_TICKS = 0, MAX_TICKS = 20, DEFAULT_TICKS = 7;

#ifdef __WXMSW__
HRESULT CALLBACK AboutCallback(HWND hwnd, UINT msg, WPARAM, LPARAM lp, LONG_PTR) {
    if (msg == TDN_HYPERLINK_CLICKED)
        ShellExecuteW(hwnd, L"open", reinterpret_cast<LPCWSTR>(lp), nullptr, nullptr, SW_SHOWNORMAL);
    return S_OK;
}
#else
// A label and a link on one line, for the portable About box.
void AddLink(wxWindow* parent, wxSizer* sizer, const wxString& label, const wxString& text, const wxString& url) {
    auto* line = new wxBoxSizer(wxHORIZONTAL);
    line->Add(new wxStaticText(parent, wxID_ANY, label + " "), 0, wxALIGN_CENTER_VERTICAL);
    line->Add(new wxHyperlinkCtrl(parent, wxID_ANY, text, url), 0, wxALIGN_CENTER_VERTICAL);
    sizer->Add(line);
}
#endif

// A small "i" icon whose hover label holds an explanation of the control beside it.
wxStaticBitmap* InfoIcon(wxWindow* parent, const wxString& text) {
    auto* icon = new wxStaticBitmap(parent, wxID_ANY, wxArtProvider::GetBitmapBundle(wxART_INFORMATION, wxART_MENU));
    icon->SetToolTip(text);
    return icon;
}

// A label, a text field, a Browse button and an info icon in a row of a two-column grid. Without `info` the
// icon's place is kept empty, so the fields and Browse buttons of all rows line up.
wxTextCtrl* PathRow(wxWindow* parent, wxFlexGridSizer* grid, const wxString& label, const wxString& tip,
                    wxButton** browse, const wxString& info = wxEmptyString) {
    grid->Add(new wxStaticText(parent, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    auto* text = new wxTextCtrl(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(parent->FromDIP(300), -1));
    text->SetToolTip(tip);
    *browse = new wxButton(parent, wxID_ANY, "B&rowse...");
    row->Add(text, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, parent->FromDIP(8));
    row->Add(*browse, 0, wxALIGN_CENTER_VERTICAL);
    wxStaticBitmap* icon = InfoIcon(parent, info);
    row->Add(icon, wxSizerFlags().CenterVertical().Border(wxLEFT, parent->FromDIP(6)).ReserveSpaceEvenIfHidden());
    if (info.empty()) icon->Hide();
    grid->Add(row, 1, wxEXPAND);
    return text;
}

}  // namespace

LauncherDialog::LauncherDialog()
    : wxDialog(nullptr, wxID_ANY, APP_TITLE, wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxMINIMIZE_BOX) {
    SetIcons(AppIcons());
    const int margin = FromDIP(12), gap = FromDIP(8), small = FromDIP(4);

    // Game files
    auto* filesBox = new wxStaticBoxSizer(wxVERTICAL, this, "Game files");
    wxWindow* fb = filesBox->GetStaticBox();
    auto* filesGrid = new wxFlexGridSizer(2, gap, gap);
    filesGrid->AddGrowableCol(1);
    wxButton* browseFolder = nullptr;
    wxButton* browseProgram = nullptr;
    folder_ = PathRow(fb, filesGrid, "&Folder:",
                      "The folder with the original game's files (F117.COM, START.EXE, VGAME.EXE and the rest).",
                      &browseFolder,
                      "The game saves its pilot roster and preferences in this folder, so it must be writable "
                      "(not under Program Files).");
    program_ = PathRow(fb, filesGrid, "&Program:", "f117-enhanced, the game.", &browseProgram);
    auto* statusRow = new wxBoxSizer(wxHORIZONTAL);
    statusIcon_ = new wxStaticBitmap(fb, wxID_ANY, wxArtProvider::GetBitmapBundle(wxART_WARNING, wxART_MENU));
    statusNote_ = new wxStaticText(fb, wxID_ANY, wxEmptyString);
    statusRow->Add(statusIcon_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, small);
    statusRow->Add(statusNote_, 1, wxALIGN_CENTER_VERTICAL);
    filesBox->Add(filesGrid, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, gap);
    filesBox->Add(statusRow, 0, wxEXPAND | wxALL, gap);
    for (wxTextCtrl* field : {folder_, program_})
        field->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
            if (!loading_) UpdateState();
        });
    browseFolder->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { BrowseFolder(); });
    browseProgram->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { BrowseProgram(); });

    // Options
    auto* optionsBox = new wxStaticBoxSizer(wxVERTICAL, this, "Options");
    wxWindow* ob = optionsBox->GetStaticBox();
    auto* grid = new wxFlexGridSizer(2, gap, gap);
    grid->Add(new wxStaticText(ob, wxID_ANY, "&Aspect:"), 0, wxALIGN_CENTER_VERTICAL);
    aspect_ = new wxChoice(ob, wxID_ANY);
    aspect_->Append("4:3 (as on a VGA monitor)");
    aspect_->Append("1:1 (square pixels)");
    aspect_->SetToolTip("4:3 stretches the 320x200 picture to a 4:3 area like the monitors of 1991; 1:1 shows "
                        "it with square pixels.");
    grid->Add(aspect_, 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(new wxStaticText(ob, wxID_ANY, "&Window size:"), 0, wxALIGN_CENTER_VERTICAL);
    scale_ = new wxChoice(ob, wxID_ANY);
    scale_->SetToolTip("The window's size when the game starts. Alt+Enter switches to full screen.");
    grid->Add(scale_, 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(new wxStaticText(ob, wxID_ANY, "Game &speed:"), 0, wxALIGN_CENTER_VERTICAL);
    auto* speedRow = new wxBoxSizer(wxHORIZONTAL);
    speed_ = new wxSpinCtrl(ob, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(64), -1),
                            wxSP_ARROW_KEYS, MIN_TICKS, MAX_TICKS, DEFAULT_TICKS);
    speed_->SetToolTip("Retrace ticks (70 a second) each frame takes at least in flight.");
    speedRow->Add(speed_, 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(speedRow, 0, wxALIGN_CENTER_VERTICAL);
    optionsBox->Add(grid, 0, wxLEFT | wxRIGHT | wxTOP, gap);
    fullscreen_ = new wxCheckBox(ob, wxID_ANY, "Start in f&ull screen");
    fullscreen_->SetToolTip("Alt+Enter switches between full screen and a window in the game.");
    optionsBox->Add(fullscreen_, 0, wxLEFT | wxRIGHT | wxTOP, gap);
    sound_ = new wxCheckBox(ob, wxID_ANY, "S&ound");
    sound_->SetToolTip("AdLib music and effects; off runs the game silently. Whatever sound device the game's "
                       "setup chose is played as AdLib in this version.");
    optionsBox->Add(sound_, 0, wxALL, gap);
    aspect_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { FillScales(); });

    // Enhanced
    auto* enhBox = new wxStaticBoxSizer(wxVERTICAL, this, "Enhanced");
    wxWindow* eb = enhBox->GetStaticBox();
    auto* enhGrid = new wxFlexGridSizer(2, gap, gap);
    renderLabel_ = new wxStaticText(eb, wxID_ANY, "&Render scale:");
    enhGrid->Add(renderLabel_, 0, wxALIGN_CENTER_VERTICAL);
    renderScale_ = new wxChoice(eb, wxID_ANY);
    renderScale_->Append("Auto");
    for (int n = 1; n <= MAX_RENDER; ++n) renderScale_->Append(wxString::Format("%d" L"×", n));
    renderScale_->SetToolTip("How many times finer than the original 320x200 the 3D view and the displays are drawn; "
                             "Auto picks it from the window size.");
    enhGrid->Add(renderScale_, 0, wxALIGN_CENTER_VERTICAL);
    frameLabel_ = new wxStaticText(eb, wxID_ANY, "Frame ra&te:");
    enhGrid->Add(frameLabel_, 0, wxALIGN_CENTER_VERTICAL);
    frameRate_ = new wxChoice(eb, wxID_ANY);
    frameRate_->Append("Monitor");
    frameRate_->Append("60");
    frameRate_->Append("30");
    frameRate_->Append("Off");
    frameRate_->SetToolTip("How many pictures a second the game draws: Monitor follows the screen's refresh; 60 and 30 "
                           "draw at that rate; Off draws one picture per game step.");
    enhGrid->Add(frameRate_, 0, wxALIGN_CENTER_VERTICAL);
    enhBox->Add(enhGrid, 0, wxLEFT | wxRIGHT | wxTOP, gap);
    wide_ = new wxCheckBox(eb, wxID_ANY, "&Wide view");
    wide_->SetToolTip("Shows more of the world to the left and right of the original 4:3 picture on a wider window.");
    enhBox->Add(wide_, 0, wxLEFT | wxRIGHT | wxTOP, gap);
    smooth_ = new wxCheckBox(eb, wxID_ANY, "Smooth distance &shading");
    smooth_->SetToolTip("Fades objects and ground into the distance smoothly instead of in the original's steps.");
    enhBox->Add(smooth_, 0, wxLEFT | wxRIGHT | wxTOP, gap);
    night_ = new wxCheckBox(eb, wxID_ANY, "True &night");
    night_->SetToolTip("Makes night flights really dark: a near black sky and ground and objects fading into the dark with distance.");
    enhBox->Add(night_, 0, wxLEFT | wxRIGHT | wxTOP, gap);
    classic_ = new wxCheckBox(eb, wxID_ANY, "Classic (all enhancements &off)");
    classic_->SetToolTip("Plays exactly like the original 320x200 game, whatever the other Enhanced options say.");
    enhBox->Add(classic_, 0, wxALL, gap);
    classic_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { UpdateEnhanced(); });

    // Buttons
    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    auto* about = new wxButton(this, wxID_ABOUT, "&About");
    auto* keyBindings = new wxButton(this, wxID_ANY, "&Key Bindings...");
    keyBindings->SetToolTip("Choose the keys for flying, weapons, views and the displays.");
    play_ = new wxButton(this, wxID_ANY, "&Play");
    auto* close = new wxButton(this, wxID_CLOSE, "Close");
    buttons->Add(about, 0, wxRIGHT, gap);
    buttons->Add(keyBindings);
    buttons->AddStretchSpacer();
    buttons->Add(play_, 0, wxRIGHT, gap);
    buttons->Add(close);
    play_->SetDefault();
    SetEscapeId(wxID_CLOSE);
    about->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { About(); });
    keyBindings->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EditKeys(); });
    play_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Play(); });
    close->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Close(); });

    auto* all = new wxBoxSizer(wxVERTICAL);
    all->Add(filesBox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, margin);
    all->Add(optionsBox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, margin);
    all->Add(enhBox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, margin);
    all->Add(buttons, 0, wxEXPAND | wxALL, margin);
    SetSizer(all);

    // Settings from the last run.
    wxString dir = settings::GetString(SECTION, "GameFolder", "");
    wxString program = settings::GetString(SECTION, "Program", "");
    folder_->ChangeValue(dir.empty() ? DefaultGameDir() : dir);
    program_->ChangeValue(program.empty() ? DefaultProgram() : program);
    aspect_->SetSelection(settings::GetInt(SECTION, "Aspect43", 1) != 0 ? 0 : 1);
    FillScales();
    scale_->SetSelection(
        wxMax(MIN_SCALE, wxMin(MAX_SCALE, settings::GetInt(SECTION, "Scale", DEFAULT_SCALE))) - MIN_SCALE);
    fullscreen_->SetValue(settings::GetInt(SECTION, "Fullscreen", 0) != 0);
    sound_->SetValue(settings::GetInt(SECTION, "Sound", 1) != 0);
    speed_->SetValue(wxMax(MIN_TICKS, wxMin(MAX_TICKS, settings::GetInt(SECTION, "FrameTicks", DEFAULT_TICKS))));
    renderScale_->SetSelection(wxMax(0, wxMin(MAX_RENDER, settings::GetInt(SECTION, "RenderScale", 0))));
    frameRate_->SetSelection(wxMax(0, wxMin(3, settings::GetInt(SECTION, "FrameRate", 0))));
    wide_->SetValue(settings::GetInt(SECTION, "Wide", 1) != 0);
    smooth_->SetValue(settings::GetInt(SECTION, "SmoothShading", 0) != 0);
    night_->SetValue(settings::GetInt(SECTION, "TrueNight", 0) != 0);
    classic_->SetValue(settings::GetInt(SECTION, "Classic", 0) != 0);
    UpdateEnhanced();
    bindings_ = keys::Load();
    loading_ = false;
    UpdateState();

    Bind(wxEVT_ACTIVATE, [this](wxActivateEvent& event) {
        if (event.GetActive()) UpdateState();  // files may have been copied in meanwhile
        event.Skip();
    });
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) {
        Save();
        Destroy();
    });

    Fit();
    if (!settings::RestoreWindowPosition(SECTION, this)) Centre();
}

void LauncherDialog::FillScales() {
    const int pick = scale_->GetSelection();
    const int h = aspect_->GetSelection() == 1 ? 200 : 240;
    scale_->Clear();
    for (int s = MIN_SCALE; s <= MAX_SCALE; ++s) scale_->Append(wxString::Format(L"%d × %d", 320 * s, h * s));
    scale_->SetSelection(pick >= 0 ? pick : DEFAULT_SCALE - MIN_SCALE);
}

void LauncherDialog::UpdateEnhanced() {
    const bool on = !classic_->GetValue();
    for (wxWindow* w : std::initializer_list<wxWindow*>{renderLabel_, renderScale_, frameLabel_, frameRate_, wide_, smooth_, night_}) w->Enable(on);
}

void LauncherDialog::UpdateState() {
    const wxString dir = folder_->GetValue(), program = program_->GetValue();
    const bool exists = wxFileName::FileExists(program);
    const bool dos = exists && IsDosProgram(program);
    const bool haveProgram = exists && !dos;
    const wxString missing = MissingGameFile(dir);
    wxString note;
    if (!missing.empty())
        note = wxString::Format("This folder needs the game's files: %s is missing.", missing);
    else if (dos)
        note = wxString::Format("%s is the original DOS game, not the port: choose f117-enhanced.exe.",
                                wxFileName(program).GetFullName());
    else if (!haveProgram)
        note = wxString::Format("%s isn't there.", wxFileName(program).GetFullName());
    else
        note = "Found the game's files.";
    const bool ok = haveProgram && missing.empty();
    statusIcon_->Show(!ok);
    statusNote_->SetLabel(note);
    play_->Enable(ok);
    Layout();
}

void LauncherDialog::BrowseFolder() {
    wxDirDialog dialog(this, "Choose the folder with the game's files", folder_->GetValue(),
                       wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
    if (dialog.ShowModal() == wxID_OK) folder_->SetValue(dialog.GetPath());  // raises wxEVT_TEXT
}

void LauncherDialog::BrowseProgram() {
    wxFileName current(program_->GetValue());
#ifdef __WXMSW__
    const char* const filter = "Programs (*.exe)|*.exe|All files (*.*)|*.*";
#else
    const char* const filter = "All files|*";
#endif
    wxFileDialog dialog(this, "Choose f117-enhanced", current.GetPath(), current.GetFullName(), filter,
                        wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dialog.ShowModal() == wxID_OK) program_->SetValue(dialog.GetPath());  // raises wxEVT_TEXT
}

GameOptions LauncherDialog::Options() const {
    GameOptions options;
    options.program = wxFileName(program_->GetValue()).GetFullPath();
    options.gameDir = wxFileName(folder_->GetValue()).GetFullPath();
    options.aspect43 = aspect_->GetSelection() != 1;
    options.scale = scale_->GetSelection() + MIN_SCALE;
    options.fullscreen = fullscreen_->GetValue();
    options.sound = sound_->GetValue();
    options.frameTicks = speed_->GetValue();
    options.classic = classic_->GetValue();
    options.renderScale = renderScale_->GetSelection();
    options.frameRate = frameRate_->GetSelection();
    options.wide = wide_->GetValue();
    options.smoothShading = smooth_->GetValue();
    options.trueNight = night_->GetValue();
    options.keys = keys::Argument(bindings_);
    return options;
}

bool LauncherDialog::PrintCommand(const wxString& path) const {
    wxFFile out(path, "wb");
    if (!out.IsOpened()) return false;
    for (const wxString& a : GameArguments(Options())) out.Write(a + wxString(wxUniChar(10)), wxConvUTF8);
    return true;
}

void LauncherDialog::Play() {
    Save();
    const GameOptions options = Options();
    wxString error;
    if (!LaunchGame(options, error)) wxMessageBox(error, APP_TITLE, wxOK | wxICON_ERROR, this);
}

void LauncherDialog::Save() {
    // A folder or program left at its default is stored empty, so it follows the launcher if it moves.
    const wxString dir = folder_->GetValue(), program = program_->GetValue();
    settings::SetString(SECTION, "GameFolder",
                        wxFileName(dir).SameAs(wxFileName(DefaultGameDir())) ? wxString() : dir);
    settings::SetString(SECTION, "Program",
                        wxFileName(program).SameAs(wxFileName(DefaultProgram())) ? wxString() : program);
    settings::SetInt(SECTION, "Aspect43", aspect_->GetSelection() != 1 ? 1 : 0);
    settings::SetInt(SECTION, "Scale", scale_->GetSelection() + MIN_SCALE);
    settings::SetInt(SECTION, "Fullscreen", fullscreen_->GetValue() ? 1 : 0);
    settings::SetInt(SECTION, "Sound", sound_->GetValue() ? 1 : 0);
    settings::SetInt(SECTION, "FrameTicks", speed_->GetValue());
    settings::SetInt(SECTION, "RenderScale", renderScale_->GetSelection());
    settings::SetInt(SECTION, "FrameRate", frameRate_->GetSelection());
    settings::SetInt(SECTION, "Wide", wide_->GetValue() ? 1 : 0);
    settings::SetInt(SECTION, "SmoothShading", smooth_->GetValue() ? 1 : 0);
    settings::SetInt(SECTION, "TrueNight", night_->GetValue() ? 1 : 0);
    settings::SetInt(SECTION, "Classic", classic_->GetValue() ? 1 : 0);
    settings::SaveWindowPosition(SECTION, this);
}

void LauncherDialog::EditKeys() {
    if (EditKeyBindings(this, bindings_)) keys::Save(bindings_);
}

void LauncherDialog::About() {
    const wxString title = wxString("About ") + APP_TITLE;
    const wxString heading = wxString(APP_TITLE) + " " + APP_VERSION_TEXT;
    const wxString blurb = "Starts f117-enhanced, the SDL3 port of F-117A Nighthawk Stealth Fighter 2.0 (MicroProse, 1991), with higher resolution and the Enhanced options.";
#ifdef __WXMSW__
    // The Windows task dialog.
    const wxString content = wxString::Format(
        "%s\n\n"
        "Author: Krzysztof Kania\n"
        "Website: <a href=\"%s\">kkania.com</a>\n"
        "Support: <a href=\"%s\">buymeacoffee.com/krzysztofkania</a>",
        blurb, WEBSITE, SUPPORT);
    wxIcon icon;
    icon.CopyFromBitmap(AppBitmap(FromDIP(32)));
    TASKDIALOGCONFIG dialog{};
    dialog.cbSize = sizeof dialog;
    dialog.hwndParent = static_cast<HWND>(GetHWND());
    dialog.dwFlags = TDF_ENABLE_HYPERLINKS | TDF_USE_HICON_MAIN | TDF_ALLOW_DIALOG_CANCELLATION |
                     TDF_POSITION_RELATIVE_TO_WINDOW;
    dialog.dwCommonButtons = TDCBF_OK_BUTTON;
    dialog.pszWindowTitle = title.wc_str();
    dialog.hMainIcon = static_cast<HICON>(icon.GetHICON());
    dialog.pszMainInstruction = heading.wc_str();
    dialog.pszContent = content.wc_str();
    dialog.pfCallback = AboutCallback;
    TaskDialogIndirect(&dialog, nullptr, nullptr, nullptr);
#else
    wxDialog dialog(this, wxID_ANY, title);
    auto* body = new wxBoxSizer(wxHORIZONTAL);
    body->Add(new wxStaticBitmap(&dialog, wxID_ANY, AppBitmap(dialog.FromDIP(48))), 0, wxALL, dialog.FromDIP(12));

    auto* text = new wxBoxSizer(wxVERTICAL);
    auto* headingText = new wxStaticText(&dialog, wxID_ANY, heading);
    headingText->SetFont(dialog.GetFont().Bold().Scaled(1.3f));
    text->Add(headingText, 0, wxBOTTOM, dialog.FromDIP(8));
    text->Add(new wxStaticText(&dialog, wxID_ANY, blurb), 0, wxBOTTOM, dialog.FromDIP(12));
    text->Add(new wxStaticText(&dialog, wxID_ANY, "Author: Krzysztof Kania"));
    AddLink(&dialog, text, "Website:", "kkania.com", WEBSITE);
    AddLink(&dialog, text, "Support:", "buymeacoffee.com/krzysztofkania", SUPPORT);
    body->Add(text, 1, wxTOP | wxRIGHT | wxBOTTOM, dialog.FromDIP(12));

    auto* all = new wxBoxSizer(wxVERTICAL);
    all->Add(body, 1, wxEXPAND);
    all->Add(dialog.CreateStdDialogButtonSizer(wxOK), 0, wxEXPAND | wxALL, dialog.FromDIP(8));
    dialog.SetSizerAndFit(all);
    dialog.CentreOnParent();
    dialog.ShowModal();
#endif
}
