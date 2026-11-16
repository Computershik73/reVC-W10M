// The game's files, and the screen that asks for them.
//
// reVC is the game's code; its data is the player's own copy of Grand Theft Auto: Vice City
// (the PC version). The player copies that folder to the phone and picks it here; the app
// copies it into LocalState\game. It cannot be read where it lies: a UWP app on Windows 10
// Mobile cannot open a picked folder's files by path (no CreateFileFromApp before 1803), and
// the game opens everything with plain C file calls.
//
// Over that copy go the files reVC itself adds and replaces (the repository's gamefiles\,
// carried in the package), again whenever the package's version changes.
//
// Saves and settings (userfiles\, reVC.ini, gta_vc.set) are in the same folder, as on a PC,
// and are kept when the game's files are replaced.
//
// The copy is 1.5 GB in 1400 files through the system's storage broker, minutes of it on a
// slow phone, and it does not always get through in one go: see "Taking the game in" below.
#pragma once

namespace revc {

class GameFiles {
public:
    using Sink = std::function<void()>;
    using LogSink = std::function<void(std::string const&)>;

    GameFiles(winrt::Windows::UI::Xaml::Controls::Panel const& host, std::wstring const& localState,
              std::wstring const& package, std::wstring const& version, Sink play, LogSink log)
        : local_(localState), game_(localState + L"\\game"), package_(package), version_(Utf8(version)),
          play_(std::move(play)), log_(std::move(log)) {
        using namespace winrt::Windows::UI::Xaml;
        using namespace winrt::Windows::UI::Xaml::Controls;

        root_ = Grid();
        root_.Background(Brush(0xFF14091C));
        ScrollViewer scroll;
        scroll_ = scroll;
        scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
        // no sideways scrolling: with it the column is measured at unlimited width and runs off
        // the right edge of a portrait screen (butterscotch-w10m)
        scroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
        scroll.HorizontalScrollMode(ScrollMode::Disabled);
        column_ = StackPanel();
        column_.MaxWidth(620);
        column_.Margin({24, 16, 24, 24});
        column_.HorizontalAlignment(HorizontalAlignment::Stretch);

        column_.Children().Append(Text(L"reVC", 32, 0xFFFF6EC7, true));
        column_.Children().Append(Text(T(L"Grand Theft Auto: Vice City, reverse-engineered, on Windows 10 Mobile",
                                         L"Grand Theft Auto: Vice City (обратная разработка) на Windows 10 Mobile"),
                                       14, 0xB3FFFFFF));

        notice_ = Text(L"", 15, 0xFFFFC66B);
        notice_.Margin({0, 16, 0, 0});
        notice_.Visibility(Visibility::Collapsed);
        column_.Children().Append(notice_);

        play_button_ = MakeButton(T(L"Play", L"Играть"));
        play_button_.Click([this](auto const&, auto const&) {
            if (!busy_) Start();
        });
        play_button_.Visibility(Visibility::Collapsed);
        column_.Children().Append(play_button_);

        choose_ = MakeButton(T(L"Choose the game's folder…", L"Выбрать папку с игрой…"));
        choose_.Click([this](auto const&, auto const&) { Import(); });
        column_.Children().Append(choose_);

        progress_ = ProgressBar();
        progress_.Margin({0, 12, 0, 0});
        progress_.Visibility(Visibility::Collapsed);
        column_.Children().Append(progress_);
        status_ = Text(L"", 14, 0xFFFFFFFF);
        status_.Margin({0, 6, 0, 0});
        column_.Children().Append(status_);

        auto help = Text(
            T(L"The app has no game data in it. It needs your own copy of Grand Theft Auto: Vice City for PC "
              L"(from a disc, Steam or another store):\n\n"
              L"1. On your PC, find the folder the game is installed in: the one with the folders audio, data, "
              L"models, anim, txd and TEXT.\n"
              L"2. Connect the phone by USB and copy that whole folder to it, to Documents or to the SD card. "
              L"It is about 1.5 GB.\n"
              L"3. Tap the button above and choose that folder. The game is copied into the app, which takes "
              L"several minutes; keep the app open. The folder you copied can be deleted afterwards.\n\n"
              L"Saves and settings stay when the game's files are replaced.\n\n"
              L"Controls: the on-screen buttons carry the names the game's own hints use, PlayStation's or "
              L"Xbox's (the PS / XBOX button in the game's menus). Put a "
              L"thumb down on the left half of the screen to move and on the right half to turn the camera. "
              L"A Bluetooth or USB gamepad takes over when it is connected. The Back button is Escape: pause, "
              L"and back in the menus.",
              L"В приложении нет данных игры. Нужна ваша собственная копия Grand Theft Auto: Vice City для ПК "
              L"(с диска, из Steam или другого магазина):\n\n"
              L"1. Найдите на ПК папку, в которую установлена игра: ту, где лежат папки audio, data, models, "
              L"anim, txd и TEXT.\n"
              L"2. Подключите телефон по USB и скопируйте эту папку целиком в Документы или на SD-карту. "
              L"Это около 1,5 ГБ.\n"
              L"3. Нажмите кнопку выше и выберите эту папку. Игра скопируется внутрь приложения, это займёт "
              L"несколько минут; не закрывайте приложение. Скопированную папку потом можно удалить.\n\n"
              L"Сохранения и настройки остаются при замене файлов игры.\n\n"
              L"Управление: на экранных кнопках те же названия, что и в подсказках самой игры, PlayStation "
              L"или Xbox (кнопка PS / XBOX в меню игры). "
              L"Палец на левой половине экрана — движение, на правой — поворот камеры. Геймпад по Bluetooth "
              L"или USB берёт управление на себя, когда подключён. Кнопка «Назад» работает как Escape: пауза "
              L"и возврат в меню."),
            14, 0xE6FFFFFF);
        help.Margin({0, 20, 0, 0});
        column_.Children().Append(help);

        Credits(column_);

        scroll.Content(column_);
        root_.Children().Append(scroll);
        host.Children().Append(root_);
    }

