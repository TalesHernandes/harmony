# Componentes de terceiros

O código do harmony é licenciado sob a [AGPL-3.0](LICENSE). Os componentes abaixo
vêm de outros autores e **mantêm as próprias licenças**; os textos completos estão
em [`licencas/`](licencas/) ou no lugar indicado.

**Sons:** os efeitos e o toque estão aqui só para o aplicativo funcionar. Eles não
fazem parte do que este repositório licencia: não os reutilize fora do harmony nem
os redistribua separados. Se for publicar um fork, troque-os por sons seus.

| Componente | Onde aparece | Licença | Texto |
|---|---|---|---|
| Twemoji (desenhos dos emoji) — Twitter, Inc. e colaboradores | web (`@twemoji/svg`, copiado para o pacote em `internal/harmony/api/assets/app/`), Windows (`windows/native/telas/emoji/`) | CC-BY 4.0 (desenhos); MIT (pacote `@twemoji/svg`, Samuel Kopp) | <https://creativecommons.org/licenses/by/4.0/>, [`licencas/twemoji-svg/`](licencas/twemoji-svg/LICENSE), [`windows/native/telas/emoji/FONT-LICENSE.md`](windows/native/telas/emoji/FONT-LICENSE.md) |
| Twemoji Mozilla (fonte colorida) | `windows/native/telas/emoji/TwemojiMozilla.ttf` | Apache-2.0 + CC-BY 4.0 | [`windows/native/telas/emoji/FONT-LICENSE.md`](windows/native/telas/emoji/FONT-LICENSE.md) |
| Inter — Rasmus Andersson | `web/harmony/src/fonts/`, Windows | SIL Open Font License 1.1 | [`licencas/Inter/`](licencas/Inter/LICENSE.txt) |
| Ícones do kit HeroUI 3 / Gravity UI Icons — YANDEX LLC | geometria em `web/harmony/src/Icon.tsx`, `windows/app/assets/*.xaml`, Android `Glyphs.kt`, `design/android/icones-do-kit/` | MIT | [`licencas/gravity-ui-icons/`](licencas/gravity-ui-icons/LICENSE) |
| DeepFilterNet 3 — Hendrik Schröter | modelo e `.wasm` em `internal/harmony/api/assets/app/assets/`; `df.dll` no Windows (baixado no build) | MIT ou Apache-2.0 | [`licencas/DeepFilterNet/`](licencas/DeepFilterNet/) |
| @sapphi-red/web-noise-suppressor | cliente web | MIT | [`licencas/web-noise-suppressor/`](licencas/web-noise-suppressor/LICENSE) |
| libwebrtc (pacotes Shiguredo) e o remendo em `android/webrtc-patch/` | Windows e Android (binários baixados no build, fora do repositório) | BSD-3-Clause + concessão de patentes do WebRTC; Apache-2.0 (empacotamento) | [`windows/app/assets/webrtc-NOTICE.txt`](windows/app/assets/webrtc-NOTICE.txt) |
| NVIDIA Video Codec SDK | `windows/native/gpu/nvenc.*` (o SDK em si é baixado no build e não está aqui) | MIT nos arquivos do repositório; EULA da NVIDIA no SDK | [`windows/native/gpu/NOTICE-nvidia.txt`](windows/native/gpu/NOTICE-nvidia.txt) |
| Windows App SDK, WinUI, WebView2 (Microsoft) | baixados no build do Windows, fora do repositório | licenças da Microsoft | [`windows/app/THIRD-PARTY.md`](windows/app/THIRD-PARTY.md) |
| Sons de aviso (entrar, sair, mutar, desmutar, tela começou, tela acabou) | `web/harmony/src/sounds/`, `windows/res/sons/`, `android/app/src/main/res/raw/` e o pacote compilado | **Não são AGPL.** Gerados com o ElevenLabs Sound Effects em plano pago, sob os termos do ElevenLabs | <https://elevenlabs.io/sound-effects-terms> |
| Toque de chamada (`toque.mp3`, `toque.wav`, `som_toque.mp3`) | os mesmos lugares | **Não é AGPL.** Pixabay Content License | <https://pixabay.com/service/license-summary/> |
| Dependências Go, npm e Gradle | `go.mod`, `web/package.json`, `android/app/build.gradle.kts` | MIT, BSD, Apache-2.0 e domínio público (SQLite) | nos próprios pacotes |
