// ==WindhawkMod==
// @id              desktop-icon-overlay-remover
// @name            Desktop Icon Overlay Remover
// @description     Remove a seta de atalho, os ícones de sincronização do OneDrive e o escudo de administrador somente na Área de Trabalho (o Explorador de Arquivos não é afetado)
// @version         1.2
// @author          aguirre-technsci, Claude (Anthropic)
// @github          https://github.com/aguirre-technsci
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -lcomctl32 -lshell32 -luser32 -lole32 -ladvapi32
// @license         MIT
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*

Author: [aguirre-technsci](https://github.com/aguirre-technsci), co-authored
with Claude (Anthropic).

# Desktop Icon Overlay Remover

Remove os ícones sobrepostos (overlays) dos ícones da **Área de Trabalho**:

- a seta de atalho;
- os ícones de sincronização do OneDrive (nuvem, check verde, setas de
  sincronização, erro, bloqueado, compartilhado);
- o escudo de administrador (UAC);
- qualquer outro overlay, no modo "Remover todos os overlays".

Janelas do Explorador de Arquivos e diálogos Abrir/Salvar continuam normais.
Cada remoção pode ser ligada ou desligada nas configurações do mod.

## Como funciona

A Área de Trabalho é uma `SHELLDLL_DefView` filha de `Progman` ou `WorkerW`. O
mod faz subclass só dessa janela e, depois que o tratamento original de
`LVN_GETDISPINFOW` roda, zera os bits de overlay (`LVIS_OVERLAYMASK`).

Dois modos de escolher o que remover:

- **Remover todos os overlays** (padrão): zera qualquer overlay. Não depende de
  identificar qual programa criou o overlay, então pega sincronização do
  OneDrive, escudo de administrador, seta e o que mais aparecer.
- **Seletivo** (se o modo acima estiver desligado):
  - seta de atalho: `SHGetIconOverlayIndexW(NULL, IDO_SHGIOI_LINK)`;
  - OneDrive: o mod lê os handlers registrados em `ShellIconOverlayIdentifiers`
    cujo nome contém `OneDrive` ou `SkyDrive`, pergunta a cada um qual ícone ele
    usa (`IShellIconOverlayIdentifier::GetOverlayInfo`) e converte para o índice
    de overlay do sistema. Esse caminho depende de o handler responder como o mod
    espera; se algum ícone sobrar, use o modo "todos".

Nada é gravado no registro; ao desativar o mod, a subclass é removida e a Área
de Trabalho é repintada.

## Diagnóstico

Na aba de log do Windhawk, cada índice de overlay removido aparece uma vez
("Overlay N removido"). Se um ícone continuar na tela e nenhuma linha nova
aparecer, ele não passa por este caminho; envie o log.

## Limitações conhecidas

- Com ícones da Área de Trabalho muito grandes (Ctrl + roda do mouse), o
  Windows pode desenhar o overlay por outro caminho e o ícone pode continuar
  aparecendo. Teste nesse tamanho.
- No modo seletivo, os handlers do OneDrive são lidos ao carregar o mod. Se o
  OneDrive for instalado ou atualizado depois, desative e ative o mod.
- Testar após ativar com o Explorer rodando, após reiniciar o Explorer e após
  reiniciar o PC.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- RemoveAllOverlays: true
  $name: Remover todos os overlays (recomendado)
  $description: Remove qualquer ícone sobreposto dos ícones da Área de Trabalho, incluindo sincronização do OneDrive e o escudo de administrador. Quando ligado, as duas opções abaixo ficam sem efeito.
- RemoveShortcutArrow: true
  $name: Remover a seta de atalho
  $description: Modo seletivo. Remove a seta de atalho dos ícones da Área de Trabalho
- RemoveOneDriveOverlays: true
  $name: Remover ícones de sincronização do OneDrive
  $description: Modo seletivo. Remove nuvem, check verde, setas de sincronização e similares dos ícones da Área de Trabalho
*/
// ==/WindhawkModSettings==

#include <windhawk_utils.h>

#include <commctrl.h>
#include <objbase.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace {

std::mutex g_mutex;
std::vector<HWND> g_subclassed;  // janelas SHELLDLL_DefView da Área de Trabalho
HANDLE g_resolveThread = nullptr;  // thread que resolve os overlays do OneDrive

std::atomic<bool> g_removeAll{true};
std::atomic<bool> g_removeArrow{true};
std::atomic<bool> g_removeOneDrive{true};
std::atomic<int> g_linkOverlayIndex{-1};  // índice (base 1) do overlay de atalho
// Bit N ligado = o overlay de índice N (1..15) pertence ao OneDrive.
std::atomic<uint32_t> g_oneDriveOverlayMask{0};
// Bit N ligado = o overlay N já foi registrado no log (uma linha por índice).
std::atomic<uint32_t> g_loggedOverlays{0};

// Interface IShellIconOverlayIdentifier (IID 0C6C4200-C589-11D0-999A-00C04FD655E1),
// declarada aqui para não depender dos cabeçalhos do toolchain.
struct IOverlayIdentifierLite : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE IsMemberOf(PCWSTR pwszPath,
                                                 DWORD dwAttrib) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetOverlayInfo(PWSTR pwszIconFile,
                                                     int cchMax,
                                                     int* pIndex,
                                                     DWORD* pdwFlags) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPriority(int* pPriority) = 0;
};