    // Who made the port, as the About of venetium-w10m and of gecko-w10m has it, and whose the
    // game's code is. At the foot of this screen, and over the game from "About the port" in
    // its Options menu (App.cpp): on a dark background in both places.
    static void Credits(winrt::Windows::UI::Xaml::Controls::Panel const& column) {
        auto ported = Text(T(L"Ported to Windows 10 Mobile by Computershik",
                             L"Порт на Windows 10 Mobile: Computershik"),
                           15, 0xFFFFFFFF);
        ported.Margin({0, 24, 0, 0});
        column.Children().Append(ported);
        column.Children().Append(Text(T(L"Porting, building and debugging on the device.",
                                        L"Портирование, сборка и отладка на устройстве."),
                                      12, 0xB3FFFFFF));
        column.Children().Append(Link(T(L"Profile on 4pda", L"Профиль на 4pda"),
                                      L"https://4pda.to/forum/index.php?showuser=4458524"));
        column.Children().Append(Link(T(L"Telegram channel @cmplog", L"Канал в Telegram: @cmplog"), L"https://t.me/cmplog"));
        column.Children().Append(Link(T(L"Donate (within the CIS for now)", L"Поддержать разработчика (пока только из СНГ)"),
                                      L"https://pay.cloudtips.ru/p/83821e32"));
        column.Children().Append(Text(T(L"Outside the CIS and would like to help? Send a message to the Telegram channel.",
                                        L"Вы не из СНГ и хотите помочь? Напишите в канал в Telegram."),
                                      12, 0xB3FFFFFF));

        auto about = Text(
            T(L"reVC is the reverse-engineered source of the game (github.com/mrxenginner/reVC); this port "
              L"builds on its Android fork (github.com/codepdbh/revc-android-port-evolved). It is not made or "
              L"endorsed by Rockstar Games or Take-Two Interactive.",
              L"reVC — исходный код игры, восстановленный обратной разработкой (github.com/mrxenginner/reVC); "
              L"этот порт основан на его форке для Android (github.com/codepdbh/revc-android-port-evolved). "
              L"Rockstar Games и Take-Two Interactive не имеют к нему отношения."),
            12, 0x99FFFFFF);
        about.Margin({0, 20, 0, 0});
        column.Children().Append(about);
    }

