#include "gui.h"

#include "util.h"
#include "search.h"
#include "ytdlp.h"

#include <commctrl.h>
#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace ytb {
namespace {

constexpr int kW = 760;
constexpr int kH = 520;
constexpr int kPerSite = 8;
constexpr UINT WM_EVT = WM_APP + 1;
const wchar_t *kClass = L"YtAudioBrowserView";
bool g_classRegistered = false;

HINSTANCE thisModule() {
   static HMODULE h = [] {
      HMODULE m = nullptr;
      GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(&thisModule), &m);
      return m;
   }();
   return h;
}

enum Id {
   IDC_QUERY = 100,
   IDC_SEARCH,
   IDC_STOP,
   IDC_REFRESH,
   IDC_SITE0 = 110, // one per entry in sites()
   IDC_FMT = 130,
   IDC_RESULTS,
   IDC_DOWNLOAD,
   IDC_UPDATE,
   IDC_OPEN,
   IDC_PROGRESS,
   IDC_STATUS,
   IDC_FILES_LABEL,
   IDC_FILES,
};

struct Event {
   enum Kind { EvHit, EvSearchDone, EvProgress, EvItemDone, EvDownloadDone, EvStatus } kind = EvStatus;
   unsigned gen = 0;
   Hit hit;
   std::string text;
   double pct = 0;
   std::wstring path;
   bool ok = false;
   int index = 0; // position in a multi-download (0-based), or number saved on EvDownloadDone
   int total = 0;
};

struct Worker {
   std::thread th;
   std::shared_ptr<std::atomic<bool>> done = std::make_shared<std::atomic<bool>>(false);
};

// Minimal IDropSource for DoDragDrop; lives on the stack for the drag.
class DropSource : public IDropSource {
 public:
   STDMETHODIMP QueryInterface(REFIID riid, void **out) override {
      if (riid == IID_IUnknown || riid == IID_IDropSource) {
         *out = static_cast<IDropSource *>(this);
         return S_OK;
      }
      *out = nullptr;
      return E_NOINTERFACE;
   }
   STDMETHODIMP_(ULONG) AddRef() override { return 2; }
   STDMETHODIMP_(ULONG) Release() override { return 1; }
   STDMETHODIMP QueryContinueDrag(BOOL escape, DWORD keys) override {
      if (escape)
         return DRAGDROP_S_CANCEL;
      if (!(keys & MK_LBUTTON))
         return DRAGDROP_S_DROP;
      return S_OK;
   }
   STDMETHODIMP GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }
};

// Starts a shell file drag (CF_HDROP + shell ID list), which DAW arrangement
// views and sampler slots accept as an ordinary file drop. All files must be
// in one folder (downloads are).
bool dragFiles(HWND owner, const std::vector<std::wstring> &paths) {
   std::vector<PIDLIST_ABSOLUTE> pidls;
   std::vector<PCUITEMID_CHILD> children;
   IShellFolder *folder = nullptr;
   IDataObject *obj = nullptr;
   fs::path dir;
   HRESULT hr = E_FAIL;

   for (const auto &p : paths) {
      PIDLIST_ABSOLUTE pidl = nullptr;
      if (FAILED(SHParseDisplayName(p.c_str(), nullptr, &pidl, 0, nullptr)))
         continue;
      if (!folder) {
         PCUITEMID_CHILD child = nullptr;
         if (FAILED(SHBindToParent(pidl, IID_IShellFolder, reinterpret_cast<void **>(&folder), &child))) {
            CoTaskMemFree(pidl);
            continue;
         }
         dir = fs::path(p).parent_path();
         children.push_back(child);
      } else if (fs::path(p).parent_path() == dir) {
         children.push_back(ILFindLastID(pidl));
      } else {
         CoTaskMemFree(pidl);
         continue;
      }
      pidls.push_back(pidl);
   }
   if (folder && !children.empty())
      hr = folder->GetUIObjectOf(owner, (UINT)children.size(), children.data(), IID_IDataObject,
                                 nullptr, reinterpret_cast<void **>(&obj));
   if (SUCCEEDED(hr)) {
      DropSource src;
      DWORD effect = 0;
      hr = DoDragDrop(obj, &src, DROPEFFECT_COPY | DROPEFFECT_LINK, &effect);
   }
   if (obj)
      obj->Release();
   if (folder)
      folder->Release();
   for (auto *p : pidls)
      CoTaskMemFree(p);
   return SUCCEEDED(hr);
}

std::vector<int> selectedRows(HWND lv) {
   std::vector<int> rows;
   for (int i = ListView_GetNextItem(lv, -1, LVNI_SELECTED); i >= 0;
        i = ListView_GetNextItem(lv, i, LVNI_SELECTED))
      rows.push_back(i);
   return rows;
}

// Asks before opening many windows or files at once.
bool confirmMany(HWND owner, size_t n, const wchar_t *what) {
   if (n <= 5)
      return true;
   std::wstring q = L"Open " + std::to_wstring(n) + L" " + what + L"?";
   return MessageBoxW(owner, q.c_str(), L"YT Audio Browser", MB_YESNO | MB_ICONQUESTION) == IDYES;
}

// Moves files to the Recycle Bin. Returns how many are gone afterwards.
size_t recycleFiles(HWND owner, const std::vector<std::wstring> &paths) {
   std::wstring list;
   for (const auto &p : paths) {
      list += p;
      list.push_back(L'\0');
   }
   list.push_back(L'\0');
   SHFILEOPSTRUCTW op{};
   op.hwnd = owner;
   op.wFunc = FO_DELETE;
   op.pFrom = list.c_str();
   op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
   SHFileOperationW(&op);
   size_t gone = 0;
   std::error_code ec;
   for (const auto &p : paths)
      if (!fs::exists(p, ec))
         ++gone;
   return gone;
}

bool copyText(HWND owner, const std::wstring &text) {
   if (!OpenClipboard(owner))
      return false;
   bool ok = false;
   EmptyClipboard();
   const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
   if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
      if (void *p = GlobalLock(mem)) {
         memcpy(p, text.c_str(), bytes);
         GlobalUnlock(mem);
         ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
      }
      if (!ok)
         GlobalFree(mem); // the clipboard owns it only after a successful SetClipboardData
   }
   CloseClipboard();
   return ok;
}