const GUID kIID_OverlayIdentifierLite = {
    0x0C6C4200,
    0xC589,
    0x11D0,
    {0x99, 0x9A, 0x00, 0xC0, 0x4F, 0xD6, 0x55, 0xE1}};

constexpr DWORD kIconFileFlag = 0x1;   // ISIOI_ICONFILE
constexpr DWORD kIconIndexFlag = 0x2;  // ISIOI_ICONINDEX

// --- utilidades ------------------------------------------------------------

void LoadSettings() {
    g_removeAll = Wh_GetIntSetting(L"RemoveAllOverlays") != 0;
    g_removeArrow = Wh_GetIntSetting(L"RemoveShortcutArrow") != 0;
    g_removeOneDrive = Wh_GetIntSetting(L"RemoveOneDriveOverlays") != 0;
}

bool IsDesktopHostClass(HWND hWnd) {
    WCHAR cls[32];
    if (!hWnd || !GetClassNameW(hWnd, cls, ARRAYSIZE(cls))) {
        return false;
    }
    return wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0;
}

bool ContainsNoCase(const WCHAR* haystack, const WCHAR* needle) {
    size_t len = wcslen(needle);
    for (; *haystack; ++haystack) {
        if (_wcsnicmp(haystack, needle, len) == 0) {
            return true;
        }
    }
    return false;
}

// Resolve o índice do overlay de atalho. Chamado ao anexar à Área de Trabalho,
// nunca dentro do handler de notificação (a primeira chamada pode carregar o
// overlay na lista de imagens do sistema).
void ResolveLinkOverlayIndex() {
    if (g_linkOverlayIndex.load() > 0) {
        return;
    }
    int index = SHGetIconOverlayIndexW(nullptr, IDO_SHGIOI_LINK);
    if (index > 0) {
        g_linkOverlayIndex.store(index);
    } else {
        Wh_Log(L"Overlay de atalho não encontrado (retorno %d)", index);
    }
}

void RefreshDesktopView(HWND hDefView) {
    HWND hList = FindWindowExW(hDefView, nullptr, L"SysListView32", nullptr);
    if (hList) {
        InvalidateRect(hList, nullptr, TRUE);
    }
}

void RefreshAllDesktopViews() {
    std::vector<HWND> windows;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        windows = g_subclassed;
    }
    for (HWND hDefView : windows) {
        if (IsWindow(hDefView)) {
            RefreshDesktopView(hDefView);
        }
    }
}

// --- overlays do OneDrive (modo seletivo) ---------------------------------------