    std::wstring const& Folder() const { return game_; }

    // The files a game folder cannot do without: enough to tell it from any other folder
    bool Present() const {
        return FileExists(game_ + L"\\models\\gta3.img") && FileExists(game_ + L"\\models\\gta3.dir") &&
               FileExists(game_ + L"\\data\\gta_vc.dat");
    }

    // Shows the screen; with a notice (the last start failed) the player chooses what to do,
    // without one and with the files in place there is nothing to ask
    void Open(std::wstring const& notice) {
        using namespace winrt::Windows::UI::Xaml;
        bool present = Present();
        notice_.Text(winrt::hstring(notice));
        notice_.Visibility(notice.empty() ? Visibility::Collapsed : Visibility::Visible);
        play_button_.Visibility(present ? Visibility::Visible : Visibility::Collapsed);
        choose_.Content(winrt::box_value(winrt::hstring(
            present ? T(L"Replace the game's files…", L"Заменить файлы игры…")
                    : T(L"Choose the game's folder…", L"Выбрать папку с игрой…"))));
        root_.Visibility(Visibility::Visible);
        Sweep();
        // A copy that was cut short (the app was closed, the phone restarted) goes on by itself,
        // once: the folder it came from is remembered
        if (!present && !resumeTried_ && FileExists(game_ + L".part\\.source")) {
            resumeTried_ = true;
            Resume();
        }
    }

    void Close() { root_.Visibility(winrt::Windows::UI::Xaml::Visibility::Collapsed); }

    // The foot of the screen, for a look at it from the PC (REVC_FILES_SCREEN=end)
    void ShowEnd() {
        scroll_.UpdateLayout();
        scroll_.ChangeView(nullptr, scroll_.ScrollableHeight(), nullptr, true);
    }

    bool Visible() const { return root_.Visibility() == winrt::Windows::UI::Xaml::Visibility::Visible; }

    // The window's width and where the status and navigation bars cover it
    void Place(double width, winrt::Windows::UI::Xaml::Thickness insets) {
        scroll_.Margin(insets);  // not Grid.Padding: older Windows 10 Mobile builds do not have it
        double inner = width - insets.Left - insets.Right - 48;
        if (inner > 0) column_.Width((std::min)(inner, 620.0));
    }

    // reVC's own files over the player's copy, when this package has not put them there yet.
    // False if they could not be written.
    bool Overlay() {
        std::wstring marker = local_ + L"\\gamefiles.version";
        if (ReadMarker(marker) == version_) return true;
        int copied = 0;
        bool ok = CopyTree(package_ + L"\\gamefiles", game_, copied);
        log_("files: reVC's game files, " + std::to_string(copied) + " copied" + (ok ? "" : ", with errors"));
        if (ok) WriteMarker(marker, version_);
        return ok;
    }

private:
    static constexpr const wchar_t* kKept[] = {L"userfiles", L"reVC.ini", L"gta_vc.set"};

    static winrt::Windows::UI::Xaml::Controls::TextBlock Text(const wchar_t* text, double size, uint32_t argb,
                                                               bool bold = false) {
        winrt::Windows::UI::Xaml::Controls::TextBlock t;
        t.Text(text);
        t.FontSize(size);
        t.Foreground(Brush(argb));
        t.TextWrapping(winrt::Windows::UI::Xaml::TextWrapping::Wrap);
        if (bold) t.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        return t;
    }

