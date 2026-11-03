#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "chat.h"
#include "version.h"

// Edit these three and rebuild. CHAT_URL is an OpenAI-compatible
// /chat/completions endpoint. CHAT_KEY is the token only, without "Bearer ".
// Leave CHAT_KEY empty for a local server that does not check one.
#define CHAT_URL    L"https://api.openai.com/v1/chat/completions"
#define CHAT_KEY    L""
#define CHAT_MODEL  L"gpt-4o-mini"
#define CHAT_SYSTEM L"你是桌宠 Clawd。用简短口语中文回答，一两句话，不要用 markdown。"

#define CHAT_KEEP 6
#define CHAT_MAX  (256 * 1024)
#define IDC_CHAT_LOG  2001
#define IDC_CHAT_IN   2002
#define IDC_CHAT_SEND 2003
#define WM_CHAT_DONE (WM_APP + 1)   // worker finished: wParam ok, lParam wchar_t * (freed here)

static HINSTANCE g_inst;
static HWND g_owner;
static HWND g_chat_hwnd, g_chat_log, g_chat_in, g_chat_send;
static Pet *g_pet;
static TextBubble *g_bubble;
static HFONT g_chat_font;
static WNDPROC g_chat_edit_proc;
static volatile LONG g_chat_busy;
static wchar_t *g_hist_user[CHAT_KEEP], *g_hist_asst[CHAT_KEEP];
static int g_nhist;
static wchar_t *g_wait_user;

