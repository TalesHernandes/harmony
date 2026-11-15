#include <windows.h>
#undef GetCurrentTime
#include <psapi.h>
#include <shellapi.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Documents.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.XamlTypeInfo.h>
#include <winrt/Windows.UI.Xaml.Interop.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Provider.h>
#include <microsoft.ui.xaml.window.h>
#include <UIAutomation.h>
#include <thread>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <vector>
#include <algorithm>

using namespace winrt;
using namespace Windows::Foundation;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Documents;
using namespace Microsoft::UI::Xaml::Media;
using namespace Microsoft::UI::Xaml::Automation;
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

struct Emoji { wchar_t const* text; wchar_t const* file; };
Emoji const emojis[] = {
    {L"🔥",L"1f525"},{L"😀",L"1f600"},{L"😃",L"1f603"},{L"😄",L"1f604"},
    {L"😁",L"1f601"},{L"😆",L"1f606"},{L"😅",L"1f605"},{L"😂",L"1f602"},
    {L"🙂",L"1f642"},{L"🙃",L"1f643"},{L"😉",L"1f609"},{L"😊",L"1f60a"},
    {L"😍",L"1f60d"},{L"😘",L"1f618"},{L"😎",L"1f60e"},{L"🤔",L"1f914"},
    {L"😭",L"1f62d"},{L"😡",L"1f621"},{L"👍",L"1f44d"},{L"👎",L"1f44e"},
    {L"👏",L"1f44f"},{L"🙌",L"1f64c"},{L"🙏",L"1f64f"},{L"🎉",L"1f389"},
    {L"🚀",L"1f680"},{L"⭐",L"2b50"},{L"✨",L"2728"},{L"💙",L"1f499"},
    {L"💚",L"1f49a"},{L"💜",L"1f49c"}
};
struct Message { std::wstring author, time, body; };
fs::path executableDirectory() {
    wchar_t path[32768]; GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    return fs::path(path).parent_path();
}
std::string quote(std::wstring const& value) {
    std::string out = "\"";
    for (unsigned char c : to_string(value)) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c < 32) { char escape[7]; sprintf_s(escape, "\\u%04x", c); out += escape; }
        else out += c;
    }
    return out + '"';
}
double processAgeMs() {
    FILETIME created{}, exited{}, kernel{}, user{}, now{};
    check_bool(GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user));
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER a{}, b{}; a.LowPart = created.dwLowDateTime; a.HighPart = created.dwHighDateTime;
    b.LowPart = now.dwLowDateTime; b.HighPart = now.dwHighDateTime;
    return static_cast<double>(b.QuadPart - a.QuadPart) / 10000.0;
}
struct Memory { double working{}, privateMb{}; };
Memory memory() {
    PROCESS_MEMORY_COUNTERS_EX info{}; info.cb = sizeof(info);
    check_bool(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&info), sizeof(info)));
    return {info.WorkingSetSize / 1048576.0, info.PrivateUsage / 1048576.0};
}
std::wstring accessible(Message const& m) { return m.author + L", " + m.time + L": " + m.body; }
struct App : ApplicationT<App, Markup::IXamlMetadataProvider> {
    XamlTypeInfo::XamlControlsXamlMetaDataProvider metadata;
    Markup::IXamlType GetXamlType(Windows::UI::Xaml::Interop::TypeName const& type) { return metadata.GetXamlType(type); }
    Markup::IXamlType GetXamlType(hstring const& name) { return metadata.GetXamlType(name); }
    com_array<Markup::XmlnsDefinition> GetXmlnsDefinitions() { return metadata.GetXmlnsDefinitions(); }
    Window window{nullptr}; ListView list{nullptr}; TextBox input{nullptr}; TextBlock count{nullptr};
    Windows::Foundation::Collections::IObservableVector<IInspectable> items{single_threaded_observable_vector<IInspectable>()};
    std::vector<Message> messages;
    bool segoe = false;
    bool unicode = true, loaded = false, measuring = false, finished = false;
    int phase = 0, waitFrames = 0, index = 0, direction = 1;
    fs::path measurePath, testPath;
    double firstMs = 0, worstMs = 0, intervalSum = 0, firstBodyHeight=0;
    uint64_t intervals = 0, slow = 0;
    Memory endMemory{}, afterMemory{};
    Clock::time_point previous{}, started{};
    event_token rendering{};
    DispatcherTimer timeout{nullptr};