    // Opens in the phone's browser
    static winrt::Windows::UI::Xaml::Controls::HyperlinkButton Link(const wchar_t* text, const wchar_t* uri) {
        winrt::Windows::UI::Xaml::Controls::HyperlinkButton b;
        b.Content(winrt::box_value(winrt::hstring(text)));
        b.NavigateUri(winrt::Windows::Foundation::Uri(uri));
        b.FontSize(15);
        b.Padding({0, 6, 0, 6});
        return b;
    }

    static winrt::Windows::UI::Xaml::Controls::Button MakeButton(const wchar_t* text) {
        winrt::Windows::UI::Xaml::Controls::Button b;
        b.Content(winrt::box_value(winrt::hstring(text)));
        b.HorizontalAlignment(winrt::Windows::UI::Xaml::HorizontalAlignment::Stretch);
        b.Height(52);
        b.FontSize(18);
        b.Margin({0, 12, 0, 0});
        return b;
    }

    static std::wstring Lower(std::wstring s) {
        for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
        return s;
    }

    static bool EndsWith(std::wstring const& s, const wchar_t* suffix) {
        size_t n = wcslen(suffix);
        return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
    }

    static std::string ReadMarker(std::wstring const& path) {
        HANDLE h = ::CreateFile2(path.c_str(), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, nullptr);
        if (h == INVALID_HANDLE_VALUE) return {};
        char text[1024] = {};
        DWORD n = 0;
        ::ReadFile(h, text, sizeof(text) - 1, &n, nullptr);
        ::CloseHandle(h);
        return std::string(text, n);
    }

    static void WriteMarker(std::wstring const& path, std::string const& text) {
        HANDLE h = ::CreateFile2(path.c_str(), GENERIC_WRITE, 0, CREATE_ALWAYS, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        DWORD n = 0;
        ::WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &n, nullptr);
        ::CloseHandle(h);
    }

    // Plain Win32: both ends are the app's own folders
    static bool CopyTree(std::wstring const& from, std::wstring const& to, int& copied) {
        ::CreateDirectoryW(to.c_str(), nullptr);
        WIN32_FIND_DATAW item;
        HANDLE find = ::FindFirstFileExW((from + L"\\*").c_str(), FindExInfoBasic, &item, FindExSearchNameMatch, nullptr, 0);
        if (find == INVALID_HANDLE_VALUE) return false;
        bool ok = true;
        do {
            std::wstring name = item.cFileName;
            if (name == L"." || name == L"..") continue;
            if (item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                ok = CopyTree(from + L"\\" + name, to + L"\\" + name, copied) && ok;
            } else {
                // a file of the player's copy may be read-only, as on the disc it came from
                ::SetFileAttributesW((to + L"\\" + name).c_str(), FILE_ATTRIBUTE_NORMAL);
                COPYFILE2_EXTENDED_PARAMETERS parameters{};
                parameters.dwSize = sizeof(parameters);
                if (SUCCEEDED(::CopyFile2((from + L"\\" + name).c_str(), (to + L"\\" + name).c_str(), &parameters)))
                    copied++;
                else
                    ok = false;
            }
        } while (::FindNextFileW(find, &item));
        ::FindClose(find);
        return ok;
    }

    static void RemoveTree(std::wstring const& path) {
        WIN32_FIND_DATAW item;
        HANDLE find = ::FindFirstFileExW((path + L"\\*").c_str(), FindExInfoBasic, &item, FindExSearchNameMatch, nullptr, 0);
        if (find != INVALID_HANDLE_VALUE) {
            do {
                std::wstring name = item.cFileName;
                if (name == L"." || name == L"..") continue;
                std::wstring child = path + L"\\" + name;
                if (item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    RemoveTree(child);
                } else {
                    ::SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
                    ::DeleteFileW(child.c_str());
                }
            } while (::FindNextFileW(find, &item));
            ::FindClose(find);
        }
        ::RemoveDirectoryW(path.c_str());
    }