std::wstring formatDuration(int sec) {
   if (sec <= 0)
      return L"";
   wchar_t b[32];
   if (sec >= 3600)
      swprintf(b, 32, L"%d:%02d:%02d", sec / 3600, (sec / 60) % 60, sec % 60);
   else
      swprintf(b, 32, L"%d:%02d", sec / 60, sec % 60);
   return b;
}

std::wstring trim(std::wstring s) {
   auto sp = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; };
   while (!s.empty() && sp(s.back()))
      s.pop_back();
   size_t i = 0;
   while (i < s.size() && sp(s[i]))
      ++i;
   return s.substr(i);
}

#ifndef SORT_DIGITSASNUMBERS
#define SORT_DIGITSASNUMBERS 0x00000008
#endif

// The lParam stored with a list row (a hit index or a file id).
LPARAM rowParam(HWND lv, int row) {
   LVITEMW it{};
   it.mask = LVIF_PARAM;
   it.iItem = row;
   SendMessageW(lv, LVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&it));
   return it.lParam;
}

// Case-insensitive, digit-aware ("track 2" before "track 10"), like Explorer.
int compareText(const std::wstring &a, const std::wstring &b) {
   int r = CompareStringEx(LOCALE_NAME_USER_DEFAULT, LINGUISTIC_IGNORECASE | SORT_DIGITSASNUMBERS,
                           a.c_str(), (int)a.size(), b.c_str(), (int)b.size(), nullptr, nullptr, 0);
   return r == 0 ? 0 : r - CSTR_EQUAL;
}

template <class T> int compare3(T a, T b) { return (a > b) - (a < b); }

