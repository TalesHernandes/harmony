app::DiagnosticHistory diagnosticHistory;
// The performance screen (the owner, 22/10/2026: the charts "não estão legais"
// and sending photos of it "não é legal"): four numbers you can read at a
// glance, each with ten minutes drawn small under it; a button that sends
// the whole report to the server, where Claude reads it; then where the
// memory goes in words of use, and the raw counters folded away.
struct DiagnosticTile { TextBlock value{nullptr},note{nullptr}; Canvas spark{nullptr}; Shapes::Path line{nullptr}; };
std::vector<DiagnosticTile> diagnosticTiles;
std::vector<TextBlock> resourceValues;
TextBlock diagnosticStatus{nullptr}; Button diagnosticSend{nullptr};
// "Gravar o áudio da próxima chamada": the switch follows the recorder, which
// turns itself off when the minute is over.
ToggleSwitch recordingToggle{nullptr}; TextBlock recordingStatus{nullptr}; bool syncingRecording=false;
std::filesystem::path RecordingRoot() { return app::DataDirectory()/L"gravacoes"; }
// "Preparar para enviar" (the owner, 29/10/2026: the switch is for everyone):
// a friend's PC is not one Claude can read, so the newest minute becomes one
// .zip in Downloads that they send to whoever asked, as Android shares its.
TextBlock recordingShare{nullptr}; bool recordingPacking=false;
std::filesystem::path NewestRecording() {
  std::filesystem::path newest; std::error_code error;
  for (auto& entry : std::filesystem::directory_iterator(RecordingRoot(), error))
    if (entry.is_directory() && entry.path().filename().wstring().starts_with(L"chamada-") &&
        std::filesystem::is_regular_file(entry.path()/L"numeros.txt") && entry.path() > newest) newest = entry.path();
  return newest;
}
// Windows' own tar (System32, since 1803) writes the .zip: no library added.
static bool ZipRecording(std::filesystem::path const& source, std::filesystem::path const& target) {
  wchar_t system[MAX_PATH]; if (!GetSystemDirectoryW(system, MAX_PATH)) return false;
  auto tar = std::filesystem::path(system)/L"tar.exe";
  std::wstring command = L"\"" + tar.wstring() + L"\" -a -c -f \"" + target.wstring() + L"\" -C \"" + source.wstring() + L"\" eco.aecdump enviado.wav numeros.txt";
  STARTUPINFOW start{sizeof start}; PROCESS_INFORMATION process{};
  if (!CreateProcessW(tar.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &start, &process)) return false;
  WaitForSingleObject(process.hProcess, 120000); DWORD code = 1; GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hThread); CloseHandle(process.hProcess);
  std::error_code error; return code == 0 && std::filesystem::file_size(target, error) > 0 && !error;
}
void ShareNote(std::wstring const& text, uint32_t tone) {
  if (!recordingShare) return; recordingShare.Foreground(Brush(tone)); recordingShare.Text(text);
  recordingShare.Visibility(text.empty() ? Visibility::Collapsed : Visibility::Visible);
}
void PackRecording() {
  if (recordingPacking) return;
  if (audio::ReadRecording().state == audio::RecordingStatus::Recording) { ShareNote(L"Espere a gravação terminar.", Muted); return; }
  auto source = NewestRecording();
  if (source.empty()) { ShareNote(L"Ainda não há gravação para enviar.", Muted); return; }
  PWSTR folder{}; std::filesystem::path downloads;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &folder))) downloads = folder;
  if (folder) CoTaskMemFree(folder);
  if (downloads.empty()) { ShareNote(L"Não foi possível achar a pasta Downloads.", 0xf5645b); return; }
  auto target = downloads/(L"harmony-" + source.filename().wstring() + L".zip");
  for (int n = 2; std::filesystem::exists(target) && n < 100; ++n) target = downloads/(L"harmony-" + source.filename().wstring() + L" (" + std::to_wstring(n) + L").zip");
  recordingPacking = true; ShareNote(L"Preparando…", Muted);
  logf("call recording packing");
  auto dispatcher = window.DispatcherQueue(); auto weak = get_weak();
  std::thread([dispatcher, weak, source, target] {
    bool ok = ZipRecording(source, target);
    std::error_code error; auto bytes = ok ? std::filesystem::file_size(target, error) : 0;
    if (!ok) std::filesystem::remove(target, error);
    dispatcher.TryEnqueue([weak, ok, bytes, target] {
      auto self = weak.get(); if (!self) return;
      self->recordingPacking = false;
      logf("call recording packed ok=%d bytes=%llu", ok ? 1 : 0, (unsigned long long)bytes);
      if (!ok) { self->ShareNote(L"Não foi possível preparar o arquivo.", 0xf5645b); return; }
      self->ShareNote(L"Pronto: " + target.filename().wstring() + L" (" + self->Bytes(bytes) + L") está em Downloads. Mande pelo WhatsApp ou por e-mail a quem pediu.", Success);
      std::wstring args = L"/select,\"" + target.wstring() + L"\"";
      ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    });
  }).detach();
}
std::wstring Metric(double value,wchar_t const* unit) {wchar_t text[100];swprintf_s(text,L"%.1f %s",value,unit);return text;}
std::wstring Whole(double value,wchar_t const* unit) {wchar_t text[100];swprintf_s(text,L"%.0f %s",value,unit);return text;}
std::wstring Bytes(uint64_t bytes){return Whole(double(bytes)/(1024*1024),L"MB");}
// The full-size chart, still used by the network tab.
struct HistoryChart {
  TextBlock value{nullptr},maximum{nullptr};Canvas plot{nullptr};
  Shapes::Path line{nullptr},area{nullptr};
  std::array<TextBlock,3> ticks;std::array<Shapes::Line,3> grid;
  TextBlock start{nullptr},end{nullptr};
  std::wstring unit;
};
HistoryChart Chart(StackPanel body,std::wstring title,std::wstring unit) {
  StackPanel words;words.Spacing(8);words.Children().Append(Text(title,13,Muted));
  HistoryChart chart;chart.unit=unit;Grid heading;ColumnDefinition fill;fill.Width(GridLength{1,GridUnitType::Star});heading.ColumnDefinitions().Append(fill);ColumnDefinition fit;fit.Width(GridLength{1,GridUnitType::Auto});heading.ColumnDefinitions().Append(fit);
  chart.value=Text(L"—",15);heading.Children().Append(chart.value);chart.maximum=Text(L"Máx. —",13,Muted);Grid::SetColumn(chart.maximum,1);heading.Children().Append(chart.maximum);words.Children().Append(heading);
  chart.plot=Canvas();chart.plot.Height(130);chart.plot.HorizontalAlignment(HorizontalAlignment::Stretch);chart.plot.Background(Brush(0x232325));
  for(size_t i=0;i<3;++i){chart.grid[i]=Shapes::Line();chart.grid[i].Stroke(Tint(0x52525b,.28));chart.grid[i].StrokeThickness(1);chart.plot.Children().Append(chart.grid[i]);chart.ticks[i]=Text(L"",12,Muted);chart.plot.Children().Append(chart.ticks[i]);Canvas::SetLeft(chart.ticks[i],6);}
  chart.area=Shapes::Path();chart.area.Fill(Tint(Accent,.16));chart.area.IsHitTestVisible(false);chart.plot.Children().Append(chart.area);
  chart.line=Shapes::Path();chart.line.Stroke(Brush(Accent));chart.line.StrokeThickness(2);chart.line.StrokeLineJoin(PenLineJoin::Round);chart.line.IsHitTestVisible(false);chart.plot.Children().Append(chart.line);
  chart.start=Text(L"Desde a abertura",12,Muted);chart.end=Text(L"agora",12,Muted);chart.plot.Children().Append(chart.start);chart.plot.Children().Append(chart.end);
  words.Children().Append(chart.plot);body.Children().Append(words);return chart;
}
void Plot(HistoryChart const& chart,std::vector<std::pair<double,double>> const& values,double maximum,std::wstring current,std::wstring startLabel=L"Desde a abertura") {
  chart.value.Text(current);double observed=0;bool hasValue=false;for(auto [at,v]:values)if(v>=0){observed=std::max(observed,v);hasValue=true;}
  chart.maximum.Text(!hasValue?L"Máx. —":L"Máx. "+Metric(observed,chart.unit.c_str()));
  double width=chart.plot.ActualWidth();if(width<=80)return;
  maximum=std::max(1.,maximum);double magnitude=std::pow(10.,std::floor(std::log10(maximum)));double steps=maximum/magnitude;
  maximum=(steps<=1?1:steps<=2?2:steps<=5?5:10)*magnitude;double left=64,right=width-12,top=8,bottom=98;
  for(size_t i=0;i<3;++i){double y=top+(bottom-top)*i/2;auto line=chart.grid[i];line.X1(left);line.X2(right);line.Y1(y);line.Y2(y);chart.ticks[i].Text(Metric(maximum*(2-i)/2,L""));Canvas::SetTop(chart.ticks[i],y-5);}
  Canvas::SetLeft(chart.start,left);Canvas::SetTop(chart.start,108);Canvas::SetLeft(chart.end,std::max(left,right-36));Canvas::SetTop(chart.end,108);
  Media::PathGeometry stroke,fill;Media::PathFigure part{nullptr},shade{nullptr};double last=0;
  auto close=[&]{if(shade){Media::LineSegment down;down.Point({float(last),float(bottom)});shade.Segments().Append(down);shade.IsClosed(true);fill.Figures().Append(shade);stroke.Figures().Append(part);shade=nullptr;part=nullptr;}};
  if(hasValue) {
    auto first=std::find_if(values.begin(),values.end(),[](auto const& p){return p.second>=0;});
    double end=values.back().first,begin=std::max(first->first,end-600.);double span=std::max(2.,end-begin),previous=0;
    chart.start.Text(end-begin>=598?L"10 min":startLabel);
    for(auto [at,value]:values) {
      if(value<0 || at<begin){close();previous=0;continue;}
      if(previous && at-previous>3)close();
      float x=float(left+std::clamp((at-begin)/span,0.,1.)*(right-left)),y=float(bottom-std::clamp(value/maximum,0.,1.)*(bottom-top));
      if(!part){part=Media::PathFigure();part.StartPoint({x,y});part.IsFilled(false);shade=Media::PathFigure();shade.StartPoint({x,float(bottom)});}
      else{Media::LineSegment point;point.Point({x,y});part.Segments().Append(point);}
      Media::LineSegment point;point.Point({x,y});shade.Segments().Append(point);last=x;previous=at;
    }
    close();
  }
  chart.line.Data(stroke);chart.area.Data(fill);
}
static constexpr std::array<wchar_t const*,15> kResourceLabels{
  L"Memória do programa",L"Em uso agora na RAM",L"Imagens guardadas",L"Imagens desenhadas na tela",L"Fotos de perfil",L"Emoji",
  L"Mensagens carregadas",L"Voz (motor da chamada)",L"Filtro de ruído",L"Câmera",L"Câmera (processo separado)",
  L"Tarefas do programa",L"Recursos do Windows (handles)",L"Objetos de desenho do Windows",L"Custo desta medição"};