    void Busy(bool on, const wchar_t* text = L"") {
        using namespace winrt::Windows::UI::Xaml;
        busy_ = on;
        choose_.IsEnabled(!on);
        play_button_.IsEnabled(!on);
        progress_.Visibility(on ? Visibility::Visible : Visibility::Collapsed);
        status_.Text(text);
    }

    static winrt::Windows::Foundation::IAsyncAction Message(winrt::hstring text) {
        auto dialog = Dialog(text);
        co_await dialog.ShowAsync();
    }

    // What is left of replacing the files of a game that is there and plays: up to a whole
    // game. Without a game it is the copy to go on with.
    void Sweep() {
        if (!busy_ && Present() && FileExists(game_ + L".part")) {
            std::wstring part = game_ + L".part";
            std::thread([part] { RemoveTree(part); }).detach();
        }
    }

    void Start() {
        if (!Overlay()) {
            status_.Text(T(L"Could not write reVC's own files next to the game's. Is the storage full?",
                           L"Не удалось записать файлы reVC рядом с файлами игры. Не заполнена ли память?"));
            return;
        }
        play_();
    }

    static winrt::Windows::Foundation::IAsyncOperation<bool> Has(winrt::Windows::Storage::StorageFolder folder,
                                                                 const wchar_t* sub, const wchar_t* file) {
        using namespace winrt::Windows::Storage;
        auto item = co_await folder.TryGetItemAsync(sub);
        auto inner = item ? item.try_as<StorageFolder>() : nullptr;
        if (!inner) co_return false;
        auto found = co_await inner.TryGetItemAsync(file);
        co_return found && found.IsOfType(StorageItemTypes::File);
    }

    // ===[ Taking the game in ]===
    // The picked folder is read a folder at a time and each file is copied into game.part: by
    // StorageFile.CopyAsync, and where that fails (it does, with nothing but E_FAIL, for
    // anim\cuts.img on a Lumia 650 with Windows 10 Mobile 15063), by reading the file and
    // writing it here, which also says why when it is the storage that is full. A copy that was
    // cut short is taken up where it stopped: what is already there at its full size stays.
    // The screen is kept on meanwhile (the phone suspends an app it has locked over), and
    // game.part becomes game when everything is in.

    static constexpr const wchar_t* kToken = L"game";  // the picked folder, in the app's future access list

    struct Copying {
        size_t files = 0, kept = 0;
        uint64_t bytes = 0;
        int failuresInARow = 0;  // of CopyAsync: after three, every file is read instead
        ULONGLONG started = 0, noted = 0;
        std::wstring current;
    };

    static std::string Hex(winrt::hresult code) {
        char text[16];
        snprintf(text, sizeof(text), "0x%08X", static_cast<unsigned>(code.value));
        return text;
    }

    static uint64_t SizeOf(std::wstring const& path, bool* exists = nullptr) {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        bool found = ::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) != 0;
        if (exists) *exists = found;
        return found ? (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow : 0;
    }

    // The file's bytes through the broker's stream, written with plain Win32
    static winrt::Windows::Foundation::IAsyncAction StreamCopy(winrt::Windows::Storage::StorageFile file,
                                                               std::wstring target) {
        using namespace winrt::Windows::Storage::Streams;
        struct Handle {
            HANDLE h = INVALID_HANDLE_VALUE;
            ~Handle() {
                if (h != INVALID_HANDLE_VALUE) ::CloseHandle(h);
            }
        } out;
        IInputStream input = co_await file.OpenSequentialReadAsync();
        ::SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_NORMAL);
        out.h = ::CreateFile2(target.c_str(), GENERIC_WRITE, 0, CREATE_ALWAYS, nullptr);
        if (out.h == INVALID_HANDLE_VALUE) throw winrt::hresult_error(HRESULT_FROM_WIN32(::GetLastError()));
        Buffer buffer(1u << 20);
        for (;;) {
            IBuffer got = co_await input.ReadAsync(buffer, buffer.Capacity(), InputStreamOptions::None);
            uint32_t length = got.Length();
            if (length == 0) break;
            DWORD written = 0;
            if (!::WriteFile(out.h, got.data(), length, &written, nullptr) || written != length)
                throw winrt::hresult_error(HRESULT_FROM_WIN32(::GetLastError()));
        }
    }