// Lê os handlers de overlay do OneDrive/SkyDrive registrados no sistema e
// devolve a máscara de bits com os índices de overlay deles. Precisa rodar em
// uma thread com COM inicializado (apartment).
uint32_t ComputeOneDriveOverlayMask() {
    uint32_t mask = 0;

    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\"
                      L"ShellIconOverlayIdentifiers",
                      0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        Wh_Log(L"ShellIconOverlayIdentifiers não pôde ser aberto");
        return 0;
    }

    for (DWORD i = 0;; i++) {
        WCHAR name[256];
        DWORD nameLen = ARRAYSIZE(name);
        LONG result = RegEnumKeyExW(hKey, i, name, &nameLen, nullptr, nullptr,
                                    nullptr, nullptr);
        if (result == ERROR_NO_MORE_ITEMS) {
            break;
        }
        if (result != ERROR_SUCCESS) {
            continue;
        }

        if (!ContainsNoCase(name, L"onedrive") &&
            !ContainsNoCase(name, L"skydrive")) {
            continue;
        }

        WCHAR clsidStr[64];
        DWORD clsidSize = sizeof(clsidStr);
        if (RegGetValueW(hKey, name, nullptr, RRF_RT_REG_SZ, nullptr, clsidStr,
                         &clsidSize) != ERROR_SUCCESS) {
            continue;
        }

        CLSID clsid;
        if (FAILED(CLSIDFromString(clsidStr, &clsid))) {
            continue;
        }

        IOverlayIdentifierLite* identifier = nullptr;
        HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER,
                                      kIID_OverlayIdentifierLite,
                                      reinterpret_cast<void**>(&identifier));
        if (FAILED(hr) || !identifier) {
            Wh_Log(L"CoCreateInstance falhou para %s (0x%08X)", name,
                   static_cast<unsigned>(hr));
            continue;
        }

        WCHAR iconFile[MAX_PATH] = {};
        int iconIndex = 0;
        DWORD flags = 0;
        if (SUCCEEDED(identifier->GetOverlayInfo(iconFile, MAX_PATH, &iconIndex,
                                                 &flags)) &&
            (flags & kIconFileFlag)) {
            int overlayIndex = SHGetIconOverlayIndexW(
                iconFile, (flags & kIconIndexFlag) ? iconIndex : 0);
            if (overlayIndex > 0 && overlayIndex < 32) {
                mask |= 1u << overlayIndex;
                Wh_Log(L"%s -> overlay %d", name, overlayIndex);
            }
        }

        identifier->Release();
    }

    RegCloseKey(hKey);
    return mask;
}

DWORD WINAPI ResolveOneDriveThreadProc(LPVOID) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    g_oneDriveOverlayMask.store(ComputeOneDriveOverlayMask());

    if (SUCCEEDED(hr)) {
        CoUninitialize();
    }

    RefreshAllDesktopViews();
    return 0;
}

// Inicia a resolução em segundo plano (não trava a inicialização do Explorer).
void StartResolveOneDrive() {
    std::lock_guard<std::mutex> lock(g_mutex);

    if (g_resolveThread) {
        if (WaitForSingleObject(g_resolveThread, 0) != WAIT_OBJECT_0) {
            return;  // ainda rodando
        }
        CloseHandle(g_resolveThread);
        g_resolveThread = nullptr;
    }

    g_resolveThread =
        CreateThread(nullptr, 0, ResolveOneDriveThreadProc, nullptr, 0, nullptr);
}

void StopResolveOneDrive() {
    HANDLE hThread = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        hThread = g_resolveThread;
        g_resolveThread = nullptr;
    }
    if (hThread) {
        WaitForSingleObject(hThread, 5000);
        CloseHandle(hThread);
    }
}

// --- subclass da Área de Trabalho -------------------------------------------

void StripOverlays(NMLVDISPINFOW* di) {
    if (!(di->item.mask & LVIF_STATE) ||
        !(di->item.stateMask & LVIS_OVERLAYMASK)) {
        return;
    }

    int overlay = static_cast<int>((di->item.state & LVIS_OVERLAYMASK) >> 8);
    if (overlay <= 0) {
        return;
    }

    bool strip = false;
    if (g_removeAll.load()) {
        // Sincronização, escudo de administrador, seta e qualquer outro.
        strip = true;
    } else if (g_removeArrow.load() && overlay == g_linkOverlayIndex.load()) {
        strip = true;
    } else if (g_removeOneDrive.load() && overlay < 32 &&
               ((g_oneDriveOverlayMask.load() >> overlay) & 1u)) {
        strip = true;
    }

    if (strip) {
        di->item.state &= ~static_cast<UINT>(LVIS_OVERLAYMASK);

        // Uma linha de log por índice, para diagnóstico.
        if (overlay < 32) {
            uint32_t bit = 1u << overlay;
            if (!(g_loggedOverlays.fetch_or(bit) & bit)) {
                Wh_Log(L"Overlay %d removido", overlay);
            }
        }
    }
}

LRESULT CALLBACK DesktopViewSubclassProc(HWND hWnd,
                                         UINT uMsg,
                                         WPARAM wParam,
                                         LPARAM lParam,
                                         DWORD_PTR /*dwRefData*/) {
    // O handler original roda primeiro; só depois ajustamos o resultado.
    LRESULT result = DefSubclassProc(hWnd, uMsg, wParam, lParam);

    if (uMsg == WM_NOTIFY) {
        auto* hdr = reinterpret_cast<NMHDR*>(lParam);
        if (hdr && hdr->code == LVN_GETDISPINFOW) {
            StripOverlays(reinterpret_cast<NMLVDISPINFOW*>(lParam));
        }
    } else if (uMsg == WM_NCDESTROY) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_subclassed.erase(
            std::remove(g_subclassed.begin(), g_subclassed.end(), hWnd),
            g_subclassed.end());
    }

    return result;
}

