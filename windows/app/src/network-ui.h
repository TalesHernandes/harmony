std::vector<HistoryChart> networkCharts;
TextBlock networkRoute{nullptr};
void DrawNetwork() {
  if(!networkRoute || networkCharts.size()!=6)return;
  auto state=ReadState();bool direct=service->CallRead().phase!=app::CallPhase::Idle;
  bool active=direct||!state.voice.room.empty();
  auto history=diagnosticHistory.Read();auto sample=network::Latest(direct).Read();auto now=GetTickCount64();
  bool fresh=active && sample.at && now>=sample.at && now-sample.at<2500;
  networkRoute.Text(!active?L"Entre numa sala ou numa ligação para medir a conexão.":!fresh?L"Aguardando estatísticas da conexão…":
    sample.route==network::Sample::Turn?L"Rota: TURN (relay)":sample.route==network::Sample::Direct?(direct?L"Rota: direto entre as pessoas":L"Rota: direto ao servidor da sala"):L"Rota: ainda sem medida");
  std::array<double network::Sample::*,6> fields{&network::Sample::rtt,&network::Sample::loss,&network::Sample::jitter,&network::Sample::available,&network::Sample::send,&network::Sample::receive};
  std::array<wchar_t const*,6> units{L"ms",L"%",L"ms",L"kbit/s",L"kbit/s",L"kbit/s"};
  for(size_t i=0;i<fields.size();++i) {
    std::vector<std::pair<double,double>> values;double maximum=1;
    for(auto& point:history) {auto s=direct?point.call:point.room;double value=s.*fields[i];
      bool valid=s.generation==sample.generation && s.at && point.at*1000>=s.at && point.at*1000-s.at<2500;
      values.push_back({point.at,valid?value:-1});if(valid)maximum=std::max(maximum,value);
    }
    double current=sample.*fields[i];Plot(networkCharts[i],values,maximum,fresh&&current>=0?Metric(current,units[i]):L"—",L"Desde a conexão");
  }
}
void NetworkControls(StackPanel body) {
  body.Children().Append(Text(L"Internet",20));
  body.Children().Append(Text(L"Últimos 10 minutos · coleta contínua a cada 2 s durante a conexão. RTT da mídia; perda recebida no intervalo; maior jitter recebido. Banda disponível é estimada.",13,Muted));
  networkRoute=Text(L"",13,Muted);body.Children().Append(networkRoute);networkCharts.clear();
  std::array<wchar_t const*,6> units{L"ms",L"%",L"ms",L"kbit/s",L"kbit/s",L"kbit/s"};size_t i=0;
  for(auto title:{L"Ping / RTT · ms",L"Perda recebida · %",L"Jitter · ms",L"Banda disponível para envio · kbit/s",L"Bitrate de envio · kbit/s",L"Bitrate de recepção · kbit/s"})networkCharts.push_back(Chart(body,title,units[i++]));
  DrawNetwork();meter=DispatcherTimer();meter.Interval(std::chrono::seconds(2));meter.Tick([this](auto const&,auto const&){DrawNetwork();});meter.Start();
}
