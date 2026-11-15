// Included inside App. Ajustes → Cargos (D12e), the web's Roles.tsx: the list,
// highest first, dragged into order by its handle; and the role picked from
// it, with its name, colour, "mostrar separado", permissions and who has it.
//
// The rule is Discord's and the server enforces it (store/roles.go): with
// Gerenciar cargos one edits only the roles below one's own highest, and
// switches only the permissions one has. A role one may not edit is drawn
// read-only, with its buttons gone, so a moderator sees why it will not open.
// The list is the model's: the `roles` event replaces it whole and the panel
// redraws, so a change made elsewhere shows up here too.
StackPanel rolesPanel{nullptr};
std::string rolesPicked, rolesDraftFor;      // the role open below the list; whose name is being typed
std::wstring rolesDraft, rolesProblem;
std::string rolesGiving;
std::string profileFor; uint64_t profileRolesShown = 0; bool profileGiving = false;  // the profile's Cargos section                     // the role whose "Dar o cargo a…" list is open
uint64_t rolesShown = 0; bool rolesBusy = false, rolesTab = false;
// The row, and the two marks of where a dropped role lands: straight bars of
// their own across the gap (style.css .drop-before), never a border on the
// rounded row, which bends at the corners (the owner, 03/11/2026).
struct RoleRowSpot { Border row{nullptr}, before{nullptr}, after{nullptr}; std::string id; bool below = false; };
std::vector<RoleRowSpot> roleRows;
std::string roleDragId, roleDropOnto; bool roleDropBefore = false, roleDragMoving = false; double roleDragY = 0;

bool CanManageRoles() {
  bool can = false;
  if (service) service->ChatModel().Inspect([&](app::ChatState const& s) { can = s.Allowed("manage_roles"); });
  return can;
}
void RolesControls(StackPanel const& body) {
  rolesPanel = StackPanel(); rolesPanel.Name(L"RolesPanel"); rolesPanel.Spacing(16);
  body.Children().Append(rolesPanel); rolesShown = 0; DrawRoles();
}
// From DrawChat: the panel follows the model while it is open, and the tab
// comes and goes with Gerenciar cargos.
void RefreshRoles() {
  if (profileOverlay && !profileFor.empty()) {
    uint64_t version = 0; service->ChatModel().Inspect([&](app::ChatState const& s) { version = s.rolesVersion; });
    if (version != profileRolesShown) OpenProfile(app::ChatPerson{profileFor});
  }
  if (!settings || !settingsSignedIn) return;
  if (CanManageRoles() != rolesTab) { OpenSettings(settingsTab == "cargos" ? "conta" : settingsTab); return; }
  if (!rolesPanel || settingsTab != "cargos") return;
  uint64_t version = 0; service->ChatModel().Inspect([&](app::ChatState const& s) { version = s.rolesVersion; });
  if (version != rolesShown && !roleDragMoving) DrawRoles();
}
// One write on the chat worker; the answer redraws the panel (Roles.tsx run).
void RoleRun(std::function<std::string(app::Chat&)> work, std::function<void(std::string const&)> done = {}) {
  if (rolesBusy || !designOutput.empty()) return;
  rolesBusy = true; rolesProblem.clear();  // no redraw here: a switch would flick back until the answer
  auto weak = get_weak(); auto dispatcher = window.DispatcherQueue();
  service->ChatWork([weak, dispatcher, work, done](app::Chat& chat) {
    auto error = work(chat);
    dispatcher.TryEnqueue([weak, error, done] { if (auto self = weak.get(); self && !self->exiting) {
      self->rolesBusy = false; self->rolesProblem = net::Wide(error);
      if (done) done(error);
      if (self->rolesPanel) self->DrawRoles();
    }});
  });
}
Button RoleButton(std::string const& glyphName, std::wstring const& label, bool danger, std::function<void()> clicked) {
  Button b; HarmonyPointer(b); b.Height(32); b.MinHeight(0); b.MinWidth(0); b.Padding(Thickness{12, 0, 12, 0});
  b.CornerRadius(CornerRadius{8, 8, 8, 8}); b.BorderThickness(Thickness{0}); b.HorizontalAlignment(HorizontalAlignment::Left);
  b.Background(Brush(danger ? 0xdb3b3e : Default));
  b.Resources().Insert(box_value(L"ButtonBackgroundPointerOver"), Brush(danger ? 0xe15451 : 0x2e2e31));
  b.Resources().Insert(box_value(L"ButtonBackgroundPressed"), Brush(danger ? 0xe15451 : 0x2e2e31));
  b.Resources().Insert(box_value(L"ButtonBackgroundDisabled"), Brush(Default));
  StackPanel row; row.Orientation(Orientation::Horizontal); row.Spacing(8);
  if (!glyphName.empty()) { auto glyph = VoiceIcon(glyphName, Foreground, 16).as<FrameworkElement>(); glyph.VerticalAlignment(VerticalAlignment::Center); row.Children().Append(glyph); }
  auto words = Line(label, 13, Foreground, 500); words.VerticalAlignment(VerticalAlignment::Center); row.Children().Append(words);
  b.Content(row); Automation::AutomationProperties::SetName(b, label);
  if (danger) NegativeButton(b);
  b.IsEnabled(!rolesBusy);
  b.Click([this, clicked](auto const&, auto const&) { if (!exiting) clicked(); });
  return b;
}
// A colour's round swatch, 24 inside a 32 cell whose 2 px ring marks the one
// in use (the web's ring-offset-2: outside, so it shows on the white one too).
Button RoleSwatch(std::string const& color, bool chosen, bool enabled, std::function<void()> pick) {
  Button cell; HarmonyPointer(cell); cell.Width(32); cell.Height(32); cell.MinWidth(0); cell.MinHeight(0); cell.Padding(Thickness{0});
  cell.CornerRadius(CornerRadius{16, 16, 16, 16}); cell.BorderThickness(Thickness{0}); cell.Background(Tint(0, 0));
  for (auto key : {L"ButtonBackgroundPointerOver", L"ButtonBackgroundPressed", L"ButtonBackgroundDisabled"}) cell.Resources().Insert(box_value(key), Tint(0, 0));
  cell.HorizontalContentAlignment(HorizontalAlignment::Stretch); cell.VerticalContentAlignment(VerticalAlignment::Stretch);
  Grid face;
  if (chosen) { Shapes::Ellipse ring; ring.Width(32); ring.Height(32); ring.Stroke(Brush(Foreground)); ring.StrokeThickness(2); face.Children().Append(ring); }
  auto value = app::ChatColorValue(color);
  Shapes::Ellipse dot; dot.Width(24); dot.Height(24); dot.Fill(value ? Brush(*value) : Brush(Default));
  dot.HorizontalAlignment(HorizontalAlignment::Center); dot.VerticalAlignment(VerticalAlignment::Center); face.Children().Append(dot);
  if (!value) { auto ban = VoiceIcon("ban", Muted, 14).as<FrameworkElement>(); ban.HorizontalAlignment(HorizontalAlignment::Center); ban.VerticalAlignment(VerticalAlignment::Center); face.Children().Append(ban); }
  cell.Content(face); cell.IsEnabled(enabled);
  std::wstring label = color.empty() ? L"Sem cor" : L"Cor " + net::Wide(color);
  cell.Name(L"RoleColor-" + (color.empty() ? std::wstring(L"none") : net::Wide(color.substr(1))));
  Automation::AutomationProperties::SetName(cell, label); ToolTipService::SetToolTip(cell, box_value(color.empty() ? L"Sem cor" : net::Wide(color)));
  cell.Click([pick](auto const&, auto const&) { pick(); });
  return cell;
}
// A switch with its name and the sentence under it, as the other Ajustes.
StackPanel RoleSwitch(std::wstring const& name, std::wstring const& detail, wchar_t const* id, bool on, bool enabled, std::function<void(bool)> changed) {
  StackPanel box; box.Spacing(2);
  ToggleSwitch toggle; HarmonyPointer(toggle); toggle.Name(id); toggle.Header(box_value(name)); toggle.IsOn(on); toggle.IsEnabled(enabled && !rolesBusy);
  toggle.Toggled([this, changed, on](auto const& sender, auto const&) { auto now = sender.template as<ToggleSwitch>().IsOn(); if (now != on && !exiting) changed(now); });
  box.Children().Append(toggle);
  auto hint = Text(detail, 13, Muted); hint.TextWrapping(TextWrapping::Wrap); box.Children().Append(hint);
  return box;
}
// One row to pick from (a person, a role): 48 high, the lead at the left, the
// name, a quiet second line, the hover of the people list.
Button PickRow(FrameworkElement const& lead, std::wstring const& name, std::wstring const& sub, std::wstring const& id, std::function<void()> pick) {
  Button b; HarmonyPointer(b); b.Height(48); b.MinHeight(0); b.Padding(Thickness{8, 0, 8, 0}); b.CornerRadius(CornerRadius{8, 8, 8, 8});
  b.BorderThickness(Thickness{0}); b.Background(Tint(0, 0)); b.HorizontalAlignment(HorizontalAlignment::Stretch); b.HorizontalContentAlignment(HorizontalAlignment::Stretch);
  b.Resources().Insert(box_value(L"ButtonBackgroundPointerOver"), Brush(Default)); b.Resources().Insert(box_value(L"ButtonBackgroundPressed"), Brush(Default));
  b.Resources().Insert(box_value(L"ButtonBackgroundDisabled"), Tint(0, 0));
  Grid row; row.ColumnSpacing(10);
  for (auto w : {GridLength{32, GridUnitType::Pixel}, GridLength{1, GridUnitType::Star}}) { ColumnDefinition c; c.Width(w); row.ColumnDefinitions().Append(c); }
  lead.VerticalAlignment(VerticalAlignment::Center); lead.HorizontalAlignment(HorizontalAlignment::Center); row.Children().Append(lead);
  StackPanel words; words.VerticalAlignment(VerticalAlignment::Center);
  words.Children().Append(Line(name, 16, Foreground, 500));
  if (!sub.empty()) words.Children().Append(Line(sub, 13, Muted, 500));
  Grid::SetColumn(words, 1); row.Children().Append(words);
  b.Content(row); b.Name(id); Automation::AutomationProperties::SetName(b, name); b.IsEnabled(!rolesBusy);
  b.Click([this, pick](auto const&, auto const&) { if (!exiting) pick(); });
  return b;
}
TextBlock RoleLabel(std::wstring const& words) { auto l = Line(words, 13, Muted, 600); l.CharacterSpacing(60); return l; }