    winrt::Windows::Foundation::IAsyncAction CopyOne(winrt::Windows::Storage::StorageFile file,
                                                     winrt::Windows::Storage::StorageFolder to, std::wstring toPath) {
        using namespace winrt::Windows::Storage;
        std::wstring name(file.Name());
        std::wstring target = toPath + L"\\" + name;
        copying_.current = name;
        bool exists = false;
        uint64_t have = SizeOf(target, &exists);
        if (exists && have == (co_await file.GetBasicPropertiesAsync()).Size()) {
            copying_.kept++;  // from the copy that was cut short
            co_return;
        }
        status_.Text(winrt::hstring(std::wstring(T(L"Copying ", L"Копирую ")) + name + L" (" +
                                    std::to_wstring(copying_.files + copying_.kept + 1) + L")"));
        bool copied = false;
        if (copying_.failuresInARow < 3) {
            try {
                co_await file.CopyAsync(to, file.Name(), NameCollisionOption::ReplaceExisting);
                copied = true;
                copying_.failuresInARow = 0;
            } catch (winrt::hresult_error const& e) {
                copying_.failuresInARow++;
                log_("files: " + Utf8(name) + ": CopyAsync failed, " + Hex(e.code()) + " " + Utf8(std::wstring(e.message())) +
                     "; reading the file instead");
            }
        }
        if (!copied) co_await StreamCopy(file, target);
        copying_.files++;
        copying_.bytes += SizeOf(target);
        ULONGLONG now = ::GetTickCount64();
        if (now - copying_.noted >= 20000) {
            copying_.noted = now;
            log_("files: " + std::to_string(copying_.files) + " copied so far, " + std::to_string(copying_.bytes >> 20) +
                 " MB, in " + std::to_string((now - copying_.started) / 1000) + " s");
        }
    }

    winrt::Windows::Foundation::IAsyncAction CopyFolder(winrt::Windows::Storage::StorageFolder from, std::wstring toPath,
                                                        bool top) {
        using namespace winrt::Windows::Storage;
        ::CreateDirectoryW(toPath.c_str(), nullptr);
        StorageFolder to = co_await StorageFolder::GetFolderFromPathAsync(toPath);
        std::vector<StorageFolder> inner;
        // The whole folder in one question: asked for a page at a time, the broker reads the
        // folder through again for every page (36 s each for Audio's 1300 files on a Lumia 650)
        auto items = co_await from.GetItemsAsync();
        uint32_t count = items.Size();
        for (uint32_t i = 0; i < count; i++) {
            auto item = items.GetAt(i);
            std::wstring name = Lower(std::wstring(item.Name()));
            if (auto file = item.try_as<StorageFile>()) {
                // the Windows program and its libraries are of no use here
                if (EndsWith(name, L".exe") || EndsWith(name, L".dll") || EndsWith(name, L".asi") ||
                    EndsWith(name, L".pdb") || EndsWith(name, L".url") || EndsWith(name, L".lnk"))
                    continue;
                co_await CopyOne(file, to, toPath);
            } else if (auto folder = item.try_as<StorageFolder>()) {
                // intro videos (not played) and the Miles sound system's DLLs (OpenAL plays)
                if (top && (name == L"movies" || name == L"mss")) continue;
                inner.push_back(folder);
            }
        }
        items = nullptr;
        for (auto const& folder : inner) co_await CopyFolder(folder, toPath + L"\\" + std::wstring(folder.Name()), false);
    }