// The table's values, in the order of kResourceLabels. Channel names are left
// out on purpose: the report travels, and a DM's name is a person's name.
std::array<std::wstring,15> ResourceTexts(app::DiagnosticSample const& sample) {
  auto& p=sample.process;auto& r=sample.resources;std::array<std::wstring,15> out;
  auto cache=[this](uint64_t n,uint64_t b){return std::to_wstring(n)+L" · "+Bytes(b);};
  out[0]=Bytes(p.private_bytes);out[1]=Bytes(p.working_set_bytes);
  out[2]=cache(r.encodedCount,r.encodedBytes)+L" (teto 64 MB)";out[3]=cache(r.decodedCount,r.decodedBytes)+L" (teto 32 MB)";
  out[4]=cache(r.avatarCount,r.avatarBytes);out[5]=cache(r.emojiCount,r.emojiBytes);
  size_t messages=0;uint64_t messageBytes=0;for(auto [n,b]:r.channels){messages+=n;messageBytes+=b;}
  out[6]=std::to_wstring(messages)+L" em "+std::to_wstring(r.channels.size())+L" conversas · "+Bytes(messageBytes);
  // Voice and the filter live inside libwebrtc's own allocations: when the
  // load delta is not measurable, say "ligado" rather than a false 0 MB.
  for(size_t i=0;i<3;++i){auto& part=r.parts[i];out[7+i]=!part.loaded?(i==2?L"Desligada":L"Desligado"):part.measured&&part.estimate>=(1<<20)?L"Ligado · ~"+Bytes(part.estimate):L"Ligado · dentro da memória do programa";}
  out[10]=r.cameraPrivate?Bytes(r.cameraPrivate):L"Desligada";
  std::wstring threads=std::to_wstring(p.thread_count)+L" tarefas";size_t shown=0;
  for(auto& t:p.top_threads){if(shown++==3)break;threads+=L"\n"+net::Wide(t.name)+L" · "+Metric(t.total_cpu_ms/1000.,L"s de CPU");}
  out[11]=threads;out[12]=std::to_wstring(p.handles);out[13]=std::to_wstring(p.gdi_objects)+L" / "+std::to_wstring(p.user_objects);
  out[14]=Metric(double(diagnosticHistory.Cpu100ns())/10000.,L"ms de CPU")+L" · "+std::to_wstring(diagnosticHistory.Collected())+L" amostras · "+Bytes(diagnosticHistory.StorageBytes());
  return out;
}
void DrawResources(app::DiagnosticSample const& sample) {
  if(resourceValues.size()!=kResourceLabels.size())return;
  auto texts=ResourceTexts(sample);for(size_t i=0;i<texts.size();++i)resourceValues[i].Text(texts[i]);
}
void ResourceControls(StackPanel body) {
  StackPanel table;table.Name(L"ResourceTable");table.Spacing(2);
  auto heading=Text(L"PARA ONDE VAI A MEMÓRIA",12,Muted);heading.Margin(Thickness{0,8,0,6});body.Children().Append(heading);
  body.Children().Append(table);resourceValues.clear();
  for(auto label:kResourceLabels){
    Border frame;frame.Background(Brush(Surface2));frame.CornerRadius(CornerRadius{8,8,8,8});frame.Padding(Thickness{12,10,12,10});
    Grid row;row.ColumnSpacing(12);ColumnDefinition name;name.Width(GridLength{1,GridUnitType::Star});row.ColumnDefinitions().Append(name);ColumnDefinition value;value.Width(GridLength{1.2,GridUnitType::Star});row.ColumnDefinitions().Append(value);
    auto title=Line(label,13,Muted,500);title.TextWrapping(TextWrapping::Wrap);row.Children().Append(title);
    auto text=Line(L"—",13,Foreground,500);text.TextWrapping(TextWrapping::Wrap);text.HorizontalAlignment(HorizontalAlignment::Right);text.TextAlignment(TextAlignment::Right);Grid::SetColumn(text,1);row.Children().Append(text);resourceValues.push_back(text);
    frame.Child(row);table.Children().Append(frame);
  }
}
DiagnosticTile Tile(Grid grid,int index,wchar_t const* label) {
  DiagnosticTile tile;Border card;card.Background(Brush(Surface2));card.CornerRadius(CornerRadius{8,8,8,8});card.Padding(Thickness{14,12,14,12});
  StackPanel inside;inside.Spacing(2);inside.Children().Append(Line(label,13,Muted,500));
  tile.value=Line(L"—",22,Foreground,600);inside.Children().Append(tile.value);
  tile.note=Line(L"",12,Muted,500);tile.note.CharacterSpacing(0);tile.note.TextTrimming(TextTrimming::CharacterEllipsis);inside.Children().Append(tile.note);
  tile.spark=Canvas();tile.spark.Height(36);tile.spark.Margin(Thickness{0,8,0,0});tile.spark.HorizontalAlignment(HorizontalAlignment::Stretch);
  tile.line=Shapes::Path();tile.line.Stroke(Brush(Accent));tile.line.StrokeThickness(2);tile.line.StrokeLineJoin(PenLineJoin::Round);tile.line.IsHitTestVisible(false);tile.spark.Children().Append(tile.line);
  inside.Children().Append(tile.spark);card.Child(inside);Grid::SetRow(card,index/2);Grid::SetColumn(card,index%2);grid.Children().Append(card);
  return tile;
}
// Ten minutes drawn as one line, scaled to its own peak; gaps stay gaps.
void Spark(DiagnosticTile const& tile,std::vector<std::pair<double,double>> const& values,uint32_t tone) {
  tile.line.Stroke(Brush(tone));double width=tile.spark.ActualWidth(),height=36;Media::PathGeometry geometry;
  double peak=0;for(auto [at,v]:values)if(v>=0)peak=std::max(peak,v);
  if(width>20 && !values.empty() && peak>0){
    // Ten minutes once there are ten; until then, everything since opening.
    double end=values.back().first,begin=std::max(values.front().first,end-600),span=std::max(2.,end-begin);Media::PathFigure part{nullptr};
    for(auto [at,v]:values){
      if(v<0||at<begin){if(part){geometry.Figures().Append(part);part=nullptr;}continue;}
      float x=float((at-begin)/span*width),y=float(height-2-v/peak*(height-4));
      if(!part){part=Media::PathFigure();part.IsFilled(false);part.StartPoint({x,y});}
      else{Media::LineSegment s;s.Point({x,y});part.Segments().Append(s);}
    }
    if(part)geometry.Figures().Append(part);
  }
  tile.line.Data(geometry);
}
void DrawRecording() {
  if(!recordingToggle)return;auto s=audio::ReadRecording();
  bool on=s.state==audio::RecordingStatus::Armed||s.state==audio::RecordingStatus::Recording;
  if(recordingToggle.IsOn()!=on){syncingRecording=true;recordingToggle.IsOn(on);syncingRecording=false;}
  std::wstring text;uint32_t tone=Muted;
  switch(s.state){
    case audio::RecordingStatus::Armed:text=L"Esperando a próxima chamada com mais alguém.";break;
    case audio::RecordingStatus::Recording:text=L"Gravando… "+std::to_wstring(s.seconds)+L" de 60 s.";tone=Accent;break;
    case audio::RecordingStatus::Saved:text=L"Gravado: "+std::to_wstring(s.seconds)+L" s. Para mandar a quem pediu, use “Preparar para enviar”.";tone=Success;break;
    case audio::RecordingStatus::Failed:text=L"Não foi possível criar os arquivos da gravação.";tone=0xf5645b;break;
    default:break;
  }
  recordingStatus.Foreground(Brush(tone));recordingStatus.Text(text);
}
void RecordingControls(StackPanel body) {
  auto heading=Text(L"GRAVAR A CHAMADA",12,Muted);heading.Margin(Thickness{0,8,0,-8});body.Children().Append(heading);
  auto s=audio::ReadRecording();
  recordingToggle=StartupToggle(s.state==audio::RecordingStatus::Armed||s.state==audio::RecordingStatus::Recording,[this](bool on){
    if(syncingRecording)return;
    if(on)audio::ArmRecording(RecordingRoot(),60);else audio::DisarmRecording();
    logf("call recording %s",on?"armed":"disarmed");DrawRecording();
  });
  recordingToggle.Name(L"RecordCall");recordingToggle.Header(box_value(L"Gravar o áudio da próxima chamada"));body.Children().Append(recordingToggle);
  auto hint=Text(L"Grava 1 minuto e desliga sozinho, na chamada em que você estiver ou na próxima: sua voz antes e depois dos filtros, o que você ouve e os números da conexão. Serve para entender eco e voz ruim. Tem a voz de todos: fica só neste PC, só as duas últimas ficam guardadas, e só sai daqui se você mandar.",13,Muted);
  hint.TextWrapping(TextWrapping::Wrap);hint.Margin(Thickness{0,-12,0,0});body.Children().Append(hint);
  recordingStatus=Line(L"",13,Muted,500);recordingStatus.Name(L"RecordCallStatus");recordingStatus.TextWrapping(TextWrapping::Wrap);CollapseEmpty(recordingStatus);body.Children().Append(recordingStatus);
  auto open=Action(L"Abrir a pasta das gravações",[this]{
    std::error_code error;auto root=RecordingRoot();std::filesystem::create_directories(root,error);
    std::wstring args=L"\""+root.wstring()+L"\"";ShellExecuteW(nullptr,L"open",L"explorer.exe",args.c_str(),nullptr,SW_SHOWNORMAL);
    logf("call recording folder opened");
  });
  auto pack=Action(L"Preparar para enviar",[this]{PackRecording();});
  pack.Name(L"PackRecording");pack.HorizontalAlignment(HorizontalAlignment::Stretch);pack.Height(44);body.Children().Append(pack);
  recordingShare=Line(L"",13,Muted,500);recordingShare.Name(L"PackRecordingStatus");recordingShare.TextWrapping(TextWrapping::Wrap);recordingShare.Visibility(Visibility::Collapsed);body.Children().Append(recordingShare);
  open.Name(L"OpenRecordings");open.HorizontalAlignment(HorizontalAlignment::Stretch);open.Height(44);body.Children().Append(open);
  DrawRecording();
}
void DrawDiagnostics() {
  DrawRecording();
  if(!measurements || diagnosticTiles.size()!=4)return;
  auto history=diagnosticHistory.Read();if(history.empty())return;
  std::vector<std::pair<double,double>> memory,cpu,stall,rtt;double memMax=0,cpuMax=0,stallMax=0;
  for(auto& entry:history){
    auto& sample=entry.process;double ms=0;for(auto& stage:sample.stages)ms=std::max(ms,stage.max_ms);
    double mb=double(sample.working_set_bytes)/(1024*1024);auto net=entry.call.rtt>=0?entry.call:entry.room;
    memory.push_back({entry.at,mb});cpu.push_back({entry.at,sample.process_cpu_percent});stall.push_back({entry.at,ms});rtt.push_back({entry.at,net.rtt});
    memMax=std::max(memMax,mb);cpuMax=std::max(cpuMax,sample.process_cpu_percent);stallMax=std::max(stallMax,ms);
  }
  auto& last=history.back();auto net=last.call.rtt>=0?last.call:last.room;
  diagnosticTiles[0].value.Text(Whole(memory.back().second,L"MB"));diagnosticTiles[0].note.Text(L"maior em 10 min: "+Whole(memMax,L"MB"));Spark(diagnosticTiles[0],memory,Accent);
  diagnosticTiles[1].value.Text(Whole(cpu.back().second,L"%"));diagnosticTiles[1].note.Text(L"de um núcleo · maior: "+Whole(cpuMax,L"%"));
  Spark(diagnosticTiles[1],cpu,cpu.back().second>=100?0xf5a524:Accent);
  // Under 50 ms nobody notices; from 100 ms it is a visible hitch.
  double now=stall.back().second;uint32_t tone=now>=100?0xf5645b:now>=50?0xf5a524:Success;
  diagnosticTiles[2].value.Text(Whole(now,L"ms"));diagnosticTiles[2].value.Foreground(Brush(tone));
  diagnosticTiles[2].note.Text(L"acima de 50 ms se sente · maior: "+Whole(stallMax,L"ms"));Spark(diagnosticTiles[2],stall,tone);
  if(net.rtt>=0){bool bad=net.rtt>=300||net.loss>=5,rough=net.rtt>=150||net.loss>=2;uint32_t netTone=bad?0xf5645b:rough?0xf5a524:Success;
    diagnosticTiles[3].value.Text(Whole(net.rtt,L"ms"));diagnosticTiles[3].value.Foreground(Brush(netTone));
    diagnosticTiles[3].note.Text(L"ida e volta · perda "+Whole(std::max(0.,net.loss),L"%"));Spark(diagnosticTiles[3],rtt,netTone);}
  else{diagnosticTiles[3].value.Text(L"—");diagnosticTiles[3].value.Foreground(Brush(Muted));diagnosticTiles[3].note.Text(L"só durante uma chamada");Spark(diagnosticTiles[3],{},Accent);}
  measurements.Text(net::Wide(TechnicalText(last)));
  DrawResources(last);
}
std::string TechnicalText(app::DiagnosticSample const& last) {
  auto sample=perf::Format(last.process);auto voice=ReadState().voice;
  sample+="\nFiltro: "+voice.noiseFilter+" reservas="+std::to_string(voice.dfnFallbacks);
  sample+="\nAEC: faltas="+std::to_string(voice.referenceMissing)+" descartes="+std::to_string(voice.referenceDropped)+" idade_ms="+std::to_string(voice.referenceAge);
  sample+="\nÁudio captura="+std::to_string(voice.captureCallbacks)+" reprodução="+std::to_string(voice.renderCallbacks)+" entrada="+voice.inputStatus+" saída="+voice.outputStatus;
  return sample;
}
// Everything the screen shows, plus ten minutes of numbers and the filtered
// log: no message text, password or e-mail (the log never holds them).
std::string DiagnosticReport() {
  auto history=diagnosticHistory.Read();std::string out="harmony nativo · diagnóstico\n"+net::Narrow(VersionText())+" · interna "+std::to_string(HARMONY_NATIVE_VERSION)+"\n";
  SYSTEMTIME now{};GetLocalTime(&now);char stamp[64];sprintf_s(stamp,"%02d/%02d/%04d %02d:%02d:%02d",now.wDay,now.wMonth,now.wYear,now.wHour,now.wMinute,now.wSecond);out+=std::string("enviado ")+stamp+"\n";
  if(!history.empty()){
    auto texts=ResourceTexts(history.back());out+="\n== para onde vai a memória ==\n";
    for(size_t i=0;i<texts.size();++i){auto value=net::Narrow(texts[i]);std::replace(value.begin(),value.end(),'\n',';');out+=net::Narrow(kResourceLabels[i])+": "+value+"\n";}
    out+="\n== agora ==\n"+TechnicalText(history.back())+"\n";
  }
  double since=CallRowsText(diagnosticHistory.ReadRows(),out);
  // The server takes 4 MB (diagnostics.go); the log gets most of it.
  out+="\n== registro ==\n"+app::DiagnosticLogSince(std::max(since,600.),3*1024*1024);
  return out;
}
void SendDiagnostic() {
  if(!service || !diagnosticSend) return;
  diagnosticSend.IsEnabled(false);diagnosticStatus.Foreground(Brush(Muted));diagnosticStatus.Text(L"Enviando…");diagnosticStatus.Visibility(Visibility::Visible);
  auto report=DiagnosticReport();auto dispatcher=window.DispatcherQueue();auto weak=get_weak();auto button=diagnosticSend;auto status=diagnosticStatus;
  logf("diagnostic send requested bytes=%zu",report.size());
  service->ChatWork([report=std::move(report),dispatcher,weak,button,status](app::Chat& chat) mutable {
    auto result=chat.Diagnostic(std::move(report));
    dispatcher.TryEnqueue([weak,button,status,code=result.status] {
      auto self=weak.get();if(!self||self->exiting)return;
      bool ok=code>=200&&code<300;
      status.Foreground(Brush(ok?Success:0xf5645b));
      status.Text(ok?L"Enviado. Avise o Claude que ele já consegue ler.":code==429?L"Você enviou há menos de um minuto. Espere um pouco.":L"Não foi possível enviar. Use Copiar registro.");
      button.IsEnabled(true);logf("diagnostic send status=%d",code);
    });
  });
}
void DiagnosticControls(StackPanel body) {
  body.Children().Append(Line(VersionText()+L" · interna "+std::to_wstring(HARMONY_NATIVE_VERSION),13,Muted,500));
  Grid tiles;tiles.ColumnSpacing(8);tiles.RowSpacing(8);
  for(int i=0;i<2;++i){ColumnDefinition c;c.Width(GridLength{1,GridUnitType::Star});tiles.ColumnDefinitions().Append(c);RowDefinition r;r.Height(GridLength{0,GridUnitType::Auto});tiles.RowDefinitions().Append(r);}
  diagnosticTiles.clear();int index=0;
  for(auto label:{L"Memória",L"Processador",L"Maior travada",L"Conexão"})diagnosticTiles.push_back(Tile(tiles,index++,label));
  tiles.SizeChanged([this](auto const&,auto const&){DrawDiagnostics();});
  body.Children().Append(tiles);
  // Send first (one click, nothing to paste), copy as the fallback.
  Grid actions;actions.ColumnSpacing(8);for(int i=0;i<2;++i){ColumnDefinition c;c.Width(GridLength{1,GridUnitType::Star});actions.ColumnDefinitions().Append(c);}
  diagnosticSend=AccountPrimary(L"Enviar diagnóstico",[this]{SendDiagnostic();});diagnosticSend.Name(L"SendDiagnostic");diagnosticSend.HorizontalAlignment(HorizontalAlignment::Stretch);diagnosticSend.Height(44);
  diagnosticSend.IsEnabled(ReadState().signedIn);actions.Children().Append(diagnosticSend);
  auto copy=Action(L"Copiar registro",[this]{logf("diagnostic copy requested");bool copied=app::CopyDiagnostic(hwnd);
    diagnosticStatus.Foreground(Brush(copied?Accent:0xf5645b));diagnosticStatus.Text(copied?L"Registro copiado.":L"Não foi possível copiar o registro.");});
  copy.Name(L"CopyLog");copy.HorizontalAlignment(HorizontalAlignment::Stretch);copy.Height(44);Grid::SetColumn(copy,1);actions.Children().Append(copy);
  body.Children().Append(actions);
  diagnosticStatus=Line(L"",13,Muted,500);diagnosticStatus.Name(L"DiagnosticStatus");diagnosticStatus.TextWrapping(TextWrapping::Wrap);CollapseEmpty(diagnosticStatus);body.Children().Append(diagnosticStatus);
  body.Children().Append(Line(ReadState().signedIn?L"Vai para o servidor do harmony: números e o registro técnico, sem mensagens, senhas nem e-mail.":L"Entre na sua conta para enviar; copiar funciona sempre.",13,Muted,500));
  RecordingControls(body);
  ResourceControls(body);
  Expander details;details.Name(L"TechnicalDetails");details.Header(box_value(L"Detalhes técnicos"));details.HorizontalAlignment(HorizontalAlignment::Stretch);details.HorizontalContentAlignment(HorizontalAlignment::Stretch);
  measurements=Text(L"—",13,Muted);measurements.Name(L"Performance");measurements.IsTextSelectionEnabled(true);details.Content(measurements);body.Children().Append(details);
  meter=DispatcherTimer();meter.Interval(std::chrono::seconds(2));meter.Tick([this](auto const&,auto const&){DrawDiagnostics();});meter.Start();DrawDiagnostics();
}