void DrawRoles() {
  if (!rolesPanel) return;
  std::vector<app::ChatRole> roles; std::vector<app::ChatPerson> members; std::vector<std::string> palette;
  std::set<std::string> mine; int64_t top = 0; bool admin = false;
  service->ChatModel().Inspect([&](app::ChatState const& s) {
    roles = s.roles; members = s.members; palette = s.palette; admin = s.admin; rolesShown = s.rolesVersion;
    mine = app::ChatPermissionsOf(s.owner, s.admin, s.roles); top = app::ChatTopOf(s.owner, s.admin, s.roles);
  });
  if (palette.empty()) palette = {  // ranks.ts ROLE_COLORS, for a server that did not send them
    "#48e5c2","#7bed9f","#74b9ff","#c39bd3","#f78fb3","#ffeaa7","#f8c291","#ff7675","#dfe6e9","#ffffff",
    "#1abc9c","#2ecc71","#3498db","#9b59b6","#e91e63","#f1c40f","#e67e22","#e74c3c","#95a5a6","#607d8b",
    "#11806a","#1f8b4c","#206694","#71368a","#ad1457","#c27c0e","#a84300","#992d22","#979c9f","#546e7a",
    "#00bcd4","#a3e635","#5865f2","#7c4dff","#e040fb","#ffd700","#ff7f50","#dc143c","#8d6e63","#b0bec5",
    "#004d40","#1b5e20","#1a3a8a","#4a148c","#880e4f","#8a6d00","#6d3410","#7f1d1d","#4e342e","#37474f"};
  bool manage = mine.contains("manage_roles");
  auto below = [&](app::ChatRole const& r) { return manage && r.position > top; };
  auto name = [&](std::string const& username) {
    for (auto const& m : members) if (m.username == username) return PersonName(m);
    return net::Wide(username);
  };
  rolesPanel.Children().Clear(); roleRows.clear();

  StackPanel intro; intro.Spacing(4);
  intro.Children().Append(Line(L"Cargos", 16, Foreground, 500));
  auto about = Text(L"Um cargo dá cor ao nome e permissões a quem o tem. O de cima vale mais: quem gerencia cargos só mexe nos que estão abaixo do seu. O dono está acima de todos.", 13, Muted);
  about.TextWrapping(TextWrapping::Wrap); intro.Children().Append(about); rolesPanel.Children().Append(intro);
  if (!rolesProblem.empty()) { auto problem = Text(rolesProblem, 13, DangerSoftFg); problem.Name(L"RolesProblem"); problem.TextWrapping(TextWrapping::Wrap); rolesPanel.Children().Append(problem); }
  if (manage) {
    auto create = RoleButton("plus", L"Criar cargo", false, [this] {
      auto made = std::make_shared<std::string>();
      RoleRun([made](app::Chat& chat) { return chat.CreateRole(made.get()); }, [this, made](std::string const& error) { if (error.empty() && !made->empty()) rolesPicked = *made; });
    });
    create.Name(L"RoleCreate"); rolesPanel.Children().Append(create);
  }

  // The list: the grip, the colour, the name in its colour, how many.
  StackPanel list; list.Name(L"RolesList"); list.Spacing(2);
  if (roles.empty()) rolesPanel.Children().Append(Text(L"Nenhum cargo ainda.", 13, Muted));
  for (auto const& r : roles) {
    bool picked = r.id == rolesPicked, movable = below(r);
    Border row; row.Name(L"RoleRow-" + net::Wide(r.name)); row.Height(48); row.CornerRadius(CornerRadius{8, 8, 8, 8});
    row.Padding(Thickness{8, 0, 8, 0});
    row.Background(Brush(picked ? Default : Surface2));
    if (!picked) {
      row.PointerEntered([](auto const& eventSender, auto const&) { auto row = eventSender.template as<Border>(); row.Background(Brush(Default)); });
      row.PointerExited([](auto const& eventSender, auto const&) { auto row = eventSender.template as<Border>(); row.Background(Brush(Surface2)); });
    }
    Grid line; line.ColumnSpacing(8);
    for (auto w : {GridLength{24, GridUnitType::Pixel}, GridLength{12, GridUnitType::Pixel}, GridLength{1, GridUnitType::Star}, GridLength{0, GridUnitType::Auto}}) { ColumnDefinition c; c.Width(w); line.ColumnDefinitions().Append(c); }
    Border grip; grip.Width(24); grip.Height(32); grip.Background(Tint(0, 0)); grip.Name(L"RoleGrip-" + net::Wide(r.name));
    auto bars = VoiceIcon("bars", Muted, 14).as<FrameworkElement>(); bars.HorizontalAlignment(HorizontalAlignment::Center); bars.VerticalAlignment(VerticalAlignment::Center);
    grip.Child(bars); grip.Opacity(movable ? 1 : 0);
    if (movable) { HarmonyPointer(grip, winrt::Microsoft::UI::Input::InputSystemCursorShape::SizeAll); ToolTipService::SetToolTip(grip, box_value(L"Arraste para mudar a ordem")); RoleGrab(grip, r.id); }
    line.Children().Append(grip);
    Shapes::Ellipse dot; dot.Width(12); dot.Height(12); dot.VerticalAlignment(VerticalAlignment::Center);
    auto color = app::ChatColorValue(r.color); dot.Fill(color ? Brush(*color) : Brush(Default)); Grid::SetColumn(dot, 1); line.Children().Append(dot);
    auto words = Line(net::Wide(r.name), 16, color.value_or(Foreground), 500); words.VerticalAlignment(VerticalAlignment::Center); Grid::SetColumn(words, 2); line.Children().Append(words);
    auto count = Line(std::to_wstring(r.members.size()) + (r.members.size() == 1 ? L" pessoa" : L" pessoas"), 13, Muted, 500);
    count.VerticalAlignment(VerticalAlignment::Center); Grid::SetColumn(count, 3); line.Children().Append(count);
    row.Child(line); HarmonyPointer(row);
    row.Tapped([this, id = r.id](auto const&, Input::TappedRoutedEventArgs const& e) { e.Handled(true); if (roleDragMoving) return; rolesPicked = rolesPicked == id ? "" : id; DrawRoles(); });
    Automation::AutomationProperties::SetName(row, net::Wide(r.name));
    Grid cell; cell.Children().Append(row);
    auto bar = [&](bool top) {
      Border b; b.Height(2); b.CornerRadius(CornerRadius{1, 1, 1, 1}); b.Background(Brush(Accent)); b.Margin(Thickness{8, 0, 8, 0});
      b.VerticalAlignment(top ? VerticalAlignment::Top : VerticalAlignment::Bottom); b.IsHitTestVisible(false); b.Visibility(Visibility::Collapsed);
      TranslateTransform half; half.Y(top ? -2 : 2); b.RenderTransform(half); cell.Children().Append(b); return b;
    };
    auto before = bar(true), after = bar(false);
    list.Children().Append(cell); roleRows.push_back({row, before, after, r.id, movable});
  }
  if (!roles.empty()) rolesPanel.Children().Append(list);

  auto at = std::find_if(roles.begin(), roles.end(), [&](auto const& r) { return r.id == rolesPicked; });
  if (at == roles.end()) { rolesPicked.clear(); return; }
  auto const role = *at; bool editable = below(role);

  // The role picked: one card under the list (Roles.tsx RoleEditor).
  Border card; card.Name(L"RoleEditor"); card.CornerRadius(CornerRadius{8, 8, 8, 8}); card.Background(Brush(Surface2)); card.Padding(Thickness{12, 12, 12, 12});
  StackPanel edit; edit.Spacing(16); card.Child(edit);
  if (!editable) {
    auto only = Text(L"Só olhando: este cargo não está abaixo do seu cargo mais alto, ou você não gerencia cargos.", 13, Muted); only.TextWrapping(TextWrapping::Wrap); edit.Children().Append(only);
  }

  StackPanel nameBox; nameBox.Spacing(8); nameBox.Children().Append(RoleLabel(L"NOME"));
  Grid nameRow; nameRow.ColumnSpacing(8);
  ColumnDefinition grow; grow.Width(GridLength{1, GridUnitType::Star}); nameRow.ColumnDefinitions().Append(grow);
  ColumnDefinition fixed; fixed.Width(GridLength{0, GridUnitType::Auto}); nameRow.ColumnDefinitions().Append(fixed);
  TextBox field = HarmonyTextBox(); field.Name(L"RoleName"); field.Background(Brush(Surface)); field.FontSize(16); field.MaxLength(32); field.Height(40);
  field.Text(rolesDraftFor == role.id ? rolesDraft : net::Wide(role.name)); field.IsEnabled(editable);
  Automation::AutomationProperties::SetName(field, L"Nome do cargo"); nameRow.Children().Append(field);
  auto saveName = [this, weakField = make_weak(field), id = role.id, old = net::Wide(role.name)] {
    auto field = weakField.get(); if (!field) return;
    std::wstring typed(field.Text()); auto a = typed.find_first_not_of(L' '), b = typed.find_last_not_of(L' ');
    typed = a == std::wstring::npos ? L"" : typed.substr(a, b - a + 1);
    if (typed.empty() || typed == old) return;
    auto patch = json::Value::Object(); patch["name"] = net::Narrow(typed);
    RoleRun([id, patch](app::Chat& chat) { return chat.UpdateRole(id, patch); }, [this](std::string const& error) { if (error.empty()) rolesDraftFor.clear(); });
  };
  if (editable) {
    auto save = RoleButton("", L"Salvar", false, saveName); save.Name(L"RoleNameSave"); save.Height(40);
    std::wstring shown(field.Text()); save.IsEnabled(!rolesBusy && shown != net::Wide(role.name) && shown.find_first_not_of(L' ') != std::wstring::npos);
    field.TextChanged([this, save, id = role.id, old = net::Wide(role.name)](auto const& eventSender, auto const&) { auto field = eventSender.template as<TextBox>();
      std::wstring typed(field.Text()); rolesDraftFor = id; rolesDraft = typed;
      save.IsEnabled(!rolesBusy && typed != old && typed.find_first_not_of(L' ') != std::wstring::npos);
    });
    field.KeyDown([saveName](auto const&, Input::KeyRoutedEventArgs const& e) { if (e.Key() == winrt::Windows::System::VirtualKey::Enter) { saveName(); e.Handled(true); } });
    Grid::SetColumn(save, 1); nameRow.Children().Append(save);
  }
  nameBox.Children().Append(nameRow); edit.Children().Append(nameBox);

  // "Sem cor" apart, then ten columns, so the five rows read light to deep.
  StackPanel colorBox; colorBox.Spacing(8); colorBox.Children().Append(RoleLabel(L"COR"));
  StackPanel colors; colors.Orientation(Orientation::Horizontal); colors.Spacing(12);
  auto pick = [this, id = role.id](std::string c) {
    auto patch = json::Value::Object(); patch["color"] = c;
    RoleRun([id, patch](app::Chat& chat) { return chat.UpdateRole(id, patch); });
  };
  auto none = RoleSwatch("", role.color.empty(), editable && !rolesBusy, [pick] { pick(""); }); none.VerticalAlignment(VerticalAlignment::Top);
  colors.Children().Append(none);
  Grid grid; grid.Name(L"RoleColors");
  for (int i = 0; i < 10; ++i) { ColumnDefinition c; c.Width(GridLength{32, GridUnitType::Pixel}); grid.ColumnDefinitions().Append(c); }
  for (size_t i = 0; i < palette.size(); ++i) {
    if (i % 10 == 0) { RowDefinition rd; rd.Height(GridLength{32, GridUnitType::Pixel}); grid.RowDefinitions().Append(rd); }
    auto swatch = RoleSwatch(palette[i], role.color == palette[i], editable && !rolesBusy, [pick, c = palette[i]] { pick(c); });
    Grid::SetColumn(swatch, int32_t(i % 10)); Grid::SetRow(swatch, int32_t(i / 10)); grid.Children().Append(swatch);
  }
  colors.Children().Append(grid); colorBox.Children().Append(colors);
  colorBox.Children().Append(Text(L"O nome aparece na cor do cargo mais alto que tiver cor.", 13, Muted));
  edit.Children().Append(colorBox);

  edit.Children().Append(RoleSwitch(L"Mostrar separado na lista de pessoas", L"Quem está online com este cargo ganha um grupo próprio.", L"RoleHoist",
    role.hoist, editable, [this, id = role.id](bool on) {
      auto patch = json::Value::Object(); patch["hoist"] = on;
      RoleRun([id, patch](app::Chat& chat) { return chat.UpdateRole(id, patch); });
    }));

  StackPanel perms; perms.Spacing(12); perms.Children().Append(RoleLabel(L"PERMISSÕES"));
  struct Perm { char const* id; wchar_t const* name; wchar_t const* detail; wchar_t const* part; };
  for (auto const& p : {Perm{"manage_channels", L"Gerenciar canais", L"Criar, renomear, organizar e apagar canais e categorias.", L"RolePerm-manage_channels"},
                        Perm{"manage_messages", L"Gerenciar mensagens", L"Apagar a mensagem dos outros e fixar mensagens nos canais.", L"RolePerm-manage_messages"},
                        Perm{"manage_roles", L"Gerenciar cargos", L"Criar, editar e dar cargos abaixo do seu cargo mais alto.", L"RolePerm-manage_roles"}}) {
    bool on = std::find(role.permissions.begin(), role.permissions.end(), p.id) != role.permissions.end();
    // Only what one has oneself can be switched, either way.
    bool can = editable && mine.contains(p.id);
    std::wstring detail = p.detail; if (editable && !mine.contains(p.id)) detail += L" Você não tem essa permissão, então não pode mudá-la.";
    perms.Children().Append(RoleSwitch(p.name, detail, p.part, on, can, [this, role, id = std::string(p.id)](bool want) {
      auto patch = json::Value::Object(); patch["permissions"] = json::Value::Array();
      for (auto x : app::ChatPermissionIds) {
        bool has = std::string(x) == id ? want : std::find(role.permissions.begin(), role.permissions.end(), x) != role.permissions.end();
        if (has) patch["permissions"].Push(std::string(x));
      }
      RoleRun([rid = role.id, patch](app::Chat& chat) { return chat.UpdateRole(rid, patch); });
    }));
  }
  perms.Children().Append(Text(L"Convites, contas e senhas continuam só do dono, com qualquer cargo.", 13, Muted));
  edit.Children().Append(perms);

  StackPanel who; who.Spacing(8); who.Name(L"RoleMembers");
  who.Children().Append(RoleLabel(L"QUEM TEM · " + std::to_wstring(role.members.size())));
  for (auto const& username : role.members) {
    Grid row; row.Height(40); row.ColumnSpacing(8); row.Name(L"RoleMember-" + net::Wide(username));
    for (auto w : {GridLength{32, GridUnitType::Pixel}, GridLength{1, GridUnitType::Star}, GridLength{0, GridUnitType::Auto}}) { ColumnDefinition c; c.Width(w); row.ColumnDefinitions().Append(c); }
    app::ChatPerson person{username}; for (auto const& m : members) if (m.username == username) person = m;
    auto face = PhotoFace(person, 32); face.VerticalAlignment(VerticalAlignment::Center); row.Children().Append(face);
    auto label = Line(name(username), 16, Foreground, 500); label.VerticalAlignment(VerticalAlignment::Center); Grid::SetColumn(label, 1); row.Children().Append(label);
    if (editable) {
      // The X here is quiet, as the web's: grey at rest, not the red of a panel's close.
      Button take; HarmonyPointer(take); take.Width(32); take.Height(32); take.MinWidth(0); take.MinHeight(0); take.Padding(Thickness{0});
      take.CornerRadius(CornerRadius{8, 8, 8, 8}); take.BorderThickness(Thickness{0}); take.Background(Tint(0, 0)); take.Content(VoiceIcon("xmark", Muted, 14));
      take.Resources().Insert(box_value(L"ButtonBackgroundPointerOver"), Brush(Default)); take.Resources().Insert(box_value(L"ButtonBackgroundPressed"), Brush(Default));
      take.Resources().Insert(box_value(L"ButtonBackgroundDisabled"), Tint(0, 0));
      take.PointerEntered([](auto const& eventSender, auto const&) { auto take = eventSender.template as<Button>(); take.Content(VoiceIcon("xmark", Foreground, 14)); });
      take.PointerExited([](auto const& eventSender, auto const&) { auto take = eventSender.template as<Button>(); take.Content(VoiceIcon("xmark", Muted, 14)); });
      auto tip = L"Tirar o cargo de " + name(username); Automation::AutomationProperties::SetName(take, tip); ToolTipService::SetToolTip(take, box_value(tip));
      take.Name(L"RoleMemberRemove-" + net::Wide(username));
      take.Click([this, id = role.id, username](auto const&, auto const&) { if (!exiting) RoleRun([id, username](app::Chat& chat) { return chat.RoleMember(id, username, false); }); });
      take.IsEnabled(!rolesBusy); take.VerticalAlignment(VerticalAlignment::Center); Grid::SetColumn(take, 2); row.Children().Append(take);
    }
    who.Children().Append(row);
  }
  // Only the owner puts a role on the owner (store.SetRoleMember).
  std::vector<app::ChatPerson> candidates;
  for (auto const& m : members) if (std::find(role.members.begin(), role.members.end(), m.username) == role.members.end() && (!m.admin || admin)) candidates.push_back(m);
  // Nor a role carrying a permission one lacks, even to oneself (the D12c audit's hole).
  bool carries = std::all_of(role.permissions.begin(), role.permissions.end(), [&](auto const& x) { return mine.contains(x); });
  if (editable && carries && !candidates.empty()) {
    // The people as the people list draws them, 48 high with the photo, not a
    // menu of small text lines (the owner, 03/11/2026: "meio pequeno, meio junto").
    bool open = rolesGiving == role.id;
    auto give = RoleButton(open ? "chevron-down" : "person-plus", L"Dar o cargo a…", false, [this, id = role.id] { rolesGiving = rolesGiving == id ? "" : id; DrawRoles(); });
    give.Name(L"RoleMemberAdd"); who.Children().Append(give);
    if (open) {
      StackPanel people; people.Name(L"RoleGiveList"); people.Spacing(2);
      for (auto const& m : candidates)
        people.Children().Append(PickRow(PhotoFace(m, 32), PersonName(m), L"@" + net::Wide(m.username), L"RoleGive-" + net::Wide(m.username),
          [this, id = role.id, username = m.username] { RoleRun([id, username](app::Chat& chat) { return chat.RoleMember(id, username, true); }); }));
      who.Children().Append(people);
    }
  }
  edit.Children().Append(who);

  if (editable) {
    auto remove = RoleButton("trash-bin", L"Apagar cargo", true, [this, role] { AskRemoveRole(role); });
    remove.Name(L"RoleDelete"); edit.Children().Append(remove);
  }
  rolesPanel.Children().Append(card);
}
void AskRemoveRole(app::ChatRole const& role) {
  ContentDialog dialog; dialog.XamlRoot(root.XamlRoot()); dialog.RequestedTheme(ElementTheme::Dark); dialog.Name(L"RemoveRoleDialog");
  ConfirmLook(dialog);
  dialog.Title(Line(L"Apagar este cargo?", 20, Foreground, 600));
  StackPanel body; body.Spacing(12);
  auto said = Text(L"Quem tem o cargo perde a cor e as permissões dele na hora. Isso não tem volta.", 13, Muted); said.TextWrapping(TextWrapping::Wrap); body.Children().Append(said);
  body.Children().Append(Line(net::Wide(role.name), 16, app::ChatColorValue(role.color).value_or(Foreground), 500));
  dialog.Content(body);
  dialog.PrimaryButtonText(L"Apagar"); dialog.CloseButtonText(L"Cancelar"); dialog.DefaultButton(ContentDialogButton::Close);
  auto op = dialog.ShowAsync();
  // Weak before the wait: the dialog can complete after the program is leaving.
  op.Completed([weak = get_weak(), id = role.id](auto const& result, auto) {
    if (result.GetResults() != ContentDialogResult::Primary) return;
    auto owner = weak.get(); if (!owner || owner->exiting) return;
    owner->window.DispatcherQueue().TryEnqueue([weak, id] { if (auto self = weak.get(); self && !self->exiting)
      self->RoleRun([id](app::Chat& chat) { return chat.RemoveRole(id); }, [self](std::string const& error) { if (error.empty()) self->rolesPicked.clear(); });
    });
  });
}
// The drag, as the column's (DragMoved): a press on the grip that travels
// 4 px picks the role up; over another role below one's own it goes before
// or after it, and the whole order goes to the server once, on release.
// The drag in three steps, y in rolesPanel's coordinates: the grip's pointer
// calls them, and the audit calls them straight, with no real mouse.
void RoleDragBegin(std::string const& id, double y) { roleDragId = id; roleDragMoving = false; roleDropOnto.clear(); roleDragY = y; }
void RoleDragTo(double y) {
  if (roleDragId.empty()) return;
  if (!roleDragMoving && std::abs(y - roleDragY) < 4) return;
  roleDragMoving = true; roleDropOnto.clear();
  for (auto& spot : roleRows) {
    spot.row.Opacity(spot.id == roleDragId ? .5 : 1); spot.before.Visibility(Visibility::Collapsed); spot.after.Visibility(Visibility::Collapsed);
    auto r = spot.row.TransformToVisual(rolesPanel).TransformBounds({0, 0, float(spot.row.ActualWidth()), float(spot.row.ActualHeight())});
    // Only among the roles below one's own: the server refuses the rest.
    if (y < r.Y || y >= r.Y + r.Height || !spot.below || spot.id == roleDragId) continue;
    roleDropOnto = spot.id; roleDropBefore = y < r.Y + r.Height / 2;
    (roleDropBefore ? spot.before : spot.after).Visibility(Visibility::Visible);
  }
}
void RoleDragEnd(bool commit) {
  auto id = std::exchange(roleDragId, std::string{}); bool moved = roleDragMoving; auto onto = roleDropOnto; bool before = roleDropBefore;
  roleDropOnto.clear();
  for (auto& spot : roleRows) { spot.row.Opacity(1); spot.before.Visibility(Visibility::Collapsed); spot.after.Visibility(Visibility::Collapsed); }
  if (moved) {
    // The tap that ends a drag is not a tap on the row it ends over.
    auto weak = get_weak();
    window.DispatcherQueue().TryEnqueue(winrt::Microsoft::UI::Dispatching::DispatcherQueuePriority::Low, [weak] { if (auto self = weak.get()) self->roleDragMoving = false; });
  }
  logf("roles: drag %s, moved %d, onto a role %d", commit ? "released" : "lost", moved, !onto.empty());
  if (!commit || !moved || onto.empty() || id.empty()) return;
  std::vector<app::ChatRole> roles; service->ChatModel().Inspect([&](app::ChatState const& s) { roles = s.roles; });
  if (auto order = app::ChatMoveRole(roles, id, onto, before)) RoleRun([order = *order](app::Chat& chat) { return chat.OrderRoles(order); });
}
void RoleGrab(Border const& grip, std::string const& id) {
  grip.PointerPressed([this, id](auto const& eventSender, Input::PointerRoutedEventArgs const& e) { auto grip = eventSender.template as<Border>();
    if (!e.GetCurrentPoint(grip).Properties().IsLeftButtonPressed() || rolesBusy) return;
    RoleDragBegin(id, e.GetCurrentPoint(rolesPanel).Position().Y);
    bool held = grip.CapturePointer(e.Pointer()); e.Handled(true);
    logf("roles: grip pressed, captured %d", held);
  });
  grip.PointerMoved([this](auto const&, Input::PointerRoutedEventArgs const& e) {
    if (roleDragId.empty()) return;
    RoleDragTo(e.GetCurrentPoint(rolesPanel).Position().Y); e.Handled(true);
  });
  // End first: releasing the capture raises PointerCaptureLost at once,
  // which would otherwise cancel the drop it is part of.
  grip.PointerReleased([this](auto const& eventSender, Input::PointerRoutedEventArgs const& e) { auto grip = eventSender.template as<Border>(); if (roleDragId.empty()) return; e.Handled(true); RoleDragEnd(true); grip.ReleasePointerCapture(e.Pointer()); });
  grip.PointerCaptureLost([this](auto const&, auto const&) { if (!roleDragId.empty()) RoleDragEnd(false); });
}