ULONGLONG ft64(const FILETIME &f) {
   return (static_cast<ULONGLONG>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
}

std::wstring formatModified(const FILETIME &ft) {
   FILETIME local;
   SYSTEMTIME st;
   if (ft64(ft) == 0 || !FileTimeToLocalFileTime(&ft, &local) || !FileTimeToSystemTime(&local, &st))
      return L"";
   wchar_t b[32];
   swprintf(b, 32, L"%04d-%02d-%02d %02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
   return b;
}

std::wstring formatSize(ULONGLONG bytes) {
   wchar_t b[32];
   if (bytes < 1024)
      swprintf(b, 32, L"%d B", (int)bytes);
   else if (bytes < 1024ull * 1024)
      swprintf(b, 32, L"%.1f KB", bytes / 1024.0);
   else if (bytes < 1024ull * 1024 * 1024)
      swprintf(b, 32, L"%.1f MB", bytes / (1024.0 * 1024));
   else
      swprintf(b, 32, L"%.2f GB", bytes / (1024.0 * 1024 * 1024));
   return b;
}

// ---- settings: %LOCALAPPDATA%\YtAudioBrowser\settings.ini ------------------

std::wstring settingsPath() {
   wchar_t base[MAX_PATH];
   DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
   if (!n || n >= MAX_PATH)
      return {};
   std::wstring dir = std::wstring(base, n) + L"\\YtAudioBrowser";
   CreateDirectoryW(dir.c_str(), nullptr); // already existing is fine
   return dir + L"\\settings.ini";
}

std::wstring iniGet(const std::wstring &file, const wchar_t *section, const wchar_t *key) {
   wchar_t buf[512];
   DWORD n = GetPrivateProfileStringW(section, key, L"", buf, 512, file.c_str());
   return std::wstring(buf, n);
}

void iniSet(const std::wstring &file, const wchar_t *section, const wchar_t *key,
            const std::wstring &value) {
   WritePrivateProfileStringW(section, key, value.c_str(), file.c_str());
}

// "12,0,-3" -> {12, 0, -3}; anything malformed -> empty.
std::vector<int> parseInts(const std::wstring &s) {
   std::vector<int> v;
   if (s.empty())
      return v;
   size_t i = 0;
   for (;;) {
      size_t j = s.find(L',', i);
      if (j == std::wstring::npos)
         j = s.size();
      std::wstring tok = s.substr(i, j - i);
      if (tok.empty())
         return {};
      wchar_t *end = nullptr;
      long x = wcstol(tok.c_str(), &end, 10);
      if (!end || *end != L'\0')
         return {};
      v.push_back((int)x);
      if (j == s.size())
         break;
      i = j + 1;
   }
   return v;
}

} // namespace

struct Gui::Impl {
   HWND hwnd = nullptr;
   HFONT font = nullptr;
   double scale = 1.0;
   bool oleInit = false;

   HWND query = nullptr, results = nullptr, files = nullptr, status = nullptr, progress = nullptr;
   HWND btnSearch = nullptr, btnStop = nullptr, btnRefresh = nullptr, btnDownload = nullptr, fmtCombo = nullptr;
   std::vector<HWND> siteChecks;

   // Row <-> data mapping: every row stores a stable id in its lParam (a index
   // into `hits`, or into `fileStore`), so sorting the list never breaks it.
   std::vector<Hit> hits;
   struct FileEntry {
      std::wstring path, name;
      FILETIME mtime{};
      ULONGLONG size = 0;
      bool alive = false;
   };
   std::vector<FileEntry> fileStore; // removed files stay as dead slots so ids stay valid

   struct SortState {
      int col = -1; // -1: default order (results: as found; downloads: newest first)
      bool asc = true;
   };
   SortState resultsSort, filesSort;

   std::mutex qMutex;
   std::deque<Event> queue;
   std::vector<Worker> workers;
   std::shared_ptr<std::atomic<bool>> searchCancel = std::make_shared<std::atomic<bool>>(false);
   std::shared_ptr<std::atomic<bool>> jobCancel = std::make_shared<std::atomic<bool>>(false);
   unsigned gen = 0;
   int pendingSearches = 0;
   bool downloading = false;
   std::wstring lastQuery;      // last submitted search, for Refresh
   std::vector<bool> lastSites; // which site boxes were ticked for it
   std::wstring savedName; // last file saved by the running download
   std::string searchError;

   Impl() {
      HDC dc = GetDC(nullptr);
      scale = GetDeviceCaps(dc, LOGPIXELSX) / 96.0;
      ReleaseDC(nullptr, dc);
   }

   ~Impl() {
      stopWorkers();
      if (hwnd)
         DestroyWindow(hwnd);
      if (font)
         DeleteObject(font);
      if (oleInit)
         OleUninitialize();
   }

   int S(int v) const { return (int)(v * scale + 0.5); }

   // ---- window plumbing -------------------------------------------------

   static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
      Impl *self = nullptr;
      if (m == WM_NCCREATE) {
         self = static_cast<Impl *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
         SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
         self->hwnd = h;
      } else {
         self = reinterpret_cast<Impl *>(GetWindowLongPtrW(h, GWLP_USERDATA));
      }
      if (!self)
         return DefWindowProcW(h, m, w, l);
      LRESULT r = self->handle(m, w, l);
      if (m == WM_NCDESTROY) {
         SetWindowLongPtrW(h, GWLP_USERDATA, 0);
         self->hwnd = nullptr;
      }
      return r;
   }

   static LRESULT CALLBACK editProc(HWND h, UINT m, WPARAM w, LPARAM l) {
      WNDPROC orig = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(h, GWLP_USERDATA));
      if (m == WM_GETDLGCODE)
         return CallWindowProcW(orig, h, m, w, l) | DLGC_WANTALLKEYS;
      if (m == WM_CHAR && w == VK_RETURN)
         return 0;
      if (m == WM_KEYDOWN && w == VK_RETURN) {
         SendMessageW(GetParent(h), WM_COMMAND, MAKEWPARAM(IDC_SEARCH, BN_CLICKED), 0);
         return 0;
      }
      return CallWindowProcW(orig, h, m, w, l);
   }

   HWND ctl(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h,
            int id, DWORD ex = 0) {
      HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h),
                               hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                               thisModule(), nullptr);
      if (c && font)
         SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
      return c;
   }

   void addColumn(HWND lv, int i, const wchar_t *text, int w) {
      LVCOLUMNW c{};
      c.mask = LVCF_TEXT | LVCF_WIDTH;
      c.pszText = const_cast<wchar_t *>(text);
      c.cx = S(w);
      SendMessageW(lv, LVM_INSERTCOLUMNW, (WPARAM)i, reinterpret_cast<LPARAM>(&c));
   }

   void addRow(HWND lv, int index, const std::vector<std::wstring> &cols, LPARAM param) {
      LVITEMW it{};
      it.mask = LVIF_TEXT | LVIF_PARAM;
      it.lParam = param;
      it.iItem = index;
      it.pszText = const_cast<wchar_t *>(cols[0].c_str());
      int row = (int)SendMessageW(lv, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&it));
      for (size_t i = 1; i < cols.size(); ++i) {
         LVITEMW s{};
         s.iSubItem = (int)i;
         s.pszText = const_cast<wchar_t *>(cols[i].c_str());
         SendMessageW(lv, LVM_SETITEMTEXTW, (WPARAM)row, reinterpret_cast<LPARAM>(&s));
      }
   }

   void build() {
      INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS};
      InitCommonControlsEx(&icc);

      HRESULT hr = OleInitialize(nullptr);
      oleInit = (hr == S_OK || hr == S_FALSE);

      font = CreateFontW(-MulDiv(9, (int)(96 * scale), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

      query = ctl(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, 10, 10, 450, 26, IDC_QUERY,
                  WS_EX_CLIENTEDGE);
      SendMessageW(query, EM_SETCUEBANNER, TRUE,
                   reinterpret_cast<LPARAM>(L"Search, or paste any URL yt-dlp supports"));
      SetWindowLongPtrW(query, GWLP_USERDATA,
                        SetWindowLongPtrW(query, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(editProc)));

      btnSearch = ctl(L"BUTTON", L"Search", WS_TABSTOP | BS_DEFPUSHBUTTON, 470, 10, 90, 26, IDC_SEARCH);
      btnStop = ctl(L"BUTTON", L"Stop", WS_TABSTOP | BS_PUSHBUTTON, 568, 10, 70, 26, IDC_STOP);
      btnRefresh = ctl(L"BUTTON", L"Refresh", WS_TABSTOP | BS_PUSHBUTTON, 646, 10, 80, 26, IDC_REFRESH);

      const auto &ss = sites();
      for (size_t i = 0; i < ss.size(); ++i) {
         const int col = (int)(i % 5), row = (int)(i / 5);
         HWND c = ctl(L"BUTTON", widen(ss[i].label).c_str(), WS_TABSTOP | BS_AUTOCHECKBOX,
                      10 + col * 112, 44 + row * 24, 110, 22, IDC_SITE0 + (int)i);
         SendMessageW(c, BM_SETCHECK, ss[i].defaultOn ? BST_CHECKED : BST_UNCHECKED, 0);
         siteChecks.push_back(c);
      }

      ctl(L"STATIC", L"Format:", SS_LEFT, 590, 47, 50, 20, 0);
      fmtCombo = ctl(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 645, 44, 95, 200,
                     IDC_FMT);
      for (const wchar_t *f : {L"flac", L"wav", L"mp3", L"m4a", L"opus"})
         SendMessageW(fmtCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(f));
      SendMessageW(fmtCombo, CB_SETCURSEL, 0, 0);

      results = ctl(L"SysListView32", L"",
                    WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS, 10, 98, 740, 204,
                    IDC_RESULTS, WS_EX_CLIENTEDGE);
      SendMessageW(results, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                   LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
      addColumn(results, 0, L"Site", 85);
      addColumn(results, 1, L"Title", 400);
      addColumn(results, 2, L"Uploader", 160);
      addColumn(results, 3, L"Length", 70);

      btnDownload = ctl(L"BUTTON", L"Download selected", WS_TABSTOP | BS_PUSHBUTTON, 10, 312, 130,
                        28, IDC_DOWNLOAD);
      ctl(L"BUTTON", L"Update yt-dlp", WS_TABSTOP | BS_PUSHBUTTON, 148, 312, 100, 28, IDC_UPDATE);
      ctl(L"BUTTON", L"Open folder", WS_TABSTOP | BS_PUSHBUTTON, 256, 312, 90, 28, IDC_OPEN);
      progress = ctl(PROGRESS_CLASSW, L"", PBS_SMOOTH, 360, 316, 390, 20, IDC_PROGRESS);
      SendMessageW(progress, PBM_SETRANGE32, 0, 100);

      status = ctl(L"STATIC", L"Ready. Double-click a result to download it.",
                   SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX, 10, 348, 740, 20, IDC_STATUS);

      ctl(L"STATIC", L"Downloads (drag a row into your DAW):", SS_LEFT, 10, 374, 740, 18,
          IDC_FILES_LABEL);
      files = ctl(L"SysListView32", L"",
                  WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS,
                  10, 394, 740, 112, IDC_FILES, WS_EX_CLIENTEDGE);
      SendMessageW(files, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                   LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
      addColumn(files, 0, L"Name", 400);
      addColumn(files, 1, L"Modified", 150);
      addColumn(files, 2, L"Size", 90);

      loadSettings();
      updateHeaders(false);
      updateHeaders(true);
      loadExistingFiles();

      Tools t;
      std::wstring why;
      if (!locateTools(t, why))
         setStatus(why);
   }

   void loadExistingFiles() {
      std::error_code ec;
      std::vector<std::pair<ULONGLONG, std::wstring>> found;
      for (auto &e : fs::directory_iterator(downloadDir(), ec)) {
         std::wstring ext = e.path().extension().wstring();
         for (auto &c : ext)
            c = (wchar_t)towlower(c);
         if (ext != L".wav" && ext != L".flac" && ext != L".mp3" && ext != L".m4a" &&
             ext != L".opus" && ext != L".ogg" && ext != L".aac")
            continue;
         WIN32_FILE_ATTRIBUTE_DATA d{};
         ULONGLONG t = 0;
         if (GetFileAttributesExW(e.path().c_str(), GetFileExInfoStandard, &d))
            t = ft64(d.ftLastWriteTime);
         found.emplace_back(t, e.path().wstring());
      }
      std::sort(found.begin(), found.end(),
                [](const auto &a, const auto &b) { return a.first > b.first; });
      if (found.size() > 200)
         found.resize(200);
      for (auto &f : found)
         addFile(f.second, false, false);
      if (filesSort.col >= 0)
         applySort(true);
   }

   void removeFileRow(size_t id) {
      LVFINDINFOW fi{};
      fi.flags = LVFI_PARAM;
      fi.lParam = (LPARAM)id;
      int row = ListView_FindItem(files, -1, &fi);
      if (row >= 0)
         ListView_DeleteItem(files, row);
      fileStore[id].alive = false;
      fileStore[id].path.clear();
   }

   void addFile(const std::wstring &path, bool atTop, bool resort = true) {
      // A re-download overwrites the same file: replace its row instead of duplicating it.
      for (size_t i = 0; i < fileStore.size(); ++i)
         if (fileStore[i].alive && fileStore[i].path == path)
            removeFileRow(i);

      FileEntry e;
      e.path = path;
      e.name = fs::path(path).filename().wstring();
      e.alive = true;
      WIN32_FILE_ATTRIBUTE_DATA d{};
      if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) {
         e.mtime = d.ftLastWriteTime;
         e.size = (static_cast<ULONGLONG>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
      }
      fileStore.push_back(e);
      const int index = atTop ? 0 : ListView_GetItemCount(files);
      addRow(files, index, {e.name, formatModified(e.mtime), formatSize(e.size)},
             (LPARAM)(fileStore.size() - 1));
      if (resort && filesSort.col >= 0)
         applySort(true);
   }

   // ---- sorting ---------------------------------------------------------

   int compareHits(size_t a, size_t b) const {
      if (a >= hits.size() || b >= hits.size())
         return 0;
      const Hit &x = hits[a], &y = hits[b];
      int c = 0;
      switch (resultsSort.col) {
      case 0: c = compareText(widen(x.site), widen(y.site)); break;
      case 1: c = compareText(widen(x.title), widen(y.title)); break;
      case 2: c = compareText(widen(x.uploader), widen(y.uploader)); break;
      case 3: {
         // Unknown lengths go last whichever way the column is sorted.
         const bool ux = x.durationSec <= 0, uy = y.durationSec <= 0;
         if (ux != uy)
            return ux ? 1 : -1;
         c = compare3(x.durationSec, y.durationSec);
         break;
      }
      default: return compare3(a, b); // as found
      }
      if (c == 0)
         return compare3(a, b);
      return resultsSort.asc ? c : -c;
   }

   int compareFiles(size_t a, size_t b) const {
      if (a >= fileStore.size() || b >= fileStore.size())
         return 0;
      const FileEntry &x = fileStore[a], &y = fileStore[b];
      int c = 0;
      switch (filesSort.col) {
      case 0: c = compareText(x.name, y.name); break;
      case 1: c = compare3(ft64(x.mtime), ft64(y.mtime)); break;
      case 2: c = compare3(x.size, y.size); break;
      default: // newest first
         c = compare3(ft64(y.mtime), ft64(x.mtime));
         return c ? c : compareText(x.name, y.name);
      }
      if (c == 0)
         c = compareText(x.name, y.name);
      return filesSort.asc ? c : -c;
   }

   static int CALLBACK resultsCompareCb(LPARAM a, LPARAM b, LPARAM self) {
      return reinterpret_cast<Impl *>(self)->compareHits((size_t)a, (size_t)b);
   }
   static int CALLBACK filesCompareCb(LPARAM a, LPARAM b, LPARAM self) {
      return reinterpret_cast<Impl *>(self)->compareFiles((size_t)a, (size_t)b);
   }

   void applySort(bool isFiles) {
      ListView_SortItems(isFiles ? files : results, isFiles ? filesCompareCb : resultsCompareCb,
                         reinterpret_cast<LPARAM>(this));
   }

   // Column titles with an arrow on the sorted column.
   void updateHeaders(bool isFiles) {
      static const wchar_t *const kResultNames[] = {L"Site", L"Title", L"Uploader", L"Length"};
      static const wchar_t *const kFileNames[] = {L"Name", L"Modified", L"Size"};
      HWND lv = isFiles ? files : results;
      const SortState &st = isFiles ? filesSort : resultsSort;
      const wchar_t *const *names = isFiles ? kFileNames : kResultNames;
      const int n = isFiles ? 3 : 4;
      for (int i = 0; i < n; ++i) {
         std::wstring t = names[i];
         if (i == st.col)
            t += st.asc ? L" \u25B2" : L" \u25BC";
         LVCOLUMNW c{};
         c.mask = LVCF_TEXT;
         c.pszText = t.data();
         SendMessageW(lv, LVM_SETCOLUMNW, (WPARAM)i, reinterpret_cast<LPARAM>(&c));
      }
   }

   // Like Explorer: first click sorts ascending, second descending, third goes
   // back to the default order.
   void onColumnClick(bool isFiles, int col) {
      SortState &st = isFiles ? filesSort : resultsSort;
      if (st.col != col) {
         st.col = col;
         st.asc = true;
      } else if (st.asc) {
         st.asc = false;
      } else {
         st.col = -1;
         st.asc = true;
      }
      updateHeaders(isFiles);
      applySort(isFiles);
      saveSettings();
   }

   // ---- settings --------------------------------------------------------

   std::wstring columnWidths(HWND lv, int n) {
      std::wstring out;
      for (int i = 0; i < n; ++i) {
         int w = ListView_GetColumnWidth(lv, i);
         if (w <= 0)
            return {}; // window already torn down: keep what is saved
         out += (i ? L"," : L"") + std::to_wstring(w);
      }
      return out;
   }

   void saveSettings() {
      if (!fmtCombo)
         return;
      const std::wstring f = settingsPath();
      if (f.empty())
         return;
      const auto &ss = sites();
      for (size_t i = 0; i < ss.size() && i < siteChecks.size(); ++i)
         iniSet(f, L"Sites", widen(ss[i].label).c_str(),
                SendMessageW(siteChecks[i], BM_GETCHECK, 0, 0) == BST_CHECKED ? L"1" : L"0");
      wchar_t fmt[16] = L"";
      int ci = (int)SendMessageW(fmtCombo, CB_GETCURSEL, 0, 0);
      if (ci >= 0)
         SendMessageW(fmtCombo, CB_GETLBTEXT, (WPARAM)ci, reinterpret_cast<LPARAM>(fmt));
      if (fmt[0])
         iniSet(f, L"General", L"Format", fmt);
      iniSet(f, L"General", L"ResultsSort",
             std::to_wstring(resultsSort.col) + L"," + (resultsSort.asc ? L"1" : L"0"));
      iniSet(f, L"General", L"FilesSort",
             std::to_wstring(filesSort.col) + L"," + (filesSort.asc ? L"1" : L"0"));
      if (auto w = columnWidths(results, 4); !w.empty())
         iniSet(f, L"Columns", L"Results", w);
      if (auto w = columnWidths(files, 3); !w.empty())
         iniSet(f, L"Columns", L"Files", w);
   }

   void loadSettings() {
      const std::wstring f = settingsPath();
      if (f.empty())
         return;
      const auto &ss = sites();
      for (size_t i = 0; i < ss.size() && i < siteChecks.size(); ++i) {
         std::wstring v = iniGet(f, L"Sites", widen(ss[i].label).c_str());
         if (v == L"1" || v == L"0")
            SendMessageW(siteChecks[i], BM_SETCHECK, v == L"1" ? BST_CHECKED : BST_UNCHECKED, 0);
      }
      std::wstring fmt = iniGet(f, L"General", L"Format");
      if (!fmt.empty()) {
         int idx = (int)SendMessageW(fmtCombo, CB_FINDSTRINGEXACT, (WPARAM)-1,
                                     reinterpret_cast<LPARAM>(fmt.c_str()));
         if (idx >= 0)
            SendMessageW(fmtCombo, CB_SETCURSEL, (WPARAM)idx, 0);
      }
      auto readSort = [&](const wchar_t *key, SortState &st, int ncols) {
         auto v = parseInts(iniGet(f, L"General", key));
         if (v.size() == 2 && v[0] >= -1 && v[0] < ncols) {
            st.col = v[0];
            st.asc = v[1] != 0;
         }
      };
      readSort(L"ResultsSort", resultsSort, 4);
      readSort(L"FilesSort", filesSort, 3);
      auto readWidths = [&](const wchar_t *key, HWND lv, int ncols) {
         auto v = parseInts(iniGet(f, L"Columns", key));
         if ((int)v.size() != ncols)
            return;
         for (int i = 0; i < ncols; ++i)
            if (v[(size_t)i] >= 20 && v[(size_t)i] <= 2000)
               ListView_SetColumnWidth(lv, i, v[(size_t)i]);
      };
      readWidths(L"Results", results, 4);
      readWidths(L"Files", files, 3);
   }

   void setStatus(const std::wstring &s) { SetWindowTextW(status, s.c_str()); }
   void setStatus(const std::string &s) { setStatus(widen(s)); }

   // ---- workers ---------------------------------------------------------

   void reap() {
      for (auto it = workers.begin(); it != workers.end();) {
         if (it->done->load()) {
            if (it->th.joinable())
               it->th.join();
            it = workers.erase(it);
         } else {
            ++it;
         }
      }
   }

   void stopWorkers() {
      searchCancel->store(true);
      jobCancel->store(true);
      for (auto &w : workers)
         if (w.th.joinable())
            w.th.join();
      workers.clear();
   }

   void post(Event e) {
      {
         std::lock_guard<std::mutex> lk(qMutex);
         queue.push_back(std::move(e));
      }
      if (hwnd)
         PostMessageW(hwnd, WM_EVT, 0, 0);
   }

   template <class Fn> void spawn(Fn fn) {
      reap();
      Worker w;
      auto done = w.done;
      w.th = std::thread([fn = std::move(fn), done]() mutable {
         fn();
         done->store(true);
      });
      workers.push_back(std::move(w));
   }

   // ---- actions ---------------------------------------------------------

   std::wstring getText(HWND h) {
      int n = GetWindowTextLengthW(h);
      std::wstring s((size_t)n + 1, L'\0');
      GetWindowTextW(h, s.data(), n + 1);
      s.resize((size_t)n);
      return s;
   }

   void doSearch() {
      std::wstring q = trim(getText(query));
      if (q.empty())
         return;
      Tools t;
      std::wstring why;
      if (!locateTools(t, why)) {
         setStatus(why);
         return;
      }

      // A pasted URL is listed directly; otherwise each ticked site is searched.
      static const Site kUrlSite = {"URL", Site::Prefix, "", true};
      std::vector<Site> jobs;
      const std::string qu = narrow(q);
      const bool isUrl = q.rfind(L"http://", 0) == 0 || q.rfind(L"https://", 0) == 0;
      if (isUrl) {
         jobs.push_back(kUrlSite);
      } else {
         const auto &ss = sites();
         for (size_t i = 0; i < ss.size(); ++i)
            if (SendMessageW(siteChecks[i], BM_GETCHECK, 0, 0) == BST_CHECKED)
               jobs.push_back(ss[i]);
      }
      if (jobs.empty()) {
         setStatus(L"Tick at least one site, or paste a URL.");
         return;
      }

      lastQuery = q;
      lastSites.clear();
      for (HWND c : siteChecks)
         lastSites.push_back(SendMessageW(c, BM_GETCHECK, 0, 0) == BST_CHECKED);

      searchCancel->store(true);
      searchCancel = std::make_shared<std::atomic<bool>>(false);
      ++gen;
      hits.clear();
      searchError.clear();
      ListView_DeleteAllItems(results);
      pendingSearches = (int)jobs.size();
      setStatus(L"Searching " + std::to_wstring(jobs.size()) + L" site(s)...");

      for (auto &job : jobs) {
         auto cancel = searchCancel;
         unsigned g = gen;
         spawn([this, t, cancel, g, site = job, isUrl, qu]() {
            std::string err;
            auto onHit = [&](Hit &&h) {
               Event e;
               e.kind = Event::EvHit;
               e.gen = g;
               e.hit = std::move(h);
               post(std::move(e));
            };
            int rc = isUrl ? searchOne(t, "URL", qu, kPerSite * 6, *cancel, onHit, err)
                           : searchSite(t, site, qu, kPerSite, *cancel, onHit, err);
            Event e;
            e.kind = Event::EvSearchDone;
            e.gen = g;
            e.ok = (rc == 0 || rc == -2);
            if (!err.empty() && rc != -2)
               e.text = std::string(site.label) + ": " + err;
            post(std::move(e));
         });
      }
   }

   // Where to show a context menu, with Explorer-style selection: right-clicking
   // an unselected row selects just that row; right-clicking inside the current
   // selection keeps it. The menu key anchors to the first selected row.
   bool menuAnchor(HWND lv, int &x, int &y) {
      if (x == -1 && y == -1) {
         int idx = ListView_GetNextItem(lv, -1, LVNI_SELECTED);
         if (idx < 0)
            return false;
         RECT r{};
         ListView_GetItemRect(lv, idx, &r, LVIR_BOUNDS);
         POINT p{r.left + S(20), r.bottom};
         ClientToScreen(lv, &p);
         x = p.x;
         y = p.y;
         return true;
      }
      LVHITTESTINFO ht{};
      ht.pt = POINT{x, y};
      ScreenToClient(lv, &ht.pt);
      int idx = ListView_HitTest(lv, &ht);
      if (idx < 0)
         return false;
      if (!(ListView_GetItemState(lv, idx, LVIS_SELECTED) & LVIS_SELECTED)) {
         ListView_SetItemState(lv, -1, 0, LVIS_SELECTED);
         ListView_SetItemState(lv, idx, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
      }
      return true;
   }

   // Hit indices of the selected result rows, in the order shown.
   std::vector<size_t> selectedHits() {
      std::vector<size_t> v;
      for (int row : selectedRows(results)) {
         size_t i = (size_t)rowParam(results, row);
         if (i < hits.size())
            v.push_back(i);
      }
      return v;
   }

   // Ids (fileStore indices) of the selected download rows, in the order shown.
   std::vector<size_t> selectedFiles() {
      std::vector<size_t> v;
      for (int row : selectedRows(files)) {
         size_t id = (size_t)rowParam(files, row);
         if (id < fileStore.size() && fileStore[id].alive)
            v.push_back(id);
      }
      return v;
   }

   void copySelectedLinks() {
      std::wstring text;
      size_t n = 0;
      for (size_t i : selectedHits()) {
         if (n++)
            text += L"\r\n";
         text += widen(hits[i].url);
      }
      if (!n)
         return;
      setStatus(!copyText(hwnd, text)  ? std::wstring(L"Could not copy to the clipboard.")
                : n == 1               ? std::wstring(L"Link copied.")
                                       : std::to_wstring(n) + L" links copied.");
   }

   void openSelectedInBrowser() {
      std::vector<std::wstring> urls;
      for (size_t i : selectedHits()) {
         std::wstring u = widen(hits[i].url);
         // Only web links are handed to the shell.
         if (u.rfind(L"http://", 0) == 0 || u.rfind(L"https://", 0) == 0)
            urls.push_back(u);
      }
      if (urls.empty()) {
         setStatus(L"Nothing to open: the selection has no web links.");
         return;
      }
      if (!confirmMany(hwnd, urls.size(), L"links in your browser"))
         return;
      size_t opened = 0;
      for (const auto &u : urls)
         if (reinterpret_cast<INT_PTR>(
                 ShellExecuteW(hwnd, L"open", u.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32)
            ++opened;
      setStatus(opened == 0 ? std::wstring(L"Could not open the browser.")
                            : L"Opened " + std::to_wstring(opened) + L" in your default browser.");
   }

   void openSelectedFiles() {
      auto sel = selectedFiles();
      if (sel.empty() || !confirmMany(hwnd, sel.size(), L"files"))
         return;
      size_t opened = 0;
      for (size_t i : sel)
         if (reinterpret_cast<INT_PTR>(ShellExecuteW(hwnd, L"open", fileStore[i].path.c_str(), nullptr,
                                                     nullptr, SW_SHOWNORMAL)) > 32)
            ++opened;
      if (opened == 0)
         setStatus(L"Could not open the file. No default app may be set for this format.");
      else
         setStatus(L"Opened " + std::to_wstring(opened) + L" file(s).");
   }

   void deleteSelectedFiles() {
      auto sel = selectedFiles();
      if (sel.empty())
         return;
      std::vector<std::wstring> paths;
      for (size_t i : sel)
         paths.push_back(fileStore[i].path);
      recycleFiles(hwnd, paths);

      // Drop the rows whose files are really gone.
      size_t gone = 0;
      std::error_code ec;
      for (size_t id : sel) {
         if (fs::exists(fileStore[id].path, ec))
            continue;
         removeFileRow(id);
         ++gone;
      }
      if (gone == sel.size())
         setStatus(L"Moved " + std::to_wstring(gone) + L" file(s) to the Recycle Bin.");
      else
         setStatus(L"Could not delete " + std::to_wstring(sel.size() - gone) +
                   L" file(s). They may be in use by your DAW.");
   }

   void showResultsMenu(int x, int y) {
      if (!menuAnchor(results, x, y))
         return;
      const size_t n = selectedHits().size();
      if (!n)
         return;
      HMENU menu = CreatePopupMenu();
      AppendMenuW(menu, MF_STRING, 1, n > 1 ? L"Copy links" : L"Copy link");
      AppendMenuW(menu, MF_STRING, 2, L"Open in browser");
      int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, x, y, 0, hwnd, nullptr);
      DestroyMenu(menu);
      if (cmd == 1)
         copySelectedLinks();
      else if (cmd == 2)
         openSelectedInBrowser();
   }

   void showFilesMenu(int x, int y) {
      if (!menuAnchor(files, x, y) || selectedFiles().empty())
         return;
      HMENU menu = CreatePopupMenu();
      AppendMenuW(menu, MF_STRING, 1, L"Open");
      AppendMenuW(menu, MF_STRING, 2, L"Delete");
      int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, x, y, 0, hwnd, nullptr);
      DestroyMenu(menu);
      if (cmd == 1)
         openSelectedFiles();
      else if (cmd == 2)
         deleteSelectedFiles();
   }

   // Re-runs the last search with the same sites, and re-reads the downloads
   // folder so the list matches what is on disk.
   void doRefresh() {
      ListView_DeleteAllItems(files);
      fileStore.clear();
      loadExistingFiles();
      if (lastQuery.empty()) {
         setStatus(L"Downloads list refreshed. Run a search first to refresh results.");
         return;
      }
      SetWindowTextW(query, lastQuery.c_str());
      for (size_t i = 0; i < siteChecks.size() && i < lastSites.size(); ++i)
         SendMessageW(siteChecks[i], BM_SETCHECK, lastSites[i] ? BST_CHECKED : BST_UNCHECKED, 0);
      doSearch();
   }

   void doStop() {
      searchCancel->store(true);
      jobCancel->store(true);
      pendingSearches = 0;
      setStatus(L"Stopped.");
   }

   void doDownload() {
      if (downloading)
         return;
      auto sel = selectedHits();
      if (sel.empty()) {
         setStatus(L"Select a result first.");
         return;
      }
      Tools t;
      std::wstring why;
      if (!locateTools(t, why)) {
         setStatus(why);
         return;
      }
      wchar_t fmt[16] = L"flac";
      int ci = (int)SendMessageW(fmtCombo, CB_GETCURSEL, 0, 0);
      if (ci >= 0)
         SendMessageW(fmtCombo, CB_GETLBTEXT, (WPARAM)ci, reinterpret_cast<LPARAM>(fmt));

      std::vector<std::string> urls;
      for (size_t i : sel)
         urls.push_back(hits[i].url);

      downloading = true;
      savedName.clear();
      EnableWindow(btnDownload, FALSE);
      SendMessageW(progress, PBM_SETPOS, 0, 0);
      setStatus(urls.size() == 1 ? L"Downloading: " + widen(hits[sel[0]].title)
                                 : L"Downloading " + std::to_wstring(urls.size()) + L" results...");

      jobCancel = std::make_shared<std::atomic<bool>>(false);
      auto cancel = jobCancel;
      // One worker, so downloads run one after another.
      spawn([this, t, cancel, urls, format = std::wstring(fmt)]() {
         const int total = (int)urls.size();
         int saved = 0;
         bool cancelled = false;
         std::string lastErr;
         for (int i = 0; i < total && !cancel->load(); ++i) {
            std::wstring path;
            std::string err;
            int rc = downloadAudio(t, urls[(size_t)i], format, *cancel,
                                   [&](double pct) {
                                      Event e;
                                      e.kind = Event::EvProgress;
                                      e.pct = pct;
                                      e.index = i;
                                      e.total = total;
                                      post(std::move(e));
                                   }, path, err);
            if (rc == -2) {
               cancelled = true;
               break;
            }
            Event e;
            e.kind = Event::EvItemDone;
            e.ok = (rc == 0);
            e.path = path;
            e.text = err;
            e.index = i;
            e.total = total;
            if (rc == 0)
               ++saved;
            else
               lastErr = err.empty() ? std::string("Download failed.") : err;
            post(std::move(e));
         }
         Event e;
         e.kind = Event::EvDownloadDone;
         e.ok = !cancelled && saved == total;
         e.index = saved;
         e.total = total;
         e.text = (cancelled || cancel->load()) ? std::string("Cancelled.") : lastErr;
         post(std::move(e));
      });
   }

   void doUpdate() {
      Tools t;
      std::wstring why;
      if (!locateTools(t, why)) {
         setStatus(why);
         return;
      }
      setStatus(L"Updating yt-dlp...");
      auto cancel = jobCancel;
      spawn([this, t, cancel]() {
         updateYtDlp(t, *cancel, [&](const std::string &line) {
            if (line.empty())
               return;
            Event e;
            e.kind = Event::EvStatus;
            e.text = line;
            post(std::move(e));
         });
      });
   }

   void drain() {
      std::deque<Event> local;
      {
         std::lock_guard<std::mutex> lk(qMutex);
         local.swap(queue);
      }
      for (auto &e : local) {
         switch (e.kind) {
         case Event::EvHit:
            if (e.gen != gen)
               break;
            addRow(results, ListView_GetItemCount(results),
                   {widen(e.hit.site), widen(e.hit.title), widen(e.hit.uploader),
                    formatDuration(e.hit.durationSec)},
                   (LPARAM)hits.size());
            hits.push_back(std::move(e.hit));
            if (resultsSort.col >= 0)
               applySort(false);
            break;
         case Event::EvSearchDone:
            if (e.gen != gen)
               break;
            if (!e.text.empty())
               searchError += (searchError.empty() ? "" : "  |  ") + e.text;
            if (pendingSearches > 0 && --pendingSearches == 0) {
               if (hits.empty())
                  setStatus(searchError.empty() ? std::string("No results.") : searchError);
               else if (searchError.empty())
                  setStatus(std::to_wstring(hits.size()) + L" results. Double-click one to download.");
               else
                  setStatus(std::to_wstring(hits.size()) + L" results. Failed: " + widen(searchError));
            }
            break;
         case Event::EvProgress: {
            SendMessageW(progress, PBM_SETPOS, (WPARAM)(int)e.pct, 0);
            std::wstring prefix = e.total > 1 ? std::to_wstring(e.index + 1) + L"/" +
                                                    std::to_wstring(e.total) + L": "
                                              : std::wstring();
            setStatus(e.pct >= 100.0 ? prefix + L"Converting..."
                                     : prefix + L"Downloading " + std::to_wstring((int)e.pct) + L"%");
            break;
         }
         case Event::EvItemDone:
            if (e.ok) {
               SendMessageW(progress, PBM_SETPOS, 100, 0);
               addFile(e.path, true);
               savedName = fs::path(e.path).filename().wstring();
            }
            break;
         case Event::EvDownloadDone:
            downloading = false;
            EnableWindow(btnDownload, TRUE);
            if (e.ok) {
               setStatus(e.total == 1 ? L"Saved: " + savedName + L"  (drag it from the list below)"
                                      : L"Saved " + std::to_wstring(e.index) +
                                            L" files  (drag them from the list below)");
            } else {
               SendMessageW(progress, PBM_SETPOS, 0, 0);
               std::wstring head = e.total > 1 ? L"Saved " + std::to_wstring(e.index) + L" of " +
                                                     std::to_wstring(e.total) + L". "
                                               : std::wstring();
               setStatus(head + (e.text.empty() ? widen("Download failed.") : widen(e.text)));
            }
            break;
         case Event::EvStatus:
            setStatus(e.text);
            break;
         }
      }
   }

   LRESULT handle(UINT m, WPARAM w, LPARAM l) {
      switch (m) {
      case WM_CREATE:
         build();
         return 0;
      case WM_COMMAND: {
         const int id = LOWORD(w), code = HIWORD(w);
         if (code == BN_CLICKED) {
            switch (id) {
            case IDC_SEARCH: doSearch(); break;
            case IDC_STOP: doStop(); break;
            case IDC_REFRESH: doRefresh(); break;
            case IDC_DOWNLOAD: doDownload(); break;
            case IDC_UPDATE: doUpdate(); break;
            case IDC_OPEN:
               ShellExecuteW(hwnd, L"open", downloadDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
               break;
            default:
               if (id >= IDC_SITE0 && id < IDC_SITE0 + (int)siteChecks.size())
                  saveSettings(); // a site box was ticked or cleared
               break;
            }
         } else if (code == CBN_SELCHANGE && id == IDC_FMT) {
            saveSettings();
         }
         return 0;
      }
      case WM_NOTIFY: {
         auto *nm = reinterpret_cast<NMHDR *>(l);
         if (nm->idFrom == IDC_RESULTS && nm->code == NM_DBLCLK) {
            doDownload();
         } else if (nm->idFrom == IDC_FILES && nm->code == NM_DBLCLK) {
            openSelectedFiles();
         } else if ((nm->idFrom == IDC_RESULTS || nm->idFrom == IDC_FILES) &&
                    nm->code == LVN_COLUMNCLICK) {
            onColumnClick(nm->idFrom == IDC_FILES, reinterpret_cast<NMLISTVIEW *>(l)->iSubItem);
         } else if (nm->idFrom == IDC_FILES && nm->code == LVN_BEGINDRAG) {
            // Drag the whole selection when the grabbed row is part of it.
            const int grabbedRow = reinterpret_cast<NMLISTVIEW *>(l)->iItem;
            const size_t grabbed = grabbedRow >= 0 ? (size_t)rowParam(files, grabbedRow) : (size_t)-1;
            std::vector<std::wstring> paths;
            bool inSelection = false;
            for (size_t id : selectedFiles()) {
               paths.push_back(fileStore[id].path);
               inSelection = inSelection || id == grabbed;
            }
            if (!inSelection) {
               paths.clear();
               if (grabbed < fileStore.size() && fileStore[grabbed].alive)
                  paths.push_back(fileStore[grabbed].path);
            }
            if (!paths.empty() && !dragFiles(hwnd, paths))
               setStatus(L"Could not start the drag. Open the folder and drag from there.");
         } else if ((nm->idFrom == IDC_RESULTS || nm->idFrom == IDC_FILES) && nm->code == LVN_KEYDOWN) {
            const WORD key = reinterpret_cast<NMLVKEYDOWN *>(l)->wVKey;
            const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool isFiles = nm->idFrom == IDC_FILES;
            if (ctrl && key == 'A') {
               ListView_SetItemState(isFiles ? files : results, -1, LVIS_SELECTED, LVIS_SELECTED);
            } else if (ctrl && key == 'C' && !isFiles) {
               copySelectedLinks();
            } else if (key == VK_DELETE && isFiles) {
               deleteSelectedFiles();
            } else if (key == VK_RETURN) {
               if (isFiles)
                  openSelectedFiles();
               else
                  doDownload();
            }
         }
         return 0;
      }
      case WM_CONTEXTMENU:
         if (reinterpret_cast<HWND>(w) == results) {
            showResultsMenu(GET_X_LPARAM(l), GET_Y_LPARAM(l));
            return 0;
         }
         if (reinterpret_cast<HWND>(w) == files) {
            showFilesMenu(GET_X_LPARAM(l), GET_Y_LPARAM(l));
            return 0;
         }
         break;
      case WM_EVT:
         drain();
         return 0;
      case WM_LBUTTONDOWN:
         SetFocus(hwnd);
         return 0;
      case WM_DESTROY:
         saveSettings(); // while the child controls still exist (column widths)
         stopWorkers();
         return 0;
      }
      return DefWindowProcW(hwnd, m, w, l);
   }

   bool create(HWND parent) {
      if (!g_classRegistered) {
         WNDCLASSEXW wc{};
         wc.cbSize = sizeof(wc);
         wc.lpfnWndProc = &Impl::wndProc;
         wc.hInstance = thisModule();
         wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
         wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
         wc.lpszClassName = kClass;
         if (!RegisterClassExW(&wc))
            return false;
         g_classRegistered = true;
      }
      HWND h = CreateWindowExW(0, kClass, L"", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0,
                               S(kW), S(kH), parent, nullptr, thisModule(), this);
      return h != nullptr;
   }
};

Gui::Gui() : d(new Impl) {}
Gui::~Gui() = default;

bool Gui::attach(HWND parent) { return d->hwnd ? false : d->create(parent); }
void Gui::show() {
   if (d->hwnd)
      ShowWindow(d->hwnd, SW_SHOW);
}
void Gui::hide() {
   if (d->hwnd)
      ShowWindow(d->hwnd, SW_HIDE);
}
void Gui::size(uint32_t &w, uint32_t &h) const {
   w = (uint32_t)d->S(kW);
   h = (uint32_t)d->S(kH);
}

void Gui::unregisterWindowClass() {
   if (g_classRegistered) {
      UnregisterClassW(kClass, thisModule());
      g_classRegistered = false;
   }
}

} // namespace ytb