    winrt::Windows::Foundation::IAsyncAction CopyFrom(winrt::Windows::Storage::StorageFolder source) {
        using namespace winrt::Windows::Storage;
        std::wstring from(source.Path());
        std::wstring part = game_ + L".part";
        log_("files: copying the game from " + Utf8(from));
        Busy(true, T(L"Copying…", L"Копирую…"));
        progress_.IsIndeterminate(true);
        winrt::Windows::System::Display::DisplayRequest awake{nullptr};
        try {
            awake = winrt::Windows::System::Display::DisplayRequest();
            awake.RequestActive();
        } catch (...) {
            awake = nullptr;
        }
        // what is there of another folder's copy is of no use to this one
        bool resumed = FileExists(part) && ReadMarker(part + L"\\.source") == Utf8(from);
        if (FileExists(part) && !resumed) RemoveTree(part);
        ::CreateDirectoryW(part.c_str(), nullptr);
        WriteMarker(part + L"\\.source", Utf8(from));
        ULARGE_INTEGER space{};
        ::GetDiskFreeSpaceExW(local_.c_str(), &space, nullptr, nullptr);
        log_("files: " + std::to_string(space.QuadPart >> 20) + " MB free" +
             (resumed ? ", going on with a copy that was cut short" : ""));
        copying_ = Copying{};
        copying_.started = copying_.noted = ::GetTickCount64();

        std::wstring failure;
        std::string reason;
        try {
            co_await CopyFolder(source, part, true);
            copying_.current.clear();
            if (!FileExists(part + L"\\models\\gta3.img") || !FileExists(part + L"\\data\\gta_vc.dat"))
                throw winrt::hresult_error(E_FAIL, L"models\\gta3.img or data\\gta_vc.dat did not arrive");
            ::DeleteFileW((part + L"\\.source").c_str());

            // Saves and settings move over from the copy that is being replaced, then it goes
            if (FileExists(game_)) {
                for (const wchar_t* kept : kKept) {
                    std::wstring old = game_ + L"\\" + kept, fresh = part + L"\\" + kept;
                    if (!FileExists(old)) continue;
                    if (FileExists(fresh)) {  // the PC copy's own saves or settings: the phone's win
                        RemoveTree(fresh);
                        ::DeleteFileW(fresh.c_str());
                    }
                    ::MoveFileExW(old.c_str(), fresh.c_str(), MOVEFILE_REPLACE_EXISTING);
                }
                RemoveTree(game_);
            }
            if (!::MoveFileExW(part.c_str(), game_.c_str(), 0)) throw winrt::hresult_error(HRESULT_FROM_WIN32(::GetLastError()));
            ::DeleteFileW((local_ + L"\\gamefiles.version").c_str());  // reVC's files go over the new copy
            log_("files: copied " + std::to_string(copying_.files) + " files, " + std::to_string(copying_.bytes >> 20) +
                 " MB, in " + std::to_string((::GetTickCount64() - copying_.started) / 1000) + " s; " +
                 std::to_string(copying_.kept) + " were there already");
            try {
                AccessCache::StorageApplicationPermissions::FutureAccessList().Remove(kToken);
            } catch (...) {
            }
        } catch (winrt::hresult_error const& e) {
            reason = Hex(e.code()) + " " + Utf8(std::wstring(e.message()));
            failure = std::wstring(e.message());
        } catch (std::exception const& e) {
            reason = e.what();
            failure = Wide(e.what());
        }
        progress_.IsIndeterminate(false);
        if (awake) {
            try {
                awake.RequestRelease();
            } catch (...) {
            }
        }
        if (!reason.empty()) {
            log_("files: copy failed" + (copying_.current.empty() ? std::string() : " at " + Utf8(copying_.current)) + ", " +
                 reason + "; " + std::to_string(copying_.files) + " files copied, " + std::to_string(copying_.kept) +
                 " there already");
            Busy(false);
            std::wstring text = T(L"Copying stopped", L"Копирование остановилось");
            if (!copying_.current.empty()) text += T(L" at ", L" на файле ") + copying_.current;
            text += L":\n\n" + failure + L"\n\n" +
                    T(L"About 1.5 GB of free space is needed. What has been copied stays: choose the folder "
                      L"again and the copy goes on from there.",
                      L"Нужно около 1,5 ГБ свободного места. Скопированное остаётся: выберите папку ещё раз, и "
                      L"копирование продолжится с этого места.");
            co_await Message(winrt::hstring(text));
            Open(L"");
            co_return;
        }
        Busy(false, T(L"Copied.", L"Скопировано."));
        if (Present()) Start();
        else Open(L"");
    }