#ifndef HARMONY_PUBLISH
// --chat-audit with HARMONY_AUDIT_ROLES (testes/roles-audit.mjs): the D12c
// criterion in this program. "dono" signs in as the owner and does it all
// through the Cargos tab; "amigo" signs in as bruna, who has Moderador, and
// writes amigo-pronto when the owner may take it away. No sound anywhere.
winrt::Windows::Foundation::IAsyncAction RolesAudit(std::string& stage) {
  auto lifetime = get_strong(); winrt::apartment_context ui;
  auto pause = [](int ms) { return winrt::resume_after(std::chrono::milliseconds(ms)); };
  auto press = [this](FrameworkElement const& e) { ParityRequire(bool(e)); Automation::Peers::ButtonAutomationPeer(e.as<Button>()).GetPattern(Automation::Peers::PatternInterface::Invoke).as<Automation::Provider::IInvokeProvider>().Invoke(); };
  auto part = [this](wchar_t const* name) { return FindDesignPart(root, name).try_as<FrameworkElement>(); };
  auto model = [this] { app::ChatState out; service->ChatModel().Inspect([&](app::ChatState const& s) { out.roles = s.roles; out.owner = s.owner; out.admin = s.admin; out.conversations = s.conversations; out.timelines = s.timelines; }); return out; };
  auto role = [&](std::string const& name) { for (auto const& r : model().roles) if (r.name == name) return std::optional<app::ChatRole>(r); return std::optional<app::ChatRole>(); };
  auto green = [this](std::string const& who) {
    auto name = FindDesignPart(root, (L"PersonName-" + net::Wide(who)).c_str()).try_as<TextBlock>();
    auto brush = name ? name.Foreground().try_as<SolidColorBrush>() : nullptr;
    return brush && brush.Color().R == 0x2e && brush.Color().G == 0xcc && brush.Color().B == 0x71;
  };
  bool owner = app::Environment(L"HARMONY_AUDIT_ROLES") == "dono";
  stage = "roles-login";
  service->Restore();
  for (int i = 0; i < 100 && ReadState().busy; ++i) { co_await pause(100); co_await ui; }
  user.Text(net::Wide(app::Environment(L"HARMONY_TEST_USER"))); password.Password(net::Wide(app::Environment(L"HARMONY_TEST_PASSWORD"))); Submit();
  for (int i = 0; i < 300 && !ReadState().signedIn; ++i) { co_await pause(100); co_await ui; }
  ParityRequire(ReadState().signedIn);
  for (int i = 0; i < 50 && !part(L"EmailSkip") && ReadState().needsEmail; ++i) { co_await pause(100); co_await ui; }
  if (ReadState().needsEmail) { press(part(L"EmailSkip")); for (int i = 0; i < 100 && ReadState().needsEmail; ++i) { co_await pause(100); co_await ui; } }
  for (int i = 0; i < 300 && (chatActive.empty() || messageItems.Size() == 0); ++i) { co_await pause(100); co_await ui; }
  ParityRequire(messageItems.Size() > 0);
  co_await pause(800); co_await ui;
  // Waits up to eight seconds for what the model or the screen should reach.
#define ROLES_WAIT(cond) do { for (int i_ = 0; i_ < 80 && !(cond); ++i_) { co_await pause(100); co_await ui; } ParityRequire(cond); } while (0)
  if (owner) {
    stage = "roles-tab";
    ParityRequire(model().admin);
    {
      // Tests run on the second monitor (the owner's rule), as ChatAuditForeground
      // does; that one is not used here, because it needs the window in front.
      MONITORINFO monitor{sizeof(MONITORINFO)}; ParityRequire(GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor));
      EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT, LPARAM data) -> BOOL {
        MONITORINFO info{sizeof(info)}; if (!GetMonitorInfoW(m, &info) || (info.dwFlags & MONITORINFOF_PRIMARY)) return TRUE;
        *reinterpret_cast<MONITORINFO*>(data) = info; return FALSE;
      }, reinterpret_cast<LPARAM>(&monitor));
      window.AppWindow().Move({monitor.rcWork.left + 32, monitor.rcWork.top + 32});
      co_await pause(300); co_await ui;
    }
    OpenSettings("cargos"); co_await pause(400); co_await ui;
    ParityRequire(settingsTab == "cargos" && bool(rolesPanel) && bool(part(L"SettingsTab-cargos")));
    co_await SaveDesignSurface(part(L"SettingsModal"), chatOutput, "d12e-cargos-vazio");
    stage = "roles-create";
    press(part(L"RoleCreate")); ROLES_WAIT(role("Novo cargo").has_value() && bool(part(L"RoleEditor")) && !rolesBusy);
    part(L"RoleName").as<TextBox>().Text(L"Moderador"); co_await pause(100); co_await ui;
    press(part(L"RoleNameSave")); ROLES_WAIT(role("Moderador").has_value() && !rolesBusy);
    stage = "roles-color";
    press(part(L"RoleColor-2ecc71")); ROLES_WAIT(role("Moderador")->color == "#2ecc71" && !rolesBusy);
    stage = "roles-permission";
    part(L"RolePerm-manage_messages").as<ToggleSwitch>().IsOn(true);
    ROLES_WAIT(role("Moderador")->permissions == std::vector<std::string>{"manage_messages"} && !rolesBusy);
    part(L"RoleHoist").as<ToggleSwitch>().IsOn(true); ROLES_WAIT(role("Moderador")->hoist && !rolesBusy);
    stage = "roles-give";
    {
      press(part(L"RoleMemberAdd")); ROLES_WAIT(bool(part(L"RoleGive-bruna")));
      co_await pause(300); co_await ui; part(L"RoleGive-bruna").StartBringIntoView(); co_await pause(500); co_await ui;
      co_await SaveDesignSurface(part(L"SettingsModal"), chatOutput, "d12e-dar-cargo-lista");
      press(part(L"RoleGive-bruna"));
    }
    ROLES_WAIT(role("Moderador")->members == std::vector<std::string>{"bruna"} && !rolesBusy && bool(part(L"RoleMember-bruna")));
    co_await pause(300); co_await ui;
    co_await SaveDesignSurface(part(L"SettingsModal"), chatOutput, "d12e-cargo-moderador");
    stage = "roles-order";
    press(part(L"RoleCreate")); ROLES_WAIT(role("Novo cargo").has_value() && !rolesBusy && bool(part(L"RoleName")));
    part(L"RoleName").as<TextBox>().Text(L"Ajudante"); co_await pause(100); co_await ui;
    press(part(L"RoleNameSave")); ROLES_WAIT(role("Ajudante").has_value() && !rolesBusy);
    {
      // The drag through its own three steps, Ajudante's grip onto the top half
      // of Moderador: no real mouse, so the owner's screen is never touched.
      auto from = part(L"RoleGrip-Ajudante"), into = part(L"RoleRow-Moderador"); ParityRequire(from && into);
      auto y = [&](FrameworkElement const& e, float fy) { return double(e.TransformToVisual(rolesPanel).TransformPoint({0, float(e.ActualHeight() * fy)}).Y); };
      double a = y(from, .5f), b = y(into, .25f);
      RoleDragBegin(role("Ajudante")->id, a);
      for (int k = 1; k <= 12; ++k) { RoleDragTo(a + (b - a) * k / 12); co_await pause(30); co_await ui; }
      ParityRequire(roleDropOnto == role("Moderador")->id && roleDropBefore);
      co_await pause(150); co_await ui;
      co_await SaveDesignSurface(part(L"SettingsModal"), chatOutput, "d12e-cargos-arrastando");
      RoleDragEnd(true);
    }
    ROLES_WAIT(model().roles.size() == 2 && model().roles[0].name == "Ajudante" && !rolesBusy);
    stage = "roles-delete";
    press(part(L"RoleCreate")); ROLES_WAIT(role("Novo cargo").has_value() && !rolesBusy && bool(part(L"RoleName")));
    part(L"RoleName").as<TextBox>().Text(L"Temporário"); co_await pause(100); co_await ui;
    press(part(L"RoleNameSave")); ROLES_WAIT(role("Temporário").has_value() && !rolesBusy);
    press(part(L"RoleDelete")); co_await pause(600); co_await ui;
    bool asked = false;
    for (auto const& popup : Media::VisualTreeHelper::GetOpenPopupsForXamlRoot(root.XamlRoot()))
      if (auto dialog = popup.Child().try_as<ContentDialog>()) {
        asked = true; co_await SaveDesignSurface(dialog, chatOutput, "d12e-cargo-apagar");
        press(FindDesignPart(dialog, L"PrimaryButton").as<FrameworkElement>());
      }
    ParityRequire(asked);
    ROLES_WAIT(!role("Temporário") && !rolesBusy);
    rolesPicked = role("Moderador")->id; DrawRoles(); co_await pause(300); co_await ui;
    co_await SaveDesignSurface(part(L"SettingsModal"), chatOutput, "d12e-cargos-lista");
    stage = "roles-people";
    CloseSettings(); co_await pause(500); co_await ui;
    ParityRequire(green("bruna") && !green("carlos"));
    co_await SaveDesignSurface(part(L"PeopleIsland"), chatOutput, "d12e-pessoas-dono");
    OpenProfile(app::ChatPerson{"bruna"}); co_await pause(500); co_await ui;
    ParityRequire(bool(part(L"ProfileRole-Moderador")));
    co_await SaveDesignSurface(part(L"PersonProfile"), chatOutput, "d12e-perfil-bruna");
    stage = "roles-profile-give";
    CloseProfile(); OpenProfile(app::ChatPerson{"carlos"}); co_await pause(500); co_await ui;
    press(part(L"ProfileRoleGive")); ROLES_WAIT(bool(part(L"ProfileGive-Ajudante")));
    co_await pause(200); co_await ui;
    co_await SaveDesignSurface(part(L"PersonProfile"), chatOutput, "d12e-perfil-dar-cargo");
    press(part(L"ProfileGive-Ajudante"));
    ROLES_WAIT(role("Ajudante")->members == std::vector<std::string>{"carlos"} && !rolesBusy && bool(part(L"ProfileRoleTake-Ajudante")));
    ParityRequire(!part(L"ProfileGiveList"));
    co_await pause(300); co_await ui;
    co_await SaveDesignSurface(part(L"PersonProfile"), chatOutput, "d12e-perfil-com-cargo");
    press(part(L"ProfileRoleTake-Ajudante"));
    ROLES_WAIT(role("Ajudante")->members.empty() && !rolesBusy && !part(L"ProfileRole-Ajudante"));
    CloseProfile(); OpenProfile(app::ChatPerson{model().owner}); co_await pause(500); co_await ui;
    ParityRequire(bool(part(L"ProfileRole-Dono")));
    co_await SaveDesignSurface(part(L"PersonProfile"), chatOutput, "d12e-perfil-dono");
    CloseProfile();
  } else {
    stage = "roles-friend";
    auto me = model();
    ParityRequire(!me.admin && me.roles.size() == 2);
    ROLES_WAIT(green("bruna"));
    // Her group: MODERADOR, above Online, because she is online.
    bool grouped = false;
    for (auto const& child : peopleList.Children()) if (auto box = child.try_as<Border>()) if (auto words = box.Child().try_as<TextBlock>())
      if (std::wstring(words.Text()).rfind(L"MODERADOR", 0) == 0) grouped = true;
    ParityRequire(grouped);
    co_await SaveDesignSurface(part(L"PeopleIsland"), chatOutput, "d12e-pessoas-amigo");
    std::vector<int64_t> carlos;
    for (auto const& m : me.timelines[chatActive].messages) if (m.author.username == "carlos" && !m.deleted) carlos.push_back(m.id);
    ParityRequire(carlos.size() == 4);
    auto row = ReadRow(carlos[0]); ParityRequire(row && row->canDelete && row->canPin && row->nameColor == std::optional<uint32_t>());
    auto mine = ReadRow(me.timelines[chatActive].messages.back().id); ParityRequire(mine && mine->m.author.username == "bruna" && mine->nameColor == 0x2ecc71u);
    co_await SaveDesignSurface(root, chatOutput, "d12e-conversa-amigo");
    OpenProfile(app::ChatPerson{"bruna"}); co_await pause(500); co_await ui;
    ParityRequire(bool(part(L"ProfileRole-Moderador")) && !part(L"ProfileRoleTake-Moderador") && !part(L"ProfileRoleGive"));
    CloseProfile(); co_await pause(200); co_await ui;
    stage = "roles-friend-delete";
    service->ChatWork([c = chatActive, id = carlos[0]](app::Chat& chat) { chat.Remove(c, id); });
    ROLES_WAIT([&] { auto now = model(); for (auto const& m : now.timelines[chatActive].messages) if (m.id == carlos[0]) return m.deleted; return false; }());
    stage = "roles-friend-settings";
    OpenSettings("cargos"); co_await pause(400); co_await ui;
    ParityRequire(settingsTab == "conta" && !part(L"SettingsTab-cargos"));
    co_await SaveDesignSurface(part(L"SettingsModal"), chatOutput, "d12e-ajustes-amigo");
    CloseSettings(); co_await pause(300); co_await ui;
    stage = "roles-taken";
    std::ofstream(chatOutput / L"amigo-pronto") << "1";
    ROLES_WAIT(!model().Allowed("manage_messages") && !green("bruna"));
    co_await pause(500); co_await ui;
    row = ReadRow(carlos[1]); ParityRequire(row && !row->canDelete && !row->canPin);
    mine = ReadRow(mine->m.id); ParityRequire(mine && !mine->nameColor);
    co_await SaveDesignSurface(part(L"PeopleIsland"), chatOutput, "d12e-pessoas-sem-cargo");
    co_await SaveDesignSurface(root, chatOutput, "d12e-conversa-sem-cargo");
  }
