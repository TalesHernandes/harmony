// File selection is the owner's explicit exception to the custom-control rule.
// Keep the operating system's navigation, multi-selection and accessibility.
winrt::Windows::Foundation::IAsyncAction PickFiles(std::wstring title,bool photos,
    std::function<void(std::vector<std::filesystem::path>)> chosen,std::filesystem::path initial={}) {
  auto lifetime=get_strong();
  com_ptr<IFileOpenDialog> dialog;
  if(FAILED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(dialog.put())))) co_return;
  DWORD options{};dialog->GetOptions(&options);
  dialog->SetOptions(options|FOS_FILEMUSTEXIST|FOS_FORCEFILESYSTEM|(photos?0:FOS_ALLOWMULTISELECT));
  dialog->SetTitle(title.c_str());
  COMDLG_FILTERSPEC pictures[]={{L"JPEG, PNG, GIF",L"*.jpg;*.jpeg;*.png;*.gif"}};
  COMDLG_FILTERSPEC files[]={{L"Imagem, PDF, som, v\u00eddeo ou zip",L"*.png;*.jpg;*.jpeg;*.gif;*.webp;*.pdf;*.txt;*.mp3;*.ogg;*.wav;*.m4a;*.opus;*.mp4;*.webm;*.zip"},{L"Todos os arquivos",L"*.*"}};
  if(photos) dialog->SetFileTypes(1,pictures);else dialog->SetFileTypes(2,files);
  if(!initial.empty()) {
    std::error_code error; auto folder=initial;
    if(std::filesystem::is_regular_file(initial,error)) {dialog->SetFileName(initial.filename().c_str());folder=initial.parent_path();}
    com_ptr<IShellItem> item;
    if(SUCCEEDED(SHCreateItemFromParsingName(folder.c_str(),nullptr,IID_PPV_ARGS(item.put())))) dialog->SetFolder(item.get());
  }
  if(FAILED(dialog->Show(hwnd))) co_return;
  com_ptr<IShellItemArray> items;if(FAILED(dialog->GetResults(items.put()))) co_return;
  DWORD count=0;items->GetCount(&count);std::vector<std::filesystem::path> paths;
  for(DWORD i=0;i<count;++i) {
    com_ptr<IShellItem> item;PWSTR path{};
    if(SUCCEEDED(items->GetItemAt(i,item.put())) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&path))) {paths.emplace_back(path);CoTaskMemFree(path);}
  }
  if(!paths.empty()) chosen(std::move(paths));
  co_return;
}
#ifndef HARMONY_PUBLISH
winrt::Windows::Foundation::IAsyncAction AuditSystemFilePicker(bool photos=false) {
  if(!AuditRealPointer()) {logf("system %s file picker activation: skipped (HARMONY_AUDIT_REAL_POINTER)",photos?"photo":"attachment");co_return;}
  AuditRequireRealPointer();
  auto lifetime=get_strong();
  auto fixture=photos?std::filesystem::path(net::Wide(app::Environment(L"HARMONY_TEST_PICTURE"))):chatOutput/L"round6-file.txt";
  if(!photos) std::ofstream(fixture)<<"local file chooser fixture";
  auto found=std::make_shared<std::atomic<bool>>(false); auto selected=std::make_shared<bool>(false);
  DWORD thread=GetCurrentThreadId();auto owner=hwnd;auto output=chatOutput;
  std::thread input([thread,owner,output,photos,found] {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    struct Target {HWND owner,dialog=nullptr;} target{owner};
    for(int attempt=0;attempt<150 && !target.dialog;++attempt) {
      EnumThreadWindows(thread,[](HWND candidate,LPARAM data)->BOOL {
        auto& target=*reinterpret_cast<Target*>(data); wchar_t kind[64]{},title[128]{};
        GetClassNameW(candidate,kind,64);GetWindowTextW(candidate,title,128);
        if(GetWindow(candidate,GW_OWNER)==target.owner && IsWindowVisible(candidate) && std::wstring(kind)==L"#32770" && std::wstring(title)==L"Harmony file selection audit") {target.dialog=candidate;return FALSE;}return TRUE;
      },reinterpret_cast<LPARAM>(&target));
      if(!target.dialog) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if(target.dialog) {
      *found=true;std::this_thread::sleep_for(std::chrono::milliseconds(250));
      try {SaveDesignWindow(target.dialog,output,photos?L"round6-system-photo-picker.png":L"round6-system-file-picker.png").get();}catch(...) {}
      PostMessageW(target.dialog,WM_COMMAND,IDOK,0);
      std::this_thread::sleep_for(std::chrono::milliseconds(1000));
      if(IsWindow(target.dialog)) PostMessageW(target.dialog,WM_CLOSE,0,0);
    }
    winrt::uninit_apartment();
  });
  co_await PickFiles(L"Harmony file selection audit",photos,[selected,fixture](auto files) {
    std::error_code error;*selected=files.size()==1 && std::filesystem::equivalent(files.front(),fixture,error);
  },fixture);
  input.join();ParityRequire(*found && *selected);
}

#endif