void AttachToDesktopView(HWND hDefView) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (std::find(g_subclassed.begin(), g_subclassed.end(), hDefView) !=
            g_subclassed.end()) {
            return;
        }
    }

    ResolveLinkOverlayIndex();

    // Fora do lock: a chamada pode esperar a thread dona da janela.
    if (!WindhawkUtils::SetWindowSubclassFromAnyThread(
            hDefView, DesktopViewSubclassProc, 0)) {
        Wh_Log(L"SetWindowSubclassFromAnyThread falhou: %p", hDefView);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_subclassed.push_back(hDefView);
    }

    RefreshDesktopView(hDefView);
    Wh_Log(L"Subclass aplicada na Área de Trabalho: %p", hDefView);
}

// --- descoberta da Área de Trabalho ------------------------------------------

BOOL CALLBACK EnumWorkerWProc(HWND hWnd, LPARAM) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hWnd, &pid);
    if (pid != GetCurrentProcessId()) {
        return TRUE;
    }

    WCHAR cls[32];
    if (GetClassNameW(hWnd, cls, ARRAYSIZE(cls)) && wcscmp(cls, L"WorkerW") == 0) {
        HWND hDefView =
            FindWindowExW(hWnd, nullptr, L"SHELLDLL_DefView", nullptr);
        if (hDefView) {
            AttachToDesktopView(hDefView);
        }
    }
    return TRUE;
}

void AttachToExistingDesktop() {
    // Área de Trabalho padrão: SHELLDLL_DefView sob Progman.
    HWND hProgman = FindWindowW(L"Progman", nullptr);
    if (hProgman) {
        HWND hDefView =
            FindWindowExW(hProgman, nullptr, L"SHELLDLL_DefView", nullptr);
        if (hDefView) {
            AttachToDesktopView(hDefView);
        }
    }

    // Com papel de parede animado/slideshow, a view é movida para um WorkerW.
    EnumWindows(EnumWorkerWProc, 0);
}

// --- hook: pega a Área de Trabalho criada depois do mod (boot, reinício) -----

using CreateWindowExW_t = decltype(&CreateWindowExW);
CreateWindowExW_t CreateWindowExW_Original;

HWND WINAPI CreateWindowExW_Hook(DWORD dwExStyle,
                                 LPCWSTR lpClassName,
                                 LPCWSTR lpWindowName,
                                 DWORD dwStyle,
                                 int X,
                                 int Y,
                                 int nWidth,
                                 int nHeight,
                                 HWND hWndParent,
                                 HMENU hMenu,
                                 HINSTANCE hInstance,
                                 LPVOID lpParam) {
    HWND hWnd = CreateWindowExW_Original(dwExStyle, lpClassName, lpWindowName,
                                         dwStyle, X, Y, nWidth, nHeight,
                                         hWndParent, hMenu, hInstance, lpParam);

    if (hWnd && !IS_INTRESOURCE(lpClassName) &&
        _wcsicmp(lpClassName, L"SHELLDLL_DefView") == 0 &&
        IsDesktopHostClass(hWndParent)) {
        AttachToDesktopView(hWnd);
    }

    return hWnd;
}

}  // namespace

BOOL Wh_ModInit() {
    Wh_Log(L">");

    LoadSettings();

    if (!WindhawkUtils::SetFunctionHook(CreateWindowExW, CreateWindowExW_Hook,
                                        &CreateWindowExW_Original)) {
        Wh_Log(L"Falha ao instalar o hook de CreateWindowExW");
        return FALSE;
    }

    return TRUE;
}

void Wh_ModAfterInit() {
    Wh_Log(L">");
    // Resolve os overlays do OneDrive em segundo plano (modo seletivo) e anexa
    // à Área de Trabalho, que já existe se o Explorer estava rodando.
    StartResolveOneDrive();
    AttachToExistingDesktop();
}

void Wh_ModSettingsChanged() {
    Wh_Log(L">");
    LoadSettings();
    g_loggedOverlays.store(0);  // novo log de diagnóstico após mudar a config
    StartResolveOneDrive();     // não faz nada se já está rodando
    RefreshAllDesktopViews();
}

void Wh_ModUninit() {
    Wh_Log(L">");

    StopResolveOneDrive();

    std::vector<HWND> windows;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        windows.swap(g_subclassed);
    }

    for (HWND hDefView : windows) {
        if (IsWindow(hDefView)) {
            WindhawkUtils::RemoveWindowSubclassFromAnyThread(
                hDefView, DesktopViewSubclassProc);
            RefreshDesktopView(hDefView);
        }
    }
}