#undef ROLES_WAIT
  stage = "roles-done";
}
#endif

// The profile's Cargos section (Profile.tsx, and since 03/11/2026 a place to
// give and take them: the owner asked for "dar cargo direto no perfil"). The
// chips, highest first, "Dono" first on the owner; whoever manages roles sees
// an x on each role they could take and "+ Dar cargo" with the ones the server
// would let them give: below their own, not held yet, carrying nothing they
// lack, and on the owner only by the owner.
void ProfileRolesSection(StackPanel const& content, app::ChatPerson const& p) {
  std::vector<app::ChatRole> all, held; std::set<std::string> mine; int64_t top = 0; bool admin = false;
  service->ChatModel().Inspect([&](app::ChatState const& s) {
    all = s.roles; held = app::ChatRolesOf(p.username, s.roles); admin = s.admin; profileRolesShown = s.rolesVersion;
    mine = app::ChatPermissionsOf(s.owner, s.admin, s.roles); top = app::ChatTopOf(s.owner, s.admin, s.roles);
  });
  if (profileFor != p.username) profileGiving = false;
  profileFor = p.username;
  bool manage = mine.contains("manage_roles") && (!p.admin || admin);
  auto editable = [&](app::ChatRole const& r) { return manage && r.position > top; };
  std::vector<app::ChatRole> givable;
  for (auto const& r : all)
    if (editable(r) && std::find(r.members.begin(), r.members.end(), p.username) == r.members.end() &&
        std::all_of(r.permissions.begin(), r.permissions.end(), [&](auto const& x) { return mine.contains(x); })) givable.push_back(r);
  if (held.empty() && !p.admin && givable.empty()) return;
  auto run = [this, who = p.username](std::string id, bool on) {
    if (rolesBusy) return;
    rolesBusy = true;
    auto weak = get_weak(); auto dispatcher = window.DispatcherQueue();
    service->ChatWork([weak, dispatcher, id, who, on](app::Chat& chat) {
      auto error = chat.RoleMember(id, who, on);
      // By person, not by panel: the roles event usually redraws the profile
      // before this answer arrives, and the list must still close.
      dispatcher.TryEnqueue([weak, error, who] { if (auto self = weak.get(); self && !self->exiting) {
        self->rolesBusy = false; if (!self->profileOverlay || self->profileFor != who) return;
        self->profileGiving = false; self->OpenProfile(app::ChatPerson{who});
        if (!error.empty()) if (auto status = FindDesignPart(self->profileOverlay, L"ProfileStatus").try_as<TextBlock>()) status.Text(net::Wide(error));
      }});
    });
  };
  StackPanel section; section.Name(L"ProfileRoles"); section.Spacing(8);
  section.Children().Append(RoleLabel(L"CARGOS"));
  // The chips wrap as words do: inline in a paragraph, 6 apart each way.
  RichTextBlock chips; chips.IsTextSelectionEnabled(false); chips.TextWrapping(TextWrapping::Wrap); Documents::Paragraph line; chips.Blocks().Append(line);
  auto place = [&](FrameworkElement const& box) { box.Margin(Thickness{0, 0, 6, 6}); Documents::InlineUIContainer at; at.Child(box); line.Inlines().Append(at); };
  auto chip = [&](std::wstring const& name, std::optional<std::string> color, std::string take) {
    StackPanel row; row.Orientation(Orientation::Horizontal); row.Spacing(6); row.VerticalAlignment(VerticalAlignment::Center);
    if (color) { Shapes::Ellipse dot; dot.Width(10); dot.Height(10); dot.VerticalAlignment(VerticalAlignment::Center);
      auto value = app::ChatColorValue(*color); dot.Fill(value ? Brush(*value) : Brush(Muted)); row.Children().Append(dot); }
    auto words = Line(name, 13, Foreground, 500); words.VerticalAlignment(VerticalAlignment::Center); row.Children().Append(words);
    Border box; box.Height(28); box.Padding(Thickness{10, 0, take.empty() ? 10.0 : 4.0, 0}); box.CornerRadius(CornerRadius{8, 8, 8, 8}); box.Background(Brush(Surface2));
    if (!take.empty()) {
      Button x; HarmonyPointer(x); x.Width(20); x.Height(20); x.MinWidth(0); x.MinHeight(0); x.Padding(Thickness{0}); x.CornerRadius(CornerRadius{6, 6, 6, 6});
      x.BorderThickness(Thickness{0}); x.Background(Tint(0, 0)); x.Content(VoiceIcon("xmark", Muted, 12)); x.VerticalAlignment(VerticalAlignment::Center);
      x.Resources().Insert(box_value(L"ButtonBackgroundPointerOver"), Brush(Surface3)); x.Resources().Insert(box_value(L"ButtonBackgroundPressed"), Brush(Surface3));
      x.PointerEntered([](auto const& eventSender, auto const&) { auto x = eventSender.template as<Button>(); x.Content(VoiceIcon("xmark", Foreground, 12)); });
      x.PointerExited([](auto const& eventSender, auto const&) { auto x = eventSender.template as<Button>(); x.Content(VoiceIcon("xmark", Muted, 12)); });
      auto tip = L"Tirar o cargo " + name; Automation::AutomationProperties::SetName(x, tip); ToolTipService::SetToolTip(x, box_value(tip));
      x.Name(L"ProfileRoleTake-" + name); x.Click([run, take](auto const&, auto const&) { run(take, false); }); row.Children().Append(x);
    }
    box.Child(row); box.Name(L"ProfileRole-" + name); place(box);
  };
  if (p.admin) chip(L"Dono", std::nullopt, "");
  for (auto const& r : held) chip(net::Wide(r.name), r.color, editable(r) ? r.id : "");
  if (!givable.empty()) {
    Button add; HarmonyPointer(add); add.Height(28); add.MinHeight(0); add.MinWidth(0); add.Padding(Thickness{10, 0, 10, 0});
    add.CornerRadius(CornerRadius{8, 8, 8, 8}); add.BorderThickness(Thickness{0}); add.Background(Brush(profileGiving ? Default : Surface2));
    add.Resources().Insert(box_value(L"ButtonBackgroundPointerOver"), Brush(Default)); add.Resources().Insert(box_value(L"ButtonBackgroundPressed"), Brush(Default));
    StackPanel row; row.Orientation(Orientation::Horizontal); row.Spacing(6);
    auto plus = VoiceIcon("plus", Foreground, 14).as<FrameworkElement>(); plus.VerticalAlignment(VerticalAlignment::Center); row.Children().Append(plus);
    auto words = Line(L"Dar cargo", 13, Foreground, 500); words.VerticalAlignment(VerticalAlignment::Center); row.Children().Append(words);
    add.Content(row); add.Name(L"ProfileRoleGive"); Automation::AutomationProperties::SetName(add, L"Dar cargo");
    add.Click([this, who = p.username](auto const&, auto const&) { profileGiving = !profileGiving; OpenProfile(app::ChatPerson{who}); });
    place(add);
  }
  section.Children().Append(chips);
  if (profileGiving && !givable.empty()) {
    StackPanel list; list.Name(L"ProfileGiveList"); list.Spacing(2); list.Margin(Thickness{0, -6, 0, 0});
    for (auto const& r : givable) {
      Shapes::Ellipse dot; dot.Width(12); dot.Height(12); auto value = app::ChatColorValue(r.color); dot.Fill(value ? Brush(*value) : Brush(Default));
      auto count = r.members.size();
      list.Children().Append(PickRow(dot, net::Wide(r.name), std::to_wstring(count) + (count == 1 ? L" pessoa" : L" pessoas"), L"ProfileGive-" + net::Wide(r.name),
        [run, id = r.id] { run(id, true); }));
    }
    section.Children().Append(list);
  }
  content.Children().Append(section);
}