static wchar_t *wcdup(const wchar_t *s)
{
    size_t n = (wcslen(s) + 1) * sizeof(wchar_t);
    wchar_t *d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

static int chat_key_needed(void)
{
    return wcsstr(CHAT_URL, L"api.openai.com") != NULL;
}

typedef struct { char *p; size_t n, cap; int bad; } ChatBuf;

static void buf_add(ChatBuf *b, const void *s, size_t n)
{
    if (b->bad) return;
    if (b->n + n + 1 > b->cap) {
        size_t c = b->cap ? b->cap * 2 : 256;
        while (c < b->n + n + 1) c *= 2;
        char *q = realloc(b->p, c);
        if (!q) { b->bad = 1; return; }
        b->p = q;
        b->cap = c;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

static void buf_json(ChatBuf *b, const wchar_t *w)
{
    int need = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *u = need > 0 ? malloc((size_t)need) : NULL;
    if (!u) { b->bad = 1; return; }
    WideCharToMultiByte(CP_UTF8, 0, w, -1, u, need, NULL, NULL);
    buf_add(b, "\"", 1);
    for (unsigned char *s = (unsigned char *)u; *s; s++) {
        char esc[8];
        if (*s == '"' || *s == '\\') {
            esc[0] = '\\'; esc[1] = (char)*s; esc[2] = 0;
            buf_add(b, esc, 2);
        } else if (*s == '\n') {
            buf_add(b, "\\n", 2);
        } else if (*s == '\r') {
            buf_add(b, "\\r", 2);
        } else if (*s == '\t') {
            buf_add(b, "\\t", 2);
        } else if (*s < 0x20) {
            sprintf(esc, "\\u%04x", *s);
            buf_add(b, esc, 6);
        } else {
            buf_add(b, s, 1);
        }
    }
    buf_add(b, "\"", 1);
    free(u);
}

// The JSON body for one turn, including the lines already said. NULL on failure.
static void buf_lit(ChatBuf *b, const char *s)
{
    buf_add(b, s, strlen(s));
}

static char *chat_body(const wchar_t *user)
{
    ChatBuf b = { 0 };
    buf_lit(&b, "{\"model\":");
    buf_json(&b, CHAT_MODEL);
    buf_lit(&b, ",\"messages\":[{\"role\":\"system\",\"content\":");
    buf_json(&b, CHAT_SYSTEM);
    buf_lit(&b, "}");
    for (int i = 0; i < g_nhist; i++) {
        buf_lit(&b, ",{\"role\":\"user\",\"content\":");
        buf_json(&b, g_hist_user[i]);
        buf_lit(&b, "},{\"role\":\"assistant\",\"content\":");
        buf_json(&b, g_hist_asst[i]);
        buf_lit(&b, "}");
    }
    buf_lit(&b, ",{\"role\":\"user\",\"content\":");
    buf_json(&b, user);
    buf_lit(&b, "}]}");
    if (b.bad) { free(b.p); return NULL; }
    return b.p;
}

static void hist_push(const wchar_t *user, const wchar_t *asst)
{
    if (g_nhist == CHAT_KEEP) {
        free(g_hist_user[0]);
        free(g_hist_asst[0]);
        memmove(g_hist_user, g_hist_user + 1, sizeof g_hist_user[0] * (CHAT_KEEP - 1));
        memmove(g_hist_asst, g_hist_asst + 1, sizeof g_hist_asst[0] * (CHAT_KEEP - 1));
        g_nhist--;
    }
    g_hist_user[g_nhist] = wcdup(user);
    g_hist_asst[g_nhist] = wcdup(asst);
    if (g_hist_user[g_nhist] && g_hist_asst[g_nhist]) g_nhist++;
    else {
        free(g_hist_user[g_nhist]);
        free(g_hist_asst[g_nhist]);
        g_hist_user[g_nhist] = g_hist_asst[g_nhist] = NULL;
    }
}

// Key lookup that does not match inside a string. *val points at the value.
static int json_find_key(const char *s, const char *key, const char **val)
{
    size_t klen = strlen(key);
    int in = 0, esc = 0;
    for (const char *p = s; *p; p++) {
        if (in) {
            if (esc) esc = 0;
            else if (*p == '\\') esc = 1;
            else if (*p == '"') in = 0;
            continue;
        }
        if (*p != '"') continue;
        if (!strncmp(p + 1, key, klen) && p[1 + klen] == '"') {
            const char *q = p + 2 + klen;
            while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
            if (*q == ':') {
                q++;
                while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
                *val = q;
                return 1;
            }
        }
        in = 1;
    }
    return 0;
}

static int hex4(const char *s, unsigned *v)
{
    *v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        unsigned d;
        if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
        else return 0;
        *v = (*v << 4) | d;
    }
    return 1;
}

static void wbuf_ch(wchar_t **p, size_t *n, size_t *cap, wchar_t ch)
{
    if (*n + 2 > *cap) {
        size_t c = *cap ? *cap * 2 : 64;
        wchar_t *q = realloc(*p, c * sizeof(wchar_t));
        if (!q) return;
        *p = q;
        *cap = c;
    }
    (*p)[(*n)++] = ch;
    (*p)[*n] = 0;
}

static void wbuf_cp(wchar_t **p, size_t *n, size_t *cap, unsigned cp)
{
    if (cp >= 0x110000 || (cp >= 0xD800 && cp <= 0xDFFF)) return;
    if (cp >= 0x10000) {
        cp -= 0x10000;
        wbuf_ch(p, n, cap, (wchar_t)(0xD800 + (cp >> 10)));
        wbuf_ch(p, n, cap, (wchar_t)(0xDC00 + (cp & 0x3FF)));
    } else {
        wbuf_ch(p, n, cap, (wchar_t)cp);
    }
}

// p points at the opening quote of a JSON string. Result is malloc'd.
static wchar_t *json_decode(const char *p)
{
    wchar_t *out = NULL;
    size_t n = 0, cap = 0;
    if (*p != '"') return NULL;
    for (p++; *p && *p != '"' && n < 4000; ) {
        unsigned char c = (unsigned char)*p;
        if (c == '\\') {
            char e = p[1];
            if (!e) break;
            p += 2;
            unsigned cp = 0;
            if (e == '"' || e == '\\' || e == '/') cp = (unsigned char)e;
            else if (e == 'n') cp = '\n';
            else if (e == 'r') cp = '\r';
            else if (e == 't') cp = '\t';
            else if (e == 'b') cp = '\b';
            else if (e == 'f') cp = '\f';
            else if (e == 'u' && hex4(p, &cp)) {
                p += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && p[0] == '\\' && p[1] == 'u') {
                    unsigned lo;
                    if (hex4(p + 2, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + (((cp - 0xD800) << 10) | (lo - 0xDC00));
                        p += 6;
                    }
                }
            } else {
                continue;
            }
            wbuf_cp(&out, &n, &cap, cp);
            continue;
        }
        if (c < 0x80) {
            wbuf_cp(&out, &n, &cap, c);
            p++;
        } else {
            int len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
            unsigned cp = c & (len == 4 ? 0x07u : len == 3 ? 0x0Fu : len == 2 ? 0x1Fu : 0x7Fu);
            int ok = len > 1;
            for (int i = 1; i < len; i++) {
                if (!p[i]) { ok = 0; break; }
                unsigned char nx = (unsigned char)p[i];
                if ((nx & 0xC0) != 0x80) { ok = 0; break; }
                cp = (cp << 6) | (nx & 0x3F);
            }
            if (!ok) { p++; continue; }
            wbuf_cp(&out, &n, &cap, cp);
            p += len;
        }
    }
    if (!out) out = wcdup(L"");
    return out;
}

static wchar_t *json_text(const char *at)
{
    if (!at) return NULL;
    if (*at == '"') return json_decode(at);
    if (*at == '[') {
        const char *t = NULL;
        if (json_find_key(at, "text", &t)) return json_decode(t);
    }
    return NULL;
}

static wchar_t *chat_take(const char *resp, int status, int *ok)
{
    const char *val = NULL;
    wchar_t *text = NULL;
    *ok = 0;
    if (status == 200) {
        const char *from = resp;
        for (;;) {
            if (!json_find_key(from, "content", &val)) break;
            text = json_text(val);
            if (text && text[0]) { *ok = 1; return text; }
            free(text);
            text = NULL;
            from = val + 1;
        }
    }
    if (json_find_key(resp, "message", &val) && *val == '"') text = json_decode(val);
    if (text && text[0]) return text;
    free(text);
    wchar_t tmp[64];
    _snwprintf(tmp, 64, L"没连上（HTTP %d）", status);
    tmp[63] = 0;
    return wcdup(status ? tmp : L"网络不太通，没连上。");
}

static char *chat_http(const char *body, int *status)
{
    wchar_t host[256], path[2048];
    URL_COMPONENTS uc = { sizeof uc };
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2048;
    *status = 0;
    if (!WinHttpCrackUrl(CHAT_URL, 0, 0, &uc)) return NULL;

    HINTERNET ses = WinHttpOpen(L"clawd-pet/" CLAWD_VERSION, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (ses) WinHttpSetTimeouts(ses, 10000, 15000, 30000, 90000);
    HINTERNET con = ses ? WinHttpConnect(ses, host, uc.nPort, 0) : NULL;
    DWORD flags = uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET req = con ? WinHttpOpenRequest(con, L"POST", path, NULL, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, flags) : NULL;
    size_t klen = wcslen(CHAT_KEY);
    wchar_t *hdr = malloc((klen + 80) * sizeof(wchar_t));
    char *buf = NULL;
    if (!req || !hdr) goto done;
    if (CHAT_KEY[0])
        _snwprintf(hdr, klen + 80, L"Content-Type: application/json\r\nAuthorization: Bearer %ls\r\n", CHAT_KEY);
    else
        wcscpy(hdr, L"Content-Type: application/json\r\n");
    hdr[klen + 79] = 0;
    DWORD nbody = (DWORD)strlen(body);
    if (!WinHttpSendRequest(req, hdr, (DWORD)-1, (void *)body, nbody, nbody, 0) ||
        !WinHttpReceiveResponse(req, NULL))
        goto done;
    DWORD st = 0, sz = sizeof st;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &st, &sz, WINHTTP_NO_HEADER_INDEX);
    *status = (int)st;
    buf = malloc(CHAT_MAX + 1);
    if (!buf) goto done;
    DWORD n = 0;
    for (;;) {
        DWORD avail = 0, got = 0;
        if (!WinHttpQueryDataAvailable(req, &avail) || !avail) break;
        if (n >= CHAT_MAX) break;
        DWORD want = avail > CHAT_MAX - n ? CHAT_MAX - n : avail;
        if (!WinHttpReadData(req, buf + n, want, &got)) break;
        n += got;
        if (!got) break;
    }
    buf[n] = 0;
done:
    free(hdr);
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    return buf;
}

static DWORD WINAPI chat_thread(void *arg)
{
    char *body = arg;
    int status = 0, ok = 0;
    char *resp = chat_http(body, &status);
    free(body);
    wchar_t *msg = resp ? chat_take(resp, status, &ok) : wcdup(L"网络不太通，没连上。");
    free(resp);
    if (!msg) msg = wcdup(L"没看懂它的回复。");
    HWND notify = g_chat_hwnd;
    if (!notify || !PostMessageW(notify, WM_CHAT_DONE, (WPARAM)ok, (LPARAM)msg)) {
        free(msg);
        InterlockedExchange(&g_chat_busy, 0);
    }
    return 0;
}

static void chat_log(const wchar_t *who, const wchar_t *text)
{
    if (!g_chat_log) return;
    int len = GetWindowTextLengthW(g_chat_log);
    SendMessageW(g_chat_log, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    size_t n = wcslen(who) + wcslen(text) + 8;
    wchar_t *line = malloc(n * sizeof(wchar_t));
    if (!line) return;
    _snwprintf(line, n, L"%ls：%ls\r\n\r\n", who, text);
    line[n - 1] = 0;
    SendMessageW(g_chat_log, EM_REPLACESEL, FALSE, (LPARAM)line);
    SendMessageW(g_chat_log, EM_SCROLLCARET, 0, 0);
    free(line);
}

static void chat_bubble(const wchar_t *text, int cheer)
{
    if (!g_bubble || !g_pet || !text) return;
    wchar_t bit[48];
    int n = 0;
    while (text[n] && text[n] != L'\n' && text[n] != L'\r' && n < 40) {
        bit[n] = text[n];
        n++;
    }
    if (text[n]) bit[n++] = L'…';
    bit[n] = 0;
    tb_show(g_bubble, bit, 15000);
    if (cheer) pet_cheer(g_pet);
}

static void chat_on_reply(int ok, wchar_t *text)
{
    if (g_chat_send) {
        EnableWindow(g_chat_send, TRUE);
        SetWindowTextW(g_chat_send, L"发送");
    }
    if (ok && g_wait_user && text) hist_push(g_wait_user, text);
    chat_log(L"Clawd", text && text[0] ? text : L"它这次没说话。");
    chat_bubble(text && text[0] ? text : L"它这次没说话。", ok);
    free(g_wait_user);
    g_wait_user = NULL;
    free(text);
    InterlockedExchange(&g_chat_busy, 0);
    if (g_chat_in && IsWindowVisible(g_chat_hwnd)) SetFocus(g_chat_in);
}

static void chat_submit(void)
{
    if (g_chat_busy || !g_chat_in) return;
    int n = GetWindowTextLengthW(g_chat_in);
    if (n <= 0) return;
    if (n > 2000) n = 2000;
    wchar_t *user = malloc(((size_t)n + 1) * sizeof(wchar_t));
    if (!user) return;
    GetWindowTextW(g_chat_in, user, n + 1);
    if (wcsspn(user, L" \t\r\n") == wcslen(user)) { free(user); return; }
    if (!CHAT_URL[0] || !CHAT_MODEL[0] || (!CHAT_KEY[0] && chat_key_needed())) {
        chat_log(L"Clawd", L"先在 chat.c 里填好 CHAT_URL、CHAT_KEY 和 CHAT_MODEL，再重新编译。");
        free(user);
        return;
    }
    char *body = chat_body(user);
    if (!body) {
        chat_log(L"Clawd", L"这句话太长了，我没装下。");
        free(user);
        return;
    }
    if (InterlockedCompareExchange(&g_chat_busy, 1, 0)) { free(body); free(user); return; }
    g_wait_user = user;
    SetWindowTextW(g_chat_in, L"");
    chat_log(L"我", user);
    chat_bubble(L"让我想想…", 0);
    EnableWindow(g_chat_send, FALSE);
    SetWindowTextW(g_chat_send, L"……");
    HANDLE t = CreateThread(NULL, 0, chat_thread, body, 0, NULL);
    if (t) {
        CloseHandle(t);
    } else {
        InterlockedExchange(&g_chat_busy, 0);
        EnableWindow(g_chat_send, TRUE);
        SetWindowTextW(g_chat_send, L"发送");
        free(g_wait_user);
        g_wait_user = NULL;
        free(body);
        chat_log(L"Clawd", L"没能发出去。");
    }
}

static LRESULT CALLBACK chat_edit_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN && wp == VK_RETURN && !(GetKeyState(VK_SHIFT) & 0x8000)) {
        chat_submit();
        return 0;
    }
    return CallWindowProcW(g_chat_edit_proc, hwnd, msg, wp, lp);
}

static void chat_layout(void)
{
    RECT r;
    GetClientRect(g_chat_hwnd, &r);
    int m = 10, bh = 28, gap = 8;
    int iw = r.right - m * 2 - 72;
    int ih = r.bottom - m * 2 - bh - gap;
    if (iw < 40) iw = 40;
    if (ih < 40) ih = 40;
    MoveWindow(g_chat_log, m, m, r.right - m * 2, ih, TRUE);
    MoveWindow(g_chat_in, m, m + ih + gap, iw, bh, TRUE);
    MoveWindow(g_chat_send, m + iw + 8, m + ih + gap, 64, bh, TRUE);
}

static LRESULT CALLBACK chat_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        g_chat_hwnd = hwnd;
        UINT dpi = GetDpiForWindow(hwnd);
        g_chat_font = CreateFontW(-MulDiv(15, dpi, 96), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                  0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
        g_chat_log = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                     WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
                                     0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_CHAT_LOG, g_inst, NULL);
        g_chat_in = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                    WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                    0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_CHAT_IN, g_inst, NULL);
        g_chat_send = CreateWindowExW(0, L"BUTTON", L"发送",
                                      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                      0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_CHAT_SEND, g_inst, NULL);
        SendMessageW(g_chat_log, WM_SETFONT, (WPARAM)g_chat_font, TRUE);
        SendMessageW(g_chat_in, WM_SETFONT, (WPARAM)g_chat_font, TRUE);
        SendMessageW(g_chat_send, WM_SETFONT, (WPARAM)g_chat_font, TRUE);
        SendMessageW(g_chat_in, EM_SETLIMITTEXT, 2000, 0);
        g_chat_edit_proc = (WNDPROC)SetWindowLongPtrW(g_chat_in, GWLP_WNDPROC, (LONG_PTR)chat_edit_proc);
        if (!CHAT_KEY[0] && chat_key_needed())
            chat_log(L"Clawd", L"先在 chat.c 里填好 CHAT_URL、CHAT_KEY 和 CHAT_MODEL，再重新编译。密钥不要提交到仓库。");
        else
            chat_log(L"Clawd", L"跟我说点什么吧。");
        return 0;
    }
    case WM_SIZE:
        chat_layout();
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_CHAT_SEND) chat_submit();
        return 0;
    case WM_CHAT_DONE:
        chat_on_reply((int)wp, (wchar_t *)lp);
        return 0;
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_DESTROY:
        if (g_chat_font) DeleteObject(g_chat_font);
        g_chat_font = NULL;
        g_chat_hwnd = g_chat_log = g_chat_in = g_chat_send = NULL;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void chat_forget(TextBubble *bubble)
{
    if (g_bubble == bubble) {
        g_bubble = NULL;
        g_pet = NULL;
    }
}

void chat_open(HWND owner, HINSTANCE inst, Pet *pet, TextBubble *bubble)
{
    g_owner = owner;
    g_inst = inst;
    g_pet = pet;
    g_bubble = bubble;
    if (!g_chat_hwnd) {
        WNDCLASSW wc = { 0 };
        wc.lpfnWndProc = chat_proc;
        wc.hInstance = g_inst;
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = L"ClawdChat";
        RegisterClassW(&wc);
        RECT a = pet->area;
        int w = 420, h = 480;
        int x = (int)pet->x;
        int y = (int)pet->y - h - 12;
        if (x + w > a.right) x = a.right - w - 8;
        if (x < a.left) x = a.left + 8;
        if (y < a.top) y = a.top + 8;
        CreateWindowExW(WS_EX_TOOLWINDOW, L"ClawdChat", L"和 Clawd 说话",
                        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
                        x, y, w, h, g_owner, NULL, g_inst, NULL);
        if (!g_chat_hwnd) return;
    }
    ShowWindow(g_chat_hwnd, SW_SHOW);
    SetForegroundWindow(g_chat_hwnd);
    if (g_chat_in) SetFocus(g_chat_in);
}