    winrt::fire_and_forget Import() {
        using namespace winrt::Windows::Storage;
        if (busy_) co_return;
        Pickers::FolderPicker picker;
        picker.SuggestedStartLocation(Pickers::PickerLocationId::DocumentsLibrary);
        picker.FileTypeFilter().Append(L"*");
        StorageFolder source{nullptr};
        try {
            source = co_await picker.PickSingleFolderAsync();
        } catch (winrt::hresult_error const& e) {
            log_("files: folder picker failed, " + Hex(e.code()));
        }
        if (!source || busy_) co_return;

        bool valid = false;
        try {
            valid = (co_await Has(source, L"models", L"gta3.img")) && (co_await Has(source, L"data", L"gta_vc.dat"));
        } catch (winrt::hresult_error const&) {
        }
        if (!valid) {
            co_await Message(T(L"This is not the game's folder: there is no models\\gta3.img and data\\gta_vc.dat in "
                               L"it.\n\nChoose the folder Grand Theft Auto: Vice City for PC is installed in, the one "
                               L"with the folders audio, data, models, anim, txd and TEXT.",
                               L"Это не папка игры: в ней нет models\\gta3.img и data\\gta_vc.dat.\n\nВыберите папку, "
                               L"в которую установлена Grand Theft Auto: Vice City для ПК, ту, где лежат папки audio, "
                               L"data, models, anim, txd и TEXT."));
            co_return;
        }
        // remembered, so that a copy cut short can go on without being asked for again
        try {
            AccessCache::StorageApplicationPermissions::FutureAccessList().AddOrReplace(kToken, source);
        } catch (winrt::hresult_error const& e) {
            log_("files: the folder could not be remembered, " + Hex(e.code()));
        }
        resumeTried_ = true;
        co_await CopyFrom(source);
    }

    winrt::fire_and_forget Resume() {
        using namespace winrt::Windows::Storage;
        if (busy_) co_return;
        StorageFolder source{nullptr};
        try {
            auto list = AccessCache::StorageApplicationPermissions::FutureAccessList();
            if (!list.ContainsItem(kToken)) co_return;
            source = co_await list.GetFolderAsync(kToken);
        } catch (winrt::hresult_error const& e) {
            log_("files: the folder of the copy that was cut short is not to be had, " + Hex(e.code()));
        }
        if (!source || busy_) co_return;
        co_await CopyFrom(source);
    }

    std::wstring local_, game_, package_;
    std::string version_;
    Sink play_;
    LogSink log_;
    winrt::Windows::UI::Xaml::Controls::Grid root_{nullptr};
    winrt::Windows::UI::Xaml::Controls::ScrollViewer scroll_{nullptr};
    winrt::Windows::UI::Xaml::Controls::StackPanel column_{nullptr};
    winrt::Windows::UI::Xaml::Controls::TextBlock notice_{nullptr}, status_{nullptr};
    winrt::Windows::UI::Xaml::Controls::Button play_button_{nullptr}, choose_{nullptr};
    winrt::Windows::UI::Xaml::Controls::ProgressBar progress_{nullptr};
    Copying copying_;
    bool busy_ = false;
    bool resumeTried_ = false;
};

}  // namespace revc