    App() {
        UnhandledException([this](auto const&,UnhandledExceptionEventArgs const& args) {
            std::ofstream out(executableDirectory().parent_path()/L"runtime-error.log");
            out << "Unhandled HRESULT 0x" << std::hex << static_cast<uint32_t>(args.Exception().value)
                << ": " << to_string(args.Message()); out.close();
            args.Handled(true); fail(std::wstring(args.Message()));
        });
        int argc = 0; auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        for (int i=1; i<argc; ++i) {
            std::wstring arg = argv[i];
            if ((arg == L"--medir" || arg == L"--autoteste") && i+1<argc) {
                auto path = fs::absolute(argv[++i]);
                if (arg == L"--medir") measurePath = path; else testPath = path;
            } else if (arg == L"--segoe") { segoe=true; } else { LocalFree(argv); throw hresult_invalid_argument(L"Use --medir file OR --autoteste file"); }
        }
        LocalFree(argv);
        if (!measurePath.empty() && !testPath.empty()) throw hresult_invalid_argument(L"Run the two modes separately");
        std::mt19937 random(20261006);
        wchar_t const* authors[] = {L"Ana",L"João",L"Érica",L"Você",L"Luís"};
        for (int i=0; i<5000; ++i) {
            wchar_t time[6]; swprintf_s(time, L"%02d:%02d", (i/60)%24, i%60);
            std::wstring body;
            if (i%19 == 0) {
                for (int j=0,n=1+random()%3;j<n;++j) body += emojis[random()%30].text;
            } else {
                int n = 1+random()%12;
                for (int j=0;j<n;++j) {
                    if (j) body += L'\n';
                    body += L"É uma ação: você trouxe pão? Linha " + std::to_wstring(j+1);
                    if (i%3 == 0 && j==0) {
                        for (int k=0,e=1+random()%3;k<e;++k) { body += L' '; body += emojis[random()%30].text; }
                        body += L" e a conversa continua.";
                    }
                }
            }
            if (i==0) body=L"Fora: ação 🔥 ç ã ê :fora";
            if (i==1) body=L"🔥😀";
            messages.push_back({authors[i%5],time,body}); items.Append(box_value(i));
        }
    }
    void fill(StackPanel const& panel, int i) {
        auto const& m = messages.at(i);
        panel.Children().Clear();
        TextBlock header; header.Text(m.author + L" · " + m.time); header.FontSize(13);
        panel.Children().Append(header);
        RichTextBlock body; body.FontSize(16); body.IsTextSelectionEnabled(true);
        AutomationProperties::SetAutomationId(body,L"body-"+std::to_wstring(i));
        body.TextWrapping(TextWrapping::Wrap);
        Paragraph paragraph;
        std::wstring pending;
        auto flush = [&] { if (!pending.empty()) { Run run; run.Text(pending); paragraph.Inlines().Append(run); pending.clear(); } };
        for (size_t pos=0;pos<m.body.size();) {
            Emoji const* match = nullptr;
            for (auto const& e : emojis) {
                if (m.body.compare(pos, wcslen(e.text), e.text)==0) { match = &e; break; }
            }
            if (match) {
                flush();
                if (unicode) {
                    Run emojiRun; emojiRun.Text(match->text); emojiRun.FontFamily(FontFamily(segoe?L"Segoe UI Emoji":L"ms-appx:///emoji/TwemojiMozilla.ttf#Twemoji Mozilla"));
                    paragraph.Inlines().Append(emojiRun);
                    pos += wcslen(match->text); continue;
                }
                Image image; image.Width(16); image.Height(16); image.VerticalAlignment(VerticalAlignment::Bottom);
                std::wstring uri = L"ms-appx:///emoji/" + std::wstring(match->file) + L".svg";
                image.Source(Imaging::SvgImageSource(Uri(uri)));
                AutomationProperties::SetName(image, match->text);
                InlineUIContainer inlineImage; inlineImage.Child(image); paragraph.Inlines().Append(inlineImage);
                pos += wcslen(match->text);
            } else pending += m.body[pos++];
        }
        flush(); body.Blocks().Append(paragraph); panel.Children().Append(body);
        // This explicit whole-message fallback retains Unicode; partial SVG selection uses WinUI's native copy.
        Button copy; copy.Content(box_value(L"Copiar mensagem inteira"));
        copy.Click([text=m.body](auto const&, auto const&) {
            Windows::ApplicationModel::DataTransfer::DataPackage data; data.SetText(text);
            Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(data);
        });
        panel.Children().Append(copy);
    }
    void updateCount() { count.Text(L"Itens no modelo: " + std::to_wstring(items.Size())); }
    void append() {
        auto text = std::wstring(input.Text()); if (text.empty()) return;
        SYSTEMTIME now{}; GetLocalTime(&now); wchar_t time[6]; swprintf_s(time,L"%02d:%02d",now.wHour,now.wMinute);
        messages.push_back({L"Você",time,text}); items.Append(box_value(static_cast<int>(messages.size()-1)));
        updateCount(); input.Text(L""); list.ScrollIntoView(items.GetAt(items.Size()-1));
    }
    void updateContainer(ListViewBase const&,ContainerContentChangingEventArgs const& args) {
        if (args.InRecycleQueue()) return;
        auto container=args.ItemContainer();
        auto panel=container.ContentTemplateRoot().try_as<StackPanel>();
        if (!panel) {
            args.RegisterUpdateCallback([this](auto const& sender,auto const& next) { updateContainer(sender,next); });
            return;
        }
        auto i=unbox_value<int>(args.Item());
        AutomationProperties::SetName(container,accessible(messages.at(i)));
        fill(panel,i); args.Handled(true);
    }
    void OnLaunched(LaunchActivatedEventArgs const&) {
        Resources().MergedDictionaries().Append(XamlControlsResources());
        window = Window(); window.Title(L"Harmony W1 — telas (offline)");
        Grid root;
        RowDefinition top; top.Height(GridLength{0,GridUnitType::Auto}); root.RowDefinitions().Append(top);
        RowDefinition middle; middle.Height(GridLength{1,GridUnitType::Star}); root.RowDefinitions().Append(middle);
        RowDefinition bottom; bottom.Height(GridLength{0,GridUnitType::Auto}); root.RowDefinitions().Append(bottom);
        StackPanel toolbar; toolbar.Spacing(6); toolbar.Margin(Thickness{12,8,12,8});
        count = TextBlock(); AutomationProperties::SetAutomationId(count,L"model-count"); updateCount(); toolbar.Children().Append(count);
        TextBlock notice; notice.Text(L"Padrão: fonte Twemoji Mozilla embutida, selecionável e copiável. Desmarque para comparar Twemoji SVG."); notice.TextWrapping(TextWrapping::Wrap); toolbar.Children().Append(notice);
        CheckBox mode; mode.Content(box_value(L"Usar emoji como texto / Twemoji Mozilla (desmarque para SVG)"));
        mode.IsChecked(true);
        mode.Click([this,mode](auto const&,auto const&) {
            unicode=mode.IsChecked().Value(); list.ItemsSource(nullptr); list.ItemsSource(items);
        }); toolbar.Children().Append(mode); root.Children().Append(toolbar);
        list=ListView(); list.SelectionMode(ListViewSelectionMode::None);
        AutomationProperties::SetAutomationId(list,L"messages"); AutomationProperties::SetName(list,L"mensagens");
        auto templateObject=Markup::XamlReader::Load(L"<DataTemplate xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation'><StackPanel Spacing='3' Margin='8,6,8,6'/></DataTemplate>");
        list.ItemTemplate(templateObject.as<DataTemplate>());
        list.ContainerContentChanging([this](ListViewBase const& sender,ContainerContentChangingEventArgs const& args) {
            updateContainer(sender,args);
            args.Handled(true);
        });
        list.ItemsSource(items); Grid::SetRow(list,1); root.Children().Append(list);
        input=TextBox(); input.PlaceholderText(L"mensagem — Enter envia; teste á, ã, ê, ç"); input.Margin(Thickness{12,8,12,12});
        AutomationProperties::SetName(input,L"mensagem"); AutomationProperties::SetAutomationId(input,L"composer");
        input.KeyDown([this](auto const&,Input::KeyRoutedEventArgs const& e) {
            if (e.Key()==Windows::System::VirtualKey::Enter) { append(); e.Handled(true); }
        }); Grid::SetRow(input,2); root.Children().Append(input);
        list.Loaded([this](auto const&,auto const&) { loaded=true; });
        window.Content(root); window.Activate();
        rendering=CompositionTarget::Rendering([this](auto const&,IInspectable const& args) { frame(args.as<RenderingEventArgs>().RenderingTime().count()); });
        if (!measurePath.empty() || !testPath.empty()) {
            timeout=DispatcherTimer(); timeout.Interval(std::chrono::minutes(3));
            timeout.Tick([this](auto const&,auto const&) { fail(L"Timed out after three minutes"); }); timeout.Start();
        }
    }
    void fail(std::wstring const& error) {
        if (finished) return;
        auto path = measurePath.empty()?testPath:measurePath;
        if (!path.empty()) {
            std::ofstream out(path); out << "{\"passed\":false,\"error\":" << quote(error)
                << ",\"first_list_rendering_ms\":" << firstMs << ",\"monitor_hz\":" << monitorHz
                << ",\"end_working_set_mb\":" << endMemory.working << ",\"end_private_mb\":" << endMemory.privateMb
                << ",\"foreground_method\":" << quote(foregroundMethod)
                << ",\"desktop_diagnostic\":" << quote(desktopDiagnostic())
                << ",\"scroll_fps\":null,\"worst_interval_ms\":null,\"intervals_over_33_ms\":null}\n";
        }
        finished=true; if (window) window.Close(); else Exit();
    }
    void selfTest() {
        using namespace Microsoft::UI::Xaml::Automation::Peers;
        using namespace Microsoft::UI::Xaml::Automation::Provider;
        // Actual automation peers provide the names; Unicode keyboard input goes through SendInput.
        auto listPeer=FrameworkElementAutomationPeer::CreatePeerForElement(list);
        auto inputPeer=FrameworkElementAutomationPeer::CreatePeerForElement(input);
        auto container=list.ContainerFromIndex(0).as<ListViewItem>();
        auto itemPeer=FrameworkElementAutomationPeer::CreatePeerForElement(container);
        bool listExists=listPeer && items.Size()==5000;
        bool nameCorrect=itemPeer.GetName()==accessible(messages.at(0));
        bool inputName=inputPeer.GetName()==L"mensagem";
        findDeadLayout();
        // Select a genuine partial range through a separate emoji-font Run.
        // Read only the text this test copied, never the previous clipboard content.
        auto panel=container.ContentTemplateRoot().as<StackPanel>();
        auto body=panel.Children().GetAt(1).as<RichTextBlock>();
        auto runs=body.Blocks().GetAt(0).as<Paragraph>().Inlines();
        body.Select(runs.GetAt(0).ContentStart().GetPositionAtOffset(6,LogicalDirection::Forward),
            runs.GetAt(runs.Size()-1).ContentEnd().GetPositionAtOffset(-6,LogicalDirection::Backward));
        testBody=body;
        testList=listExists; testName=nameCorrect; testInputName=inputName;
        HWND ownWindow=nullptr; check_hresult(window.as<IWindowNative>()->get_WindowHandle(&ownWindow));

        phase=6; waitFrames=0;
    }
    RichTextBlock testBody{nullptr};
    std::wstring selectedPartial;
    void copyPartial() {
        testSelection=testBody.SelectedText()==L"ação 🔥 ç ã ê";
        selectedPartial=testBody.SelectedText();
        testBody.CopySelectionToClipboard();
        copiedPartial=readTestClipboard(); testCopy=copiedPartial==L"ação 🔥 ç ã ê";
        auto emojiContainer=list.ContainerFromIndex(1).as<ListViewItem>();
        auto emojiBody=emojiContainer.ContentTemplateRoot().as<StackPanel>().Children().GetAt(1).as<RichTextBlock>();
        auto emojiRuns=emojiBody.Blocks().GetAt(0).as<Paragraph>().Inlines();
        emojiBody.Select(emojiRuns.GetAt(0).ContentStart(),emojiRuns.GetAt(emojiRuns.Size()-1).ContentEnd());
        testBody=emojiBody; phase=7; waitFrames=0;
    }
    void copyEmojiAndInput() {
        testBody.CopySelectionToClipboard();
        copiedEmoji=readTestClipboard();
        testEmojiCopy=copiedEmoji==L"🔥😀" || copiedEmoji==L"🔥😀\r\n";
        HWND ownWindow=nullptr; check_hresult(window.as<IWindowNative>()->get_WindowHandle(&ownWindow));
        testForeground=foreground(ownWindow);
        inputMethod=L"UIA SetFocus + ValuePattern on MTA worker; SendInput VK_RETURN; dead keys via KEYEVENTF_SCANCODE";
        phase=8; waitFrames=0;
        std::thread([this,ownWindow] {
            try {
                init_apartment(apartment_type::multi_threaded);
                com_ptr<IUIAutomation> automation;
                check_hresult(CoCreateInstance(__uuidof(CUIAutomation),nullptr,CLSCTX_INPROC_SERVER,__uuidof(IUIAutomation),automation.put_void()));
                com_ptr<IUIAutomationElement> root;
                check_hresult(automation->ElementFromHandle(ownWindow,root.put()));
                VARIANT id{}; id.vt=VT_BSTR; id.bstrVal=SysAllocString(L"composer");
                com_ptr<IUIAutomationCondition> condition;
                auto result=automation->CreatePropertyCondition(UIA_AutomationIdPropertyId,id,condition.put());
                VariantClear(&id); check_hresult(result);
                com_ptr<IUIAutomationElement> composer;
                check_hresult(root->FindFirst(TreeScope_Descendants,condition.get(),composer.put()));
                if (!composer) throw hresult_error(E_FAIL,L"UIA composer not found");
                check_hresult(composer->SetFocus()); inputFocused=true;
                com_ptr<IUIAutomationValuePattern> value;
                check_hresult(composer->GetCurrentPatternAs(UIA_ValuePatternId,__uuidof(IUIAutomationValuePattern),value.put_void()));
                auto text=SysAllocString(L"ação é ç ã ê");
                auto setResult=value->SetValue(text); SysFreeString(text); check_hresult(setResult); uiaValueSet=true;
            } catch (hresult_error const& e) { inputError=std::wstring(e.message()); }
            uiaReady.store(true);
        }).detach();
    }
    std::wstring foregroundMethod;
    std::wstring desktopName(HANDLE desktop) {
        wchar_t name[256]{}; DWORD needed=0;
        if (!desktop || !GetUserObjectInformationW(desktop,UOI_NAME,name,sizeof(name),&needed)) return L"not found";
        return name;
    }
    std::wstring desktopDiagnostic() {
        auto current=desktopName(GetThreadDesktop(GetCurrentThreadId()));
        auto inputDesktop=OpenInputDesktop(0,FALSE,DESKTOP_READOBJECTS);
        auto inputName=desktopName(inputDesktop); if (inputDesktop) CloseDesktop(inputDesktop);
        return L"Thread desktop: "+current+L"; input desktop: "+inputName+L"; foreground HWND: "+std::to_wstring(reinterpret_cast<uintptr_t>(GetForegroundWindow()));
    }
    bool foreground(HWND own) {
        foregroundRequested=SetForegroundWindow(own)!=FALSE;
        foregroundMethod=L"SetForegroundWindow";
        if (GetForegroundWindow()==own) return true;
        auto current=GetCurrentThreadId();
        auto other=GetWindowThreadProcessId(GetForegroundWindow(),nullptr);
        bool attached=other && other!=current && AttachThreadInput(current,other,TRUE);
        foregroundMethod+=attached?L"; AttachThreadInput attached":L"; AttachThreadInput unavailable (no foreground thread or denied)";
        if (attached) {
            SetForegroundWindow(own); BringWindowToTop(own); SetFocus(own);
            AttachThreadInput(current,other,FALSE); foregroundMethod=L"AttachThreadInput + SetForegroundWindow";
        }
        if (GetForegroundWindow()==own) return true;
        auto inputDesktop=OpenInputDesktop(0,FALSE,DESKTOP_READOBJECTS);
        bool sameDesktop=inputDesktop && desktopName(inputDesktop)==desktopName(GetThreadDesktop(current));
        if (inputDesktop) CloseDesktop(inputDesktop);
        if (!sameDesktop) { foregroundMethod+=L"; own-window click blocked: input desktop differs"; return false; }
        RECT r{}; GetWindowRect(own,&r);
        // Click our title bar only; never inject a keyboard event into another app.
        SetCursorPos(r.left+80,r.top+12);
        INPUT clicks[2]{}; clicks[0].type=clicks[1].type=INPUT_MOUSE;
        clicks[0].mi.dwFlags=MOUSEEVENTF_LEFTDOWN; clicks[1].mi.dwFlags=MOUSEEVENTF_LEFTUP;
        SendInput(2,clicks,sizeof(INPUT)); foregroundMethod=L"SendInput click on own title bar";
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        return GetForegroundWindow()==own;
    }
    void sendEnter() {
        testAccents=uiaValueSet && input.Text()==L"ação é ç ã ê";
        HWND own=nullptr; check_hresult(window.as<IWindowNative>()->get_WindowHandle(&own));
        testForeground=GetForegroundWindow()==own || foreground(own);
        if (!testForeground || !inputFocused) { inputError+=L" Foreground/focus unavailable before Enter"; writeTest(); return; }
        INPUT keys[2]{}; keys[0].type=keys[1].type=INPUT_KEYBOARD;
        keys[0].ki.wVk=keys[1].ki.wVk=VK_RETURN; keys[1].ki.dwFlags=KEYEVENTF_KEYUP;
        testSent=SendInput(2,keys,sizeof(INPUT))==2;
        phase=10; waitFrames=0;
    }
    bool appended=false, deadInstalled=false, deadSent=false, deadPassed=false, deadAttempted=false;
    HKL originalLayout=nullptr, deadLayout=nullptr;
    std::wstring deadText;
    void findDeadLayout() {
        int layoutCount=GetKeyboardLayoutList(0,nullptr); std::vector<HKL> layouts(layoutCount);
        GetKeyboardLayoutList(layoutCount,layouts.data());
        // ABNT2's KLID is 00010416; its loaded HKL usually uses F0020416.
        for (auto layout:layouts) {
            originalLayout=GetKeyboardLayout(0); ActivateKeyboardLayout(layout,0);
            wchar_t name[KL_NAMELENGTH]{}; GetKeyboardLayoutNameW(name);
            ActivateKeyboardLayout(originalLayout,0);
            if (std::wstring(name)==L"00010416") deadLayout=layout;
        }
        deadInstalled=deadLayout!=nullptr;
    }
    void startDeadKeys() {
        appended=items.Size()==5001 && messages.back().body==L"ação é ç ã ê" && input.Text().empty();
        if (!deadInstalled) { writeTest(); return; }
        deadAttempted=true;
        originalLayout=GetKeyboardLayout(0); ActivateKeyboardLayout(deadLayout,0);
        input.Focus(FocusState::Programmatic);
        std::vector<INPUT> keys;
        auto scan=[&](WORD code,bool up=false) { INPUT k{}; k.type=INPUT_KEYBOARD; k.ki.wScan=code; k.ki.dwFlags=KEYEVENTF_SCANCODE|(up?KEYEVENTF_KEYUP:0); keys.push_back(k); };
        auto tap=[&](WORD code) { scan(code); scan(code,true); };
        tap(0x1a); tap(0x1e); tap(0x39); // acute + a, space
        tap(0x28); tap(0x1e); tap(0x39); // tilde + a, space
        scan(0x2a); tap(0x28); scan(0x2a,true); tap(0x12); tap(0x39); // circumflex + e
        tap(0x27); // cedilla
        deadSent=SendInput(static_cast<UINT>(keys.size()),keys.data(),sizeof(INPUT))==keys.size();
        phase=11; waitFrames=0;
    }
    std::wstring readTestClipboard() {
        check_bool(OpenClipboard(nullptr));
        auto handle=GetClipboardData(CF_UNICODETEXT);
        if (!handle) { CloseClipboard(); throw hresult_error(HRESULT_FROM_WIN32(GetLastError()),L"CF_UNICODETEXT unavailable after native copy"); }
        auto text=static_cast<wchar_t const*>(GlobalLock(handle));
        if (!text) { CloseClipboard(); throw hresult_error(E_FAIL,L"Clipboard GlobalLock failed"); }
        std::wstring result(text); GlobalUnlock(handle); CloseClipboard(); return result;
    }
    bool testList=false,testName=false,testInputName=false,testAccents=false,testSent=false;
    bool testUnicodeSent=false;
    bool foregroundRequested=false,testForeground=false;
    bool inputFocused=false;
    std::atomic<bool> uiaReady=false;
    bool uiaValueSet=false;
    HWND testWindow=nullptr;
    std::wstring testWindowClass;
    std::wstring inputMethod=L"SendInput KEYEVENTF_UNICODE; Enter via SendInput";
    std::wstring inputError;
    bool testSelection=false,testCopy=false,testEmojiCopy=false;
    std::wstring copiedPartial,copiedEmoji;
    void writeTest() {
        // appended was verified before starting the dead-key sequence.
        if (!appended && !testForeground && inputError.empty()) inputError=L"Own window cannot become foreground; SendInput Enter was not sent";
        bool passed=testList&&testName&&testInputName&&testAccents&&testSent&&appended&&testSelection&&testCopy&&testEmojiCopy&&(!deadInstalled||deadPassed);
        std::ofstream out(testPath); out << std::boolalpha << "{\"passed\":" << passed
            << ",\"list_exists_and_model_5000\":" << testList << ",\"item_accessible_name\":" << testName
            << ",\"composer_accessible_name\":" << testInputName << ",\"unicode_input_preserved\":" << testAccents
            << ",\"enter_event_sent\":" << testSent << ",\"external_uia_valuepattern\":" << uiaValueSet << ",\"model_5001\":" << appended
            << ",\"partial_selection_across_emoji_run\":" << testSelection << ",\"native_clipboard_partial_unicode\":" << testCopy
            << ",\"native_clipboard_emoji_only\":" << testEmojiCopy << ",\"copied_partial\":" << quote(copiedPartial)
            << ",\"copied_emoji_only\":" << quote(copiedEmoji)
            << ",\"setforegroundwindow_succeeded\":" << foregroundRequested << ",\"own_window_foreground\":" << testForeground
            << ",\"input_error\":" << quote(inputError)
            << ",\"selected_partial\":" << quote(selectedPartial)
            << ",\"composer_focus_succeeded\":" << inputFocused
            << ",\"enter_target_window_class\":" << quote(testWindowClass)
            << ",\"foreground_method\":" << quote(foregroundMethod) << ",\"dead_keys_abnt2_installed\":" << deadInstalled << ",\"dead_keys_scancodes_sent\":" << deadSent << ",\"dead_keys_passed\":" << deadPassed << ",\"dead_keys_text\":" << quote(deadText)
            << ",\"emoji_mode\":" << quote(segoe?L"Segoe UI Emoji text Runs":L"Twemoji Mozilla embedded COLR/CPAL text Runs")
            << ",\"copy_method\":\"RichTextBlock.Select (partial range through emoji Run) + CopySelectionToClipboard; Win32 CF_UNICODETEXT readback\""
            << ",\"desktop_diagnostic\":" << quote(desktopDiagnostic())
            << ",\"input_method\":" << quote(inputMethod) << ",\"dead_keys_tested\":" << deadAttempted << ",\"screen_reader_tested\":false}\n";
        out.close(); finished=true; window.Close();
    }
    void frame(int64_t renderingTime) {
        try {
            if (!loaded || finished || list.ActualHeight()<=0) return;
            if (phase==0) {
                auto first=list.ContainerFromIndex(0).try_as<ListViewItem>(); if (!first) return;
                auto panel=first.ContentTemplateRoot().try_as<StackPanel>();
                if (!panel || panel.Children().Size()<2) return;
                auto body=panel.Children().GetAt(1).try_as<RichTextBlock>();
                if (!body || body.ActualHeight()<=0) return;
                firstBodyHeight=body.ActualHeight();
            }
            auto now=Clock::now();
            if (phase==0) {
                // Rendering is a pre-render notification, not an OS presentation timestamp.
                firstMs=processAgeMs(); phase=1; waitFrames=0;
            }
            if (phase==6) { if (++waitFrames>=2) copyPartial(); return; }
            if (phase==7) { if (++waitFrames>=2) copyEmojiAndInput(); return; }
            if (phase==8) { if (uiaReady.load()) sendEnter(); return; }
            if (phase==9) { if (++waitFrames>=10) sendEnter(); return; }
            if (phase==10) { if (++waitFrames>=20) startDeadKeys(); return; }
            if (phase==11) { if (++waitFrames>=20) { deadText=input.Text(); deadPassed=deadSent && deadText==L"á ã ê ç"; ActivateKeyboardLayout(originalLayout,0); writeTest(); } return; }
            if (!testPath.empty()) { if (++waitFrames==10) selfTest(); return; }
            if (measurePath.empty()) { CompositionTarget::Rendering(rendering); return; }
            if (phase==1) {
                if (++waitFrames==10) { list.ScrollIntoView(items.GetAt(4999)); phase=2; waitFrames=0; }
            } else if (phase==2) {
                if (++waitFrames==20) { endMemory=memory(); list.ScrollIntoView(items.GetAt(0)); phase=3; waitFrames=0; }
            } else if (phase==3) {
                if (++waitFrames==20) beginScroll();
            } else if (phase==4 || phase==12) {
                recordRendering(renderingTime);
            }
        } catch (hresult_error const& e) { fail(std::wstring(e.message())); }
          catch (std::exception const& e) { fail(std::wstring(to_hstring(e.what()))); }
    }
    struct Samples {
        uint64_t frames=0, slow=0; double sum=0,worst=0,duration=0;
        int64_t previous=-1; double startOffset=0,turnOffset=0,endOffset=0,extent=0;
    } animationSamples, wheelSamples;
    ScrollViewer scroll{nullptr}; DispatcherTimer driver{nullptr};
    Clock::time_point scrollStarted; bool turned=false;
    std::atomic<bool> wheelDone=false, wheelOk=true; uint64_t wheelClicks=0;
    double monitorHz=0;
    ScrollViewer findScroll(DependencyObject const& parent) {
        if (auto viewer=parent.try_as<ScrollViewer>()) return viewer;
        for (int i=0;i<VisualTreeHelper::GetChildrenCount(parent);++i)
            if (auto viewer=findScroll(VisualTreeHelper::GetChild(parent,i))) return viewer;
        return nullptr;
    }
    void recordRendering(int64_t time) {
        auto& sample=phase==4?animationSamples:wheelSamples;
        if (sample.previous==time) return;
        if (sample.previous>=0) {
            double ms=(time-sample.previous)/10000.0;
            sample.sum+=ms; sample.worst=std::max(sample.worst,ms); if (ms>33) ++sample.slow;
        }
        sample.previous=time; ++sample.frames;
    }
    void beginScroll() {
        HWND own=nullptr; check_hresult(window.as<IWindowNative>()->get_WindowHandle(&own));
        MONITORINFOEXW info{}; info.cbSize=sizeof(info);
        if (GetMonitorInfoW(MonitorFromWindow(own,MONITOR_DEFAULTTONEAREST),&info)) {
            DEVMODEW mode{}; mode.dmSize=sizeof(mode);
            if (EnumDisplaySettingsW(info.szDevice,ENUM_CURRENT_SETTINGS,&mode)) monitorHz=mode.dmDisplayFrequency;
        }
        testForeground=foreground(own);
        if (!testForeground) { fail(L"Scroll requires foreground; SetForegroundWindow/AttachThreadInput failed; own-window click requires the same input desktop"); return; }
        scroll=findScroll(list); if (!scroll) { fail(L"ListView ScrollViewer not found"); return; }
        animationSamples.startOffset=scroll.VerticalOffset(); animationSamples.extent=scroll.ScrollableHeight();
        scrollStarted=Clock::now(); phase=4; turned=false;
        driver=DispatcherTimer(); driver.Interval(std::chrono::milliseconds(16));
        driver.Tick([this,own](auto const&,auto const&) {
            try {
                if (GetForegroundWindow()!=own) { driver.Stop(); fail(L"Foreground lost during scroll"); return; }
                double seconds=std::chrono::duration<double>(Clock::now()-scrollStarted).count();
                if (phase==4) {
                    // Time-driven animation, no wait for individual item realization.
                    double fraction=seconds<5?seconds/5:seconds<5.5?1:std::max(0.0,1-(seconds-5.5)/5);
                    scroll.ChangeView(nullptr,scroll.ScrollableHeight()*fraction,nullptr,false);
                    if (seconds>=5.5 && !turned) { animationSamples.turnOffset=scroll.VerticalOffset(); turned=true; }
                    if (seconds>=11) {
                        animationSamples.duration=seconds*1000; animationSamples.endOffset=scroll.VerticalOffset();
                        afterMemory=memory(); phase=12; wheelSamples.startOffset=scroll.VerticalOffset();
                        wheelSamples.extent=scroll.ScrollableHeight(); scrollStarted=Clock::now();
                        RECT r{}; GetWindowRect(own,&r); SetCursorPos((r.left+r.right)/2,(r.top+r.bottom)/2);
                        std::thread([this,own] {
                            auto start=Clock::now();
                            for (int i=0;i<600;++i) {
                                std::this_thread::sleep_until(start+std::chrono::microseconds(i*1000000LL/60));
                                if (GetForegroundWindow()!=own) { wheelOk=false; break; }
                                INPUT event{}; event.type=INPUT_MOUSE; event.mi.dwFlags=MOUSEEVENTF_WHEEL;
                                event.mi.mouseData=static_cast<DWORD>(i<300?-WHEEL_DELTA:WHEEL_DELTA);
                                if (SendInput(1,&event,sizeof(INPUT))!=1) { wheelOk=false; break; }
                                ++wheelClicks;
                            }
                            wheelDone.store(true);
                        }).detach();
                    }
                } else if (phase==12) {
                    if (seconds>=5 && wheelSamples.turnOffset==0) wheelSamples.turnOffset=scroll.VerticalOffset();
                    if (wheelDone.load() && seconds>=10.5) {
                        driver.Stop(); wheelSamples.duration=seconds*1000; wheelSamples.endOffset=scroll.VerticalOffset(); writeMeasure();
                    }
                }
            } catch (hresult_error const& e) { driver.Stop(); fail(std::wstring(e.message())); }
        }); driver.Start();
    }
    void sampleJson(std::ostream& out, Samples const& v) {
        out << "{\"distinct_rendering_frames\":" << v.frames << ",\"duration_ms\":" << v.duration
            << ",\"fps\":" << (v.frames>1 && v.sum>0?(v.frames-1)*1000/v.sum:0)
            << ",\"worst_interval_ms\":" << v.worst << ",\"intervals_over_33_ms\":" << v.slow
            << ",\"start_offset\":" << v.startOffset << ",\"turn_offset\":" << v.turnOffset
            << ",\"end_offset\":" << v.endOffset << ",\"initial_scrollable_height\":" << v.extent << '}';
    }
    void writeMeasure() {
        uintmax_t total=0; std::vector<std::pair<uintmax_t,std::wstring>> dlls;
        for (auto const& entry:fs::recursive_directory_iterator(executableDirectory())) {
            if (!entry.is_regular_file()) continue;
            auto bytes=entry.file_size(); total+=bytes;
            if (entry.path().extension()==L".dll") dlls.emplace_back(bytes,fs::relative(entry.path(),executableDirectory()).wstring());
        }
        std::sort(dlls.rbegin(),dlls.rend());
        std::ofstream out(measurePath); out << std::fixed << std::setprecision(3)
            << "{\"measurement\":\"Distinct RenderingEventArgs.RenderingTime (requested frame proxy, not GPU presentation confirmation)\","
            << "\"first_list_rendering_ms\":" << firstMs << ",\"model_count\":" << items.Size()
            << ",\"first_message_body_height\":" << firstBodyHeight
            << ",\"monitor_hz\":" << monitorHz << ",\"own_window_foreground\":" << std::boolalpha << testForeground
            << ",\"foreground_method\":" << quote(foregroundMethod)
            << ",\"end_working_set_mb\":" << endMemory.working << ",\"end_private_mb\":" << endMemory.privateMb
            << ",\"after_roundtrip_working_set_mb\":" << afterMemory.working << ",\"after_roundtrip_private_mb\":" << afterMemory.privateMb
            << ",\"animation\":"; sampleJson(out,animationSamples);
        out << ",\"wheel\":"; sampleJson(out,wheelSamples);
        out << ",\"wheel_clicks_sent\":" << wheelClicks << ",\"wheel_sendinput_ok\":" << wheelOk.load()
            << ",\"wheel_delta\":120,\"wheel_requested_clicks_per_second\":60"
            << ",\"output_folder_mb\":" << total/1048576.0 << ",\"largest_dlls\":[";
        for (size_t i=0;i<std::min<size_t>(10,dlls.size());++i) {
            if (i) out << ',';
            out << "{\"name\":" << quote(dlls[i].second) << ",\"mb\":" << dlls[i].first/1048576.0 << '}';
        }
        out << "]}\n"; out.close(); finished=true; window.Close();
    }
};

int __stdcall wWinMain(HINSTANCE,HINSTANCE,PWSTR,int) {
    try {
        init_apartment(apartment_type::single_threaded);
        Application::Start([](auto const&) { make<App>(); }); return 0;
    } catch (hresult_error const& e) {
        std::ofstream out(executableDirectory()/L"telas.runtime-error.txt");
        out << "HRESULT 0x" << std::hex << static_cast<uint32_t>(e.code().value) << ": " << to_string(e.message()); return 1;
    } catch (std::exception const& e) {
        std::ofstream out(executableDirectory()/L"telas.runtime-error.txt"); out << e.what(); return 1;
    }
}
