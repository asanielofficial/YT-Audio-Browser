#include "../gui_gtk.h"

#include "../search.h"
#include "../ytdlp.h"

#include <gdk/gdkx.h>
#include <gtk/gtk.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

namespace ytb {
namespace {

constexpr int kPerSite = 8;
constexpr int kDefaultSort = 100; // sort id meaning "the list's natural order"

struct SortState {
   int col = -1; // -1 = natural order
   bool asc = true;
};

std::string formatDuration(int sec) {
   if (sec <= 0)
      return "";
   char b[32];
   if (sec >= 3600)
      std::snprintf(b, sizeof b, "%d:%02d:%02d", sec / 3600, (sec / 60) % 60, sec % 60);
   else
      std::snprintf(b, sizeof b, "%d:%02d", sec / 60, sec % 60);
   return b;
}

std::string formatSize(gint64 bytes) {
   char b[32];
   if (bytes < 1024)
      std::snprintf(b, sizeof b, "%d B", (int)bytes);
   else if (bytes < 1024 * 1024)
      std::snprintf(b, sizeof b, "%.1f KB", bytes / 1024.0);
   else if (bytes < 1024LL * 1024 * 1024)
      std::snprintf(b, sizeof b, "%.1f MB", bytes / (1024.0 * 1024));
   else
      std::snprintf(b, sizeof b, "%.2f GB", bytes / (1024.0 * 1024 * 1024));
   return b;
}

std::string formatModified(gint64 t) {
   if (t <= 0)
      return "";
   char b[32];
   std::time_t tt = (std::time_t)t;
   std::tm tmv{};
   localtime_r(&tt, &tmv);
   std::strftime(b, sizeof b, "%Y-%m-%d %H:%M", &tmv);
   return b;
}

std::string trim(std::string s) {
   auto sp = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
   while (!s.empty() && sp(s.back()))
      s.pop_back();
   size_t i = 0;
   while (i < s.size() && sp(s[i]))
      ++i;
   return s.substr(i);
}

std::string settingsPath() {
   const char *x = std::getenv("XDG_CONFIG_HOME");
   fs::path dir = (x && *x && x[0] == '/') ? fs::path(x)
                                          : fs::path(std::getenv("HOME") ? std::getenv("HOME") : "/tmp") / ".config";
   dir /= "YtAudioBrowser";
   std::error_code ec;
   fs::create_directories(dir, ec);
   return (dir / "settings.ini").string();
}

std::string fileUri(const std::string &path) {
   gchar *u = g_filename_to_uri(path.c_str(), nullptr, nullptr);
   std::string s = u ? u : "";
   g_free(u);
   return s;
}

bool isAudioExt(std::string ext) {
   for (auto &c : ext)
      c = (char)std::tolower((unsigned char)c);
   return ext == ".wav" || ext == ".flac" || ext == ".mp3" || ext == ".m4a" || ext == ".opus" ||
          ext == ".ogg" || ext == ".aac";
}

struct Event {
   enum Kind { EvHit, EvSearchDone, EvProgress, EvItemDone, EvDownloadDone, EvStatus } kind;
   unsigned gen = 0;
   Hit hit;
   bool ok = false;
   std::string text, path;
   double pct = 0;
   int index = 0, total = 0;
};

struct Worker {
   std::thread th;
   std::shared_ptr<std::atomic<bool>> done;
};

struct FileEntry {
   std::string path;
   gint64 mtime = 0;
   gint64 size = 0;
};

enum ResCol { R_SITE, R_TITLE, R_UPLOADER, R_LEN, R_IDX, R_DUR, R_N };
enum FileCol { F_NAME, F_MOD, F_SIZE, F_ID, F_MTIME, F_BYTES, F_N };

} // namespace

struct GtkGui::Impl {
   std::function<void()> onClosed;
   GtkWidget *win = nullptr, *query = nullptr, *btnSearch = nullptr, *btnStop = nullptr,
             *btnRefresh = nullptr, *btnDownload = nullptr, *fmtCombo = nullptr, *progress = nullptr,
             *status = nullptr, *resultsView = nullptr, *filesView = nullptr;
   GtkListStore *resultsStore = nullptr, *filesStore = nullptr;
   std::vector<GtkWidget *> siteChecks;
   std::vector<GtkTreeViewColumn *> resCols, fileCols;
   std::vector<Hit> hits;
   std::vector<FileEntry> fileEntries; // index = id; path empty once deleted
   SortState resultsSort, filesSort;
   std::string lastQuery;
   std::vector<bool> lastSites;
   std::string searchError;
   std::string savedName;
   int pendingSearches = 0;
   unsigned gen = 0;
   bool downloading = false;
   bool loadingSettings = false;
   std::shared_ptr<std::atomic<bool>> searchCancel = std::make_shared<std::atomic<bool>>(false);
   std::shared_ptr<std::atomic<bool>> jobCancel = std::make_shared<std::atomic<bool>>(false);
   std::vector<Worker> workers;
   std::mutex qMutex;
   std::deque<Event> queue;
   guint idleId = 0;

   // ---- small helpers ----------------------------------------------------
   void setStatus(const std::string &s) { gtk_label_set_text(GTK_LABEL(status), s.c_str()); }

   std::string currentFormat() {
      gchar *t = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(fmtCombo));
      std::string f = t ? t : "flac";
      g_free(t);
      return f;
   }

   bool confirmMany(size_t n, const char *what) {
      if (n <= 5)
         return true;
      GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(win), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
                                            GTK_BUTTONS_YES_NO, "Open %zu %s?", n, what);
      int r = gtk_dialog_run(GTK_DIALOG(d));
      gtk_widget_destroy(d);
      return r == GTK_RESPONSE_YES;
   }

   void reap() {
      for (auto it = workers.begin(); it != workers.end();) {
         if (it->done->load()) {
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
      g_idle_add(
          [](gpointer p) -> gboolean {
             static_cast<Impl *>(p)->idleId = 0;
             static_cast<Impl *>(p)->drain();
             return G_SOURCE_REMOVE;
          },
          this);
   }

   template <class Fn> void spawn(Fn fn) {
      reap();
      Worker w;
      w.done = std::make_shared<std::atomic<bool>>(false);
      auto done = w.done;
      w.th = std::thread([fn = std::move(fn), done]() mutable {
         fn();
         done->store(true);
      });
      workers.push_back(std::move(w));
   }

   // ---- settings ---------------------------------------------------------
   static std::string widthsOf(const std::vector<GtkTreeViewColumn *> &cols) {
      std::string s;
      for (size_t i = 0; i < cols.size(); ++i)
         s += (i ? "," : "") + std::to_string(gtk_tree_view_column_get_width(cols[i]));
      return s;
   }

   static std::vector<int> parseInts(const char *s) {
      std::vector<int> v;
      if (!s)
         return v;
      std::stringstream ss(s);
      std::string tok;
      while (std::getline(ss, tok, ','))
         v.push_back(std::atoi(tok.c_str()));
      return v;
   }

   void saveSettings() {
      if (loadingSettings || !win)
         return;
      const std::string f = settingsPath();
      GKeyFile *kf = g_key_file_new();
      g_key_file_load_from_file(kf, f.c_str(), G_KEY_FILE_NONE, nullptr); // keeps [Paths]
      const auto &ss = sites();
      for (size_t i = 0; i < ss.size() && i < siteChecks.size(); ++i)
         g_key_file_set_string(
             kf, "Sites", ss[i].label,
             gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(siteChecks[i])) ? "1" : "0");
      g_key_file_set_string(kf, "General", "Format", currentFormat().c_str());
      g_key_file_set_string(kf, "General", "ResultsSort",
                            (std::to_string(resultsSort.col) + "," + (resultsSort.asc ? "1" : "0")).c_str());
      g_key_file_set_string(kf, "General", "FilesSort",
                            (std::to_string(filesSort.col) + "," + (filesSort.asc ? "1" : "0")).c_str());
      if (gtk_widget_get_mapped(win)) { // widths read as 0 or stale while hidden
         g_key_file_set_string(kf, "Columns", "Results", widthsOf(resCols).c_str());
         g_key_file_set_string(kf, "Columns", "Files", widthsOf(fileCols).c_str());
      }
      g_key_file_save_to_file(kf, f.c_str(), nullptr);
      g_key_file_free(kf);
   }

   void loadSettings() {
      loadingSettings = true;
      GKeyFile *kf = g_key_file_new();
      if (g_key_file_load_from_file(kf, settingsPath().c_str(), G_KEY_FILE_NONE, nullptr)) {
         const auto &ss = sites();
         for (size_t i = 0; i < ss.size() && i < siteChecks.size(); ++i) {
            gchar *v = g_key_file_get_string(kf, "Sites", ss[i].label, nullptr);
            if (v && (!strcmp(v, "1") || !strcmp(v, "0")))
               gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(siteChecks[i]), !strcmp(v, "1"));
            g_free(v);
         }
         if (gchar *fmt = g_key_file_get_string(kf, "General", "Format", nullptr)) {
            GtkTreeModel *m = gtk_combo_box_get_model(GTK_COMBO_BOX(fmtCombo));
            GtkTreeIter it;
            int idx = 0;
            for (bool ok = gtk_tree_model_get_iter_first(m, &it); ok; ok = gtk_tree_model_iter_next(m, &it), ++idx) {
               gchar *t = nullptr;
               gtk_tree_model_get(m, &it, 0, &t, -1);
               if (t && !strcmp(t, fmt))
                  gtk_combo_box_set_active(GTK_COMBO_BOX(fmtCombo), idx);
               g_free(t);
            }
            g_free(fmt);
         }
         auto readSort = [&](const char *key, SortState &st, int ncols) {
            gchar *v = g_key_file_get_string(kf, "General", key, nullptr);
            auto n = parseInts(v);
            g_free(v);
            if (n.size() == 2 && n[0] >= -1 && n[0] < ncols) {
               st.col = n[0];
               st.asc = n[1] != 0;
            }
         };
         readSort("ResultsSort", resultsSort, 4);
         readSort("FilesSort", filesSort, 3);
         auto readWidths = [&](const char *key, std::vector<GtkTreeViewColumn *> &cols) {
            gchar *v = g_key_file_get_string(kf, "Columns", key, nullptr);
            auto n = parseInts(v);
            g_free(v);
            if (n.size() == cols.size())
               for (size_t i = 0; i < n.size(); ++i)
                  if (n[i] >= 30 && n[i] <= 2000)
                     gtk_tree_view_column_set_fixed_width(cols[i], n[i]);
         };
         readWidths("Results", resCols);
         readWidths("Files", fileCols);
      }
      g_key_file_free(kf);
      loadingSettings = false;
   }

   // ---- sorting ----------------------------------------------------------
   static int cmpText(const char *a, const char *b) {
      gchar *ka = g_utf8_collate_key_for_filename(a ? a : "", -1);
      gchar *kb = g_utf8_collate_key_for_filename(b ? b : "", -1);
      int r = std::strcmp(ka, kb);
      g_free(ka);
      g_free(kb);
      return r;
   }

   static int resultsCompare(GtkTreeModel *m, GtkTreeIter *a, GtkTreeIter *b, gpointer data) {
      const int col = GPOINTER_TO_INT(data);
      if (col == kDefaultSort) {
         guint ia, ib;
         gtk_tree_model_get(m, a, R_IDX, &ia, -1);
         gtk_tree_model_get(m, b, R_IDX, &ib, -1);
         return (ia > ib) - (ia < ib);
      }
      if (col == R_LEN) {
         gint da, db;
         gtk_tree_model_get(m, a, R_DUR, &da, -1);
         gtk_tree_model_get(m, b, R_DUR, &db, -1);
         return (da > db) - (da < db);
      }
      gchar *ta = nullptr, *tb = nullptr;
      gtk_tree_model_get(m, a, col, &ta, -1);
      gtk_tree_model_get(m, b, col, &tb, -1);
      int r = cmpText(ta, tb);
      g_free(ta);
      g_free(tb);
      return r;
   }

   static int filesCompare(GtkTreeModel *m, GtkTreeIter *a, GtkTreeIter *b, gpointer data) {
      const int col = GPOINTER_TO_INT(data);
      if (col == F_NAME) {
         gchar *ta = nullptr, *tb = nullptr;
         gtk_tree_model_get(m, a, F_NAME, &ta, -1);
         gtk_tree_model_get(m, b, F_NAME, &tb, -1);
         int r = cmpText(ta, tb);
         g_free(ta);
         g_free(tb);
         return r;
      }
      gint64 va, vb;
      const int key = (col == F_SIZE) ? F_BYTES : F_MTIME;
      gtk_tree_model_get(m, a, key, &va, -1);
      gtk_tree_model_get(m, b, key, &vb, -1);
      int r = (va > vb) - (va < vb);
      return col == kDefaultSort ? -r : r; // natural order: newest first
   }

   void applySort(bool isFiles) {
      GtkListStore *store = isFiles ? filesStore : resultsStore;
      SortState &st = isFiles ? filesSort : resultsSort;
      auto &cols = isFiles ? fileCols : resCols;
      if (st.col < 0)
         gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(store), kDefaultSort, GTK_SORT_ASCENDING);
      else
         gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(store), st.col,
                                              st.asc ? GTK_SORT_ASCENDING : GTK_SORT_DESCENDING);
      for (size_t i = 0; i < cols.size(); ++i) {
         const bool on = (int)i == st.col;
         gtk_tree_view_column_set_sort_indicator(cols[i], on);
         if (on)
            gtk_tree_view_column_set_sort_order(cols[i], st.asc ? GTK_SORT_ASCENDING : GTK_SORT_DESCENDING);
      }
   }

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
      applySort(isFiles);
      saveSettings();
   }

   // ---- lists ------------------------------------------------------------
   void addFile(const std::string &path) {
      FileEntry fe;
      fe.path = path;
      std::error_code ec;
      auto ft = fs::last_write_time(path, ec);
      if (!ec) {
         auto sys = std::chrono::time_point_cast<std::chrono::seconds>(
             ft - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
         fe.mtime = (gint64)sys.time_since_epoch().count();
      }
      auto sz = fs::file_size(path, ec);
      fe.size = ec ? 0 : (gint64)sz;
      fileEntries.push_back(fe);
      GtkTreeIter it;
      gtk_list_store_append(filesStore, &it);
      gtk_list_store_set(filesStore, &it, F_NAME, fs::path(path).filename().string().c_str(), F_MOD,
                         formatModified(fe.mtime).c_str(), F_SIZE, formatSize(fe.size).c_str(), F_ID,
                         (guint)(fileEntries.size() - 1), F_MTIME, fe.mtime, F_BYTES, fe.size, -1);
   }

   void loadExistingFiles() {
      std::error_code ec;
      std::vector<std::pair<gint64, std::string>> found;
      for (auto &e : fs::directory_iterator(downloadDir(), ec)) {
         if (!e.is_regular_file(ec) || !isAudioExt(e.path().extension().string()))
            continue;
         auto ft = fs::last_write_time(e.path(), ec);
         auto sys = std::chrono::time_point_cast<std::chrono::seconds>(
             ft - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
         found.emplace_back((gint64)sys.time_since_epoch().count(), e.path().string());
      }
      std::sort(found.begin(), found.end(), [](auto &a, auto &b) { return a.first > b.first; });
      if (found.size() > 200)
         found.resize(200);
      for (auto &f : found)
         addFile(f.second);
   }

   static std::vector<GtkTreePath *> selectedPaths(GtkWidget *view) {
      GtkTreeModel *m = nullptr;
      GList *l = gtk_tree_selection_get_selected_rows(gtk_tree_view_get_selection(GTK_TREE_VIEW(view)), &m);
      std::vector<GtkTreePath *> v;
      for (GList *p = l; p; p = p->next)
         v.push_back(static_cast<GtkTreePath *>(p->data));
      g_list_free(l); // paths are freed by the caller via freePaths
      return v;
   }
   static void freePaths(std::vector<GtkTreePath *> &v) {
      for (auto *p : v)
         gtk_tree_path_free(p);
      v.clear();
   }

   std::vector<size_t> selectedHits() {
      std::vector<size_t> out;
      auto paths = selectedPaths(resultsView);
      for (auto *p : paths) {
         GtkTreeIter it;
         guint idx;
         if (gtk_tree_model_get_iter(GTK_TREE_MODEL(resultsStore), &it, p)) {
            gtk_tree_model_get(GTK_TREE_MODEL(resultsStore), &it, R_IDX, &idx, -1);
            if (idx < hits.size())
               out.push_back(idx);
         }
      }
      freePaths(paths);
      return out;
   }

   std::vector<size_t> selectedFiles() {
      std::vector<size_t> out;
      auto paths = selectedPaths(filesView);
      for (auto *p : paths) {
         GtkTreeIter it;
         guint id;
         if (gtk_tree_model_get_iter(GTK_TREE_MODEL(filesStore), &it, p)) {
            gtk_tree_model_get(GTK_TREE_MODEL(filesStore), &it, F_ID, &id, -1);
            if (id < fileEntries.size() && !fileEntries[id].path.empty())
               out.push_back(id);
         }
      }
      freePaths(paths);
      return out;
   }

   // ---- actions ----------------------------------------------------------
   void doSearch() {
      std::string q = trim(gtk_entry_get_text(GTK_ENTRY(query)));
      if (q.empty())
         return;
      Tools t;
      PathStr why;
      if (!locateTools(t, why)) {
         setStatus(why);
         return;
      }
      static const Site kUrlSite = {"URL", Site::Prefix, "", true};
      std::vector<Site> jobs;
      const bool isUrl = q.rfind("http://", 0) == 0 || q.rfind("https://", 0) == 0;
      if (isUrl) {
         jobs.push_back(kUrlSite);
      } else {
         const auto &ss = sites();
         for (size_t i = 0; i < ss.size(); ++i)
            if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(siteChecks[i])))
               jobs.push_back(ss[i]);
      }
      if (jobs.empty()) {
         setStatus("Tick at least one site, or paste a URL.");
         return;
      }
      lastQuery = q;
      lastSites.clear();
      for (auto *c : siteChecks)
         lastSites.push_back(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(c)));

      searchCancel->store(true);
      searchCancel = std::make_shared<std::atomic<bool>>(false);
      ++gen;
      hits.clear();
      searchError.clear();
      gtk_list_store_clear(resultsStore);
      pendingSearches = (int)jobs.size();
      setStatus("Searching " + std::to_string(jobs.size()) + " site(s)...");

      for (auto &job : jobs) {
         auto cancel = searchCancel;
         unsigned g = gen;
         spawn([this, t, cancel, g, site = job, isUrl, q]() {
            std::string err;
            auto onHit = [&](Hit &&h) {
               Event e;
               e.kind = Event::EvHit;
               e.gen = g;
               e.hit = std::move(h);
               post(std::move(e));
            };
            int rc = isUrl ? searchOne(t, "URL", q, kPerSite * 6, *cancel, onHit, err)
                           : searchSite(t, site, q, kPerSite, *cancel, onHit, err);
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

   void doRefresh() {
      gtk_list_store_clear(filesStore);
      fileEntries.clear();
      loadExistingFiles();
      if (lastQuery.empty()) {
         setStatus("Downloads list refreshed. Run a search first to refresh results.");
         return;
      }
      gtk_entry_set_text(GTK_ENTRY(query), lastQuery.c_str());
      loadingSettings = true; // do not save while restoring the ticks
      for (size_t i = 0; i < siteChecks.size() && i < lastSites.size(); ++i)
         gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(siteChecks[i]), lastSites[i]);
      loadingSettings = false;
      doSearch();
   }

   void doStop() {
      searchCancel->store(true);
      jobCancel->store(true);
      pendingSearches = 0;
      setStatus("Stopped.");
   }

   void doDownload() {
      if (downloading)
         return;
      auto sel = selectedHits();
      if (sel.empty()) {
         setStatus("Select a result first.");
         return;
      }
      Tools t;
      PathStr why;
      if (!locateTools(t, why)) {
         setStatus(why);
         return;
      }
      std::vector<std::string> urls;
      for (size_t i : sel)
         urls.push_back(hits[i].url);

      downloading = true;
      savedName.clear();
      gtk_widget_set_sensitive(btnDownload, FALSE);
      gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress), 0);
      setStatus(urls.size() == 1 ? "Downloading: " + hits[sel[0]].title
                                 : "Downloading " + std::to_string(urls.size()) + " results...");

      jobCancel = std::make_shared<std::atomic<bool>>(false);
      auto cancel = jobCancel;
      spawn([this, t, cancel, urls, format = currentFormat()]() {
         const int total = (int)urls.size();
         int saved = 0;
         bool cancelled = false;
         std::string lastErr;
         for (int i = 0; i < total && !cancel->load(); ++i) {
            std::string path, err;
            int rc = downloadAudio(t, urls[(size_t)i], format, *cancel,
                                   [&](double pct) {
                                      Event e;
                                      e.kind = Event::EvProgress;
                                      e.pct = pct;
                                      e.index = i;
                                      e.total = total;
                                      post(std::move(e));
                                   },
                                   path, err);
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
      PathStr why;
      if (!locateTools(t, why)) {
         setStatus(why);
         return;
      }
      setStatus("Updating yt-dlp...");
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

   void openUri(const std::string &uri) {
      GError *err = nullptr;
      gboolean ok = gtk_show_uri_on_window(GTK_WINDOW(win), uri.c_str(), GDK_CURRENT_TIME, &err);
      if (!ok)
         setStatus(std::string("Could not open: ") + (err ? err->message : "no handler"));
      if (err)
         g_error_free(err);
   }

   void openFolder() { openUri(fileUri(downloadDir())); }

   void copySelectedLinks() {
      auto sel = selectedHits();
      if (sel.empty())
         return;
      std::string text;
      for (size_t i = 0; i < sel.size(); ++i)
         text += (i ? "\n" : "") + hits[sel[i]].url;
      gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), text.c_str(), -1);
      setStatus(sel.size() == 1 ? "Link copied." : std::to_string(sel.size()) + " links copied.");
   }

   void openSelectedInBrowser() {
      std::vector<std::string> urls;
      for (size_t i : selectedHits()) {
         const std::string &u = hits[i].url;
         if (u.rfind("http://", 0) == 0 || u.rfind("https://", 0) == 0)
            urls.push_back(u);
      }
      if (urls.empty()) {
         setStatus("Nothing to open: the selection has no web links.");
         return;
      }
      if (!confirmMany(urls.size(), "links in your browser"))
         return;
      for (auto &u : urls)
         openUri(u);
      setStatus("Opened " + std::to_string(urls.size()) + " in your default browser.");
   }

   void openSelectedFiles() {
      auto sel = selectedFiles();
      if (sel.empty() || !confirmMany(sel.size(), "files"))
         return;
      for (size_t id : sel)
         openUri(fileUri(fileEntries[id].path));
   }

   void removeFileRow(size_t id) {
      GtkTreeModel *m = GTK_TREE_MODEL(filesStore);
      GtkTreeIter it;
      for (bool ok = gtk_tree_model_get_iter_first(m, &it); ok; ok = gtk_tree_model_iter_next(m, &it)) {
         guint rid;
         gtk_tree_model_get(m, &it, F_ID, &rid, -1);
         if (rid == id) {
            gtk_list_store_remove(filesStore, &it);
            return;
         }
      }
   }

   void deleteSelectedFiles() {
      auto sel = selectedFiles();
      if (sel.empty())
         return;
      GtkWidget *d = gtk_message_dialog_new(
          GTK_WINDOW(win), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
          sel.size() == 1 ? "Move this file to the Trash?" : "Move these %zu files to the Trash?", sel.size());
      int r = gtk_dialog_run(GTK_DIALOG(d));
      gtk_widget_destroy(d);
      if (r != GTK_RESPONSE_YES)
         return;
      size_t gone = 0;
      for (size_t id : sel) {
         GFile *f = g_file_new_for_path(fileEntries[id].path.c_str());
         GError *err = nullptr;
         if (g_file_trash(f, nullptr, &err)) {
            fileEntries[id].path.clear();
            removeFileRow(id);
            ++gone;
         }
         if (err)
            g_error_free(err);
         g_object_unref(f);
      }
      if (gone == sel.size())
         setStatus("Moved " + std::to_string(gone) + " file(s) to the Trash.");
      else
         setStatus("Could not delete " + std::to_string(sel.size() - gone) +
                   " file(s). They may be in use by your DAW, or the Trash may be unavailable.");
   }

   // Explorer-style: right-clicking an unselected row selects just that row.
   bool prepareMenu(GtkWidget *view, GdkEventButton *ev) {
      GtkTreePath *p = nullptr;
      if (!ev) {
         auto sel = selectedPaths(view);
         bool any = !sel.empty();
         freePaths(sel);
         return any;
      }
      if (!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(view), (int)ev->x, (int)ev->y, &p, nullptr, nullptr, nullptr))
         return false;
      GtkTreeSelection *s = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
      if (!gtk_tree_selection_path_is_selected(s, p)) {
         gtk_tree_selection_unselect_all(s);
         gtk_tree_selection_select_path(s, p);
         gtk_tree_view_set_cursor(GTK_TREE_VIEW(view), p, nullptr, FALSE);
      }
      gtk_tree_path_free(p);
      return true;
   }

   void popup(bool isFiles, GdkEventButton *ev) {
      GtkWidget *view = isFiles ? filesView : resultsView;
      if (!prepareMenu(view, ev))
         return;
      GtkWidget *menu = gtk_menu_new();
      auto add = [&](const char *label, void (*cb)(GtkMenuItem *, gpointer)) {
         GtkWidget *mi = gtk_menu_item_new_with_label(label);
         g_signal_connect(mi, "activate", G_CALLBACK(cb), this);
         gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
      };
      if (isFiles) {
         add("Open", [](GtkMenuItem *, gpointer p) { static_cast<Impl *>(p)->openSelectedFiles(); });
         add("Delete", [](GtkMenuItem *, gpointer p) { static_cast<Impl *>(p)->deleteSelectedFiles(); });
      } else {
         add(selectedHits().size() > 1 ? "Copy links" : "Copy link",
             [](GtkMenuItem *, gpointer p) { static_cast<Impl *>(p)->copySelectedLinks(); });
         add("Open in browser", [](GtkMenuItem *, gpointer p) { static_cast<Impl *>(p)->openSelectedInBrowser(); });
      }
      gtk_widget_show_all(menu);
      g_signal_connect(menu, "selection-done", G_CALLBACK(gtk_widget_destroy), nullptr);
      gtk_menu_popup_at_pointer(GTK_MENU(menu), ev ? reinterpret_cast<GdkEvent *>(ev) : nullptr);
   }

   // ---- events from workers ---------------------------------------------
   void drain() {
      std::deque<Event> local;
      {
         std::lock_guard<std::mutex> lk(qMutex);
         local.swap(queue);
      }
      for (auto &e : local) {
         switch (e.kind) {
         case Event::EvHit: {
            if (e.gen != gen)
               break;
            GtkTreeIter it;
            gtk_list_store_append(resultsStore, &it);
            gtk_list_store_set(resultsStore, &it, R_SITE, e.hit.site.c_str(), R_TITLE, e.hit.title.c_str(),
                               R_UPLOADER, e.hit.uploader.c_str(), R_LEN,
                               formatDuration(e.hit.durationSec).c_str(), R_IDX, (guint)hits.size(),
                               R_DUR, (gint)e.hit.durationSec, -1);
            hits.push_back(std::move(e.hit));
            break;
         }
         case Event::EvSearchDone:
            if (e.gen != gen)
               break;
            if (!e.text.empty())
               searchError += (searchError.empty() ? "" : "  |  ") + e.text;
            if (pendingSearches > 0 && --pendingSearches == 0) {
               if (hits.empty())
                  setStatus(searchError.empty() ? "No results." : searchError);
               else if (searchError.empty())
                  setStatus(std::to_string(hits.size()) + " results. Double-click one to download.");
               else
                  setStatus(std::to_string(hits.size()) + " results. Failed: " + searchError);
            }
            break;
         case Event::EvProgress: {
            gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress), std::min(1.0, e.pct / 100.0));
            std::string prefix = e.total > 1 ? std::to_string(e.index + 1) + "/" + std::to_string(e.total) + ": " : "";
            setStatus(e.pct >= 100.0 ? prefix + "Converting..." : prefix + "Downloading " + std::to_string((int)e.pct) + "%");
            break;
         }
         case Event::EvItemDone:
            if (e.ok) {
               gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress), 1.0);
               addFile(e.path);
               savedName = fs::path(e.path).filename().string();
            }
            break;
         case Event::EvDownloadDone:
            downloading = false;
            gtk_widget_set_sensitive(btnDownload, TRUE);
            if (e.ok) {
               setStatus(e.total == 1 ? "Saved: " + savedName + "  (drag it from the list below)"
                                      : "Saved " + std::to_string(e.index) + " files  (drag them from the list below)");
            } else {
               gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress), 0);
               std::string head = e.total > 1 ? "Saved " + std::to_string(e.index) + " of " + std::to_string(e.total) + ". " : "";
               setStatus(head + (e.text.empty() ? std::string("Download failed.") : e.text));
            }
            break;
         case Event::EvStatus:
            setStatus(e.text);
            break;
         }
      }
   }

   // ---- window -----------------------------------------------------------
   GtkTreeViewColumn *addTextColumn(GtkWidget *view, const char *title, int modelCol, int width, bool expand,
                                    std::vector<GtkTreeViewColumn *> &cols, bool isFiles, int sortIdx,
                                    bool rightAlign = false) {
      GtkCellRenderer *r = gtk_cell_renderer_text_new();
      g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
      if (rightAlign)
         g_object_set(r, "xalign", 1.0f, nullptr);
      GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes(title, r, "text", modelCol, nullptr);
      gtk_tree_view_column_set_sizing(c, GTK_TREE_VIEW_COLUMN_FIXED);
      gtk_tree_view_column_set_fixed_width(c, width);
      gtk_tree_view_column_set_resizable(c, TRUE);
      gtk_tree_view_column_set_clickable(c, TRUE);
      (void)expand;
      struct Ctx {
         Impl *d;
         bool files;
         int idx;
      };
      auto *ctx = new Ctx{this, isFiles, sortIdx};
      g_signal_connect_data(
          c, "clicked",
          G_CALLBACK(+[](GtkTreeViewColumn *, gpointer p) {
             auto *x = static_cast<Ctx *>(p);
             x->d->onColumnClick(x->files, x->idx);
          }),
          ctx, [](gpointer p, GClosure *) { delete static_cast<Ctx *>(p); }, (GConnectFlags)0);
      gtk_tree_view_append_column(GTK_TREE_VIEW(view), c);
      cols.push_back(c);
      return c;
   }

   GtkWidget *scrolled(GtkWidget *child, int minH) {
      GtkWidget *sw = gtk_scrolled_window_new(nullptr, nullptr);
      gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
      gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw), GTK_SHADOW_IN);
      gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(sw), minH);
      gtk_container_add(GTK_CONTAINER(sw), child);
      gtk_widget_set_vexpand(sw, TRUE);
      return sw;
   }

   void wireView(GtkWidget *view, bool isFiles) {
      gtk_tree_view_set_enable_search(GTK_TREE_VIEW(view), FALSE);
      gtk_tree_view_set_rubber_banding(GTK_TREE_VIEW(view), TRUE);
      gtk_tree_selection_set_mode(gtk_tree_view_get_selection(GTK_TREE_VIEW(view)), GTK_SELECTION_MULTIPLE);
      g_object_set_data(G_OBJECT(view), "ytb-files", GINT_TO_POINTER(isFiles ? 1 : 0));

      g_signal_connect(view, "button-press-event",
                       G_CALLBACK(+[](GtkWidget *w, GdkEventButton *ev, gpointer p) -> gboolean {
                          if (ev->type == GDK_BUTTON_PRESS && ev->button == 3) {
                             bool files = g_object_get_data(G_OBJECT(w), "ytb-files") != nullptr;
                             static_cast<Impl *>(p)->popup(files, ev);
                             return TRUE;
                          }
                          return FALSE;
                       }),
                       this);
      g_signal_connect(view, "popup-menu", G_CALLBACK(+[](GtkWidget *w, gpointer p) -> gboolean {
                          bool files = g_object_get_data(G_OBJECT(w), "ytb-files") != nullptr;
                          static_cast<Impl *>(p)->popup(files, nullptr);
                          return TRUE;
                       }),
                       this);
      g_signal_connect(view, "row-activated",
                       G_CALLBACK(+[](GtkTreeView *w, GtkTreePath *, GtkTreeViewColumn *, gpointer p) {
                          auto *d = static_cast<Impl *>(p);
                          if (g_object_get_data(G_OBJECT(w), "ytb-files"))
                             d->openSelectedFiles();
                          else
                             d->doDownload();
                       }),
                       this);
      g_signal_connect(view, "key-press-event",
                       G_CALLBACK(+[](GtkWidget *w, GdkEventKey *ev, gpointer p) -> gboolean {
                          auto *d = static_cast<Impl *>(p);
                          const bool files = g_object_get_data(G_OBJECT(w), "ytb-files") != nullptr;
                          const bool ctrl = (ev->state & GDK_CONTROL_MASK) != 0;
                          if (ev->keyval == GDK_KEY_Delete && files) {
                             d->deleteSelectedFiles();
                             return TRUE;
                          }
                          if (ctrl && (ev->keyval == GDK_KEY_c || ev->keyval == GDK_KEY_C) && !files) {
                             d->copySelectedLinks();
                             return TRUE;
                          }
                          return FALSE;
                       }),
                       this);
   }

   bool build() {
      if (!gtk_init_check(nullptr, nullptr))
         return false;

      win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
      gtk_window_set_title(GTK_WINDOW(win), "YT Audio Browser");
      gtk_window_set_default_size(GTK_WINDOW(win), 800, 680);
      gtk_window_set_type_hint(GTK_WINDOW(win), GDK_WINDOW_TYPE_HINT_NORMAL);
      g_signal_connect(win, "delete-event", G_CALLBACK(+[](GtkWidget *w, GdkEvent *, gpointer p) -> gboolean {
                          gtk_widget_hide(w);
                          static_cast<Impl *>(p)->saveSettings();
                          if (static_cast<Impl *>(p)->onClosed)
                             static_cast<Impl *>(p)->onClosed();
                          return TRUE;
                       }),
                       this);

      GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
      gtk_container_set_border_width(GTK_CONTAINER(root), 10);
      gtk_container_add(GTK_CONTAINER(win), root);

      // search row
      GtkWidget *row1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
      query = gtk_entry_new();
      gtk_entry_set_placeholder_text(GTK_ENTRY(query), "Search, or paste any URL yt-dlp supports");
      gtk_widget_set_hexpand(query, TRUE);
      btnSearch = gtk_button_new_with_label("Search");
      btnStop = gtk_button_new_with_label("Stop");
      btnRefresh = gtk_button_new_with_label("Refresh");
      gtk_box_pack_start(GTK_BOX(row1), query, TRUE, TRUE, 0);
      gtk_box_pack_start(GTK_BOX(row1), btnSearch, FALSE, FALSE, 0);
      gtk_box_pack_start(GTK_BOX(row1), btnStop, FALSE, FALSE, 0);
      gtk_box_pack_start(GTK_BOX(row1), btnRefresh, FALSE, FALSE, 0);
      gtk_box_pack_start(GTK_BOX(root), row1, FALSE, FALSE, 0);
      g_signal_connect(query, "activate", G_CALLBACK(+[](GtkWidget *, gpointer p) { static_cast<Impl *>(p)->doSearch(); }), this);
      g_signal_connect(btnSearch, "clicked", G_CALLBACK(+[](GtkWidget *, gpointer p) { static_cast<Impl *>(p)->doSearch(); }), this);
      g_signal_connect(btnStop, "clicked", G_CALLBACK(+[](GtkWidget *, gpointer p) { static_cast<Impl *>(p)->doStop(); }), this);
      g_signal_connect(btnRefresh, "clicked", G_CALLBACK(+[](GtkWidget *, gpointer p) { static_cast<Impl *>(p)->doRefresh(); }), this);

      // sites + format
      GtkWidget *row2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
      GtkWidget *flow = gtk_flow_box_new();
      gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow), GTK_SELECTION_NONE);
      gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flow), 9);
      gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(flow), FALSE);
      gtk_widget_set_hexpand(flow, TRUE);
      for (const auto &s : sites()) {
         GtkWidget *c = gtk_check_button_new_with_label(s.label);
         gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(c), s.defaultOn);
         g_signal_connect(c, "toggled", G_CALLBACK(+[](GtkWidget *, gpointer p) { static_cast<Impl *>(p)->saveSettings(); }), this);
         gtk_container_add(GTK_CONTAINER(flow), c);
         siteChecks.push_back(c);
      }
      gtk_box_pack_start(GTK_BOX(row2), flow, TRUE, TRUE, 0);
      GtkWidget *fbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
      gtk_box_pack_start(GTK_BOX(fbox), gtk_label_new("Format:"), FALSE, FALSE, 0);
      fmtCombo = gtk_combo_box_text_new();
      for (const char *f : {"flac", "wav", "mp3", "m4a", "opus"})
         gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(fmtCombo), f);
      gtk_combo_box_set_active(GTK_COMBO_BOX(fmtCombo), 0);
      g_signal_connect(fmtCombo, "changed", G_CALLBACK(+[](GtkWidget *, gpointer p) { static_cast<Impl *>(p)->saveSettings(); }), this);
      gtk_box_pack_start(GTK_BOX(fbox), fmtCombo, FALSE, FALSE, 0);
      gtk_box_pack_end(GTK_BOX(row2), fbox, FALSE, FALSE, 0);
      gtk_box_pack_start(GTK_BOX(root), row2, FALSE, FALSE, 0);

      // results
      resultsStore = gtk_list_store_new(R_N, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                        G_TYPE_UINT, G_TYPE_INT);
      for (int c : std::initializer_list<int>{R_SITE, R_TITLE, R_UPLOADER, R_LEN, kDefaultSort})
         gtk_tree_sortable_set_sort_func(GTK_TREE_SORTABLE(resultsStore), c, &Impl::resultsCompare,
                                         GINT_TO_POINTER(c), nullptr);
      resultsView = gtk_tree_view_new_with_model(GTK_TREE_MODEL(resultsStore));
      addTextColumn(resultsView, "Site", R_SITE, 95, false, resCols, false, 0);
      addTextColumn(resultsView, "Title", R_TITLE, 400, true, resCols, false, 1);
      addTextColumn(resultsView, "Uploader", R_UPLOADER, 160, false, resCols, false, 2);
      addTextColumn(resultsView, "Length", R_LEN, 70, false, resCols, false, 3, true);
      wireView(resultsView, false);
      gtk_box_pack_start(GTK_BOX(root), scrolled(resultsView, 180), TRUE, TRUE, 0);

      // action row
      GtkWidget *row3 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
      btnDownload = gtk_button_new_with_label("Download selected");
      GtkWidget *btnUpdate = gtk_button_new_with_label("Update yt-dlp");
      GtkWidget *btnOpen = gtk_button_new_with_label("Open folder");
      progress = gtk_progress_bar_new();
      gtk_widget_set_hexpand(progress, TRUE);
      gtk_widget_set_valign(progress, GTK_ALIGN_CENTER);
      gtk_box_pack_start(GTK_BOX(row3), btnDownload, FALSE, FALSE, 0);
      gtk_box_pack_start(GTK_BOX(row3), btnUpdate, FALSE, FALSE, 0);
      gtk_box_pack_start(GTK_BOX(row3), btnOpen, FALSE, FALSE, 0);
      gtk_box_pack_start(GTK_BOX(row3), progress, TRUE, TRUE, 0);
      gtk_box_pack_start(GTK_BOX(root), row3, FALSE, FALSE, 0);
      g_signal_connect(btnDownload, "clicked", G_CALLBACK(+[](GtkWidget *, gpointer p) { static_cast<Impl *>(p)->doDownload(); }), this);
      g_signal_connect(btnUpdate, "clicked", G_CALLBACK(+[](GtkWidget *, gpointer p) { static_cast<Impl *>(p)->doUpdate(); }), this);
      g_signal_connect(btnOpen, "clicked", G_CALLBACK(+[](GtkWidget *, gpointer p) { static_cast<Impl *>(p)->openFolder(); }), this);

      status = gtk_label_new("Ready. Double-click a result to download it.");
      gtk_label_set_xalign(GTK_LABEL(status), 0);
      gtk_label_set_ellipsize(GTK_LABEL(status), PANGO_ELLIPSIZE_END);
      gtk_label_set_selectable(GTK_LABEL(status), TRUE);
      gtk_box_pack_start(GTK_BOX(root), status, FALSE, FALSE, 0);

      // downloads
      GtkWidget *dl = gtk_label_new("Downloads (drag a row into your DAW):");
      gtk_label_set_xalign(GTK_LABEL(dl), 0);
      gtk_box_pack_start(GTK_BOX(root), dl, FALSE, FALSE, 0);
      filesStore = gtk_list_store_new(F_N, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_UINT,
                                      G_TYPE_INT64, G_TYPE_INT64);
      for (int c : std::initializer_list<int>{F_NAME, F_MOD, F_SIZE, kDefaultSort})
         gtk_tree_sortable_set_sort_func(GTK_TREE_SORTABLE(filesStore), c, &Impl::filesCompare,
                                         GINT_TO_POINTER(c == F_MOD ? F_MTIME : c), nullptr);
      filesView = gtk_tree_view_new_with_model(GTK_TREE_MODEL(filesStore));
      addTextColumn(filesView, "Name", F_NAME, 400, true, fileCols, true, 0);
      addTextColumn(filesView, "Modified", F_MOD, 150, false, fileCols, true, 1);
      addTextColumn(filesView, "Size", F_SIZE, 90, false, fileCols, true, 2, true);
      wireView(filesView, true);

      // Dragging rows out: offer the selected files as a text/uri-list.
      static const GtkTargetEntry kTargets[] = {{(gchar *)"text/uri-list", 0, 0}};
      gtk_tree_view_enable_model_drag_source(GTK_TREE_VIEW(filesView), GDK_BUTTON1_MASK, kTargets, 1,
                                             GDK_ACTION_COPY);
      g_signal_connect(filesView, "drag-data-get",
                       G_CALLBACK(+[](GtkWidget *, GdkDragContext *, GtkSelectionData *sd, guint, guint,
                                      gpointer p) {
                          auto *d = static_cast<Impl *>(p);
                          std::vector<std::string> uris;
                          for (size_t id : d->selectedFiles())
                             uris.push_back(fileUri(d->fileEntries[id].path));
                          if (uris.empty())
                             return;
                          std::vector<const gchar *> v;
                          for (auto &u : uris)
                             v.push_back(u.c_str());
                          v.push_back(nullptr);
                          gtk_selection_data_set_uris(sd, const_cast<gchar **>(v.data()));
                       }),
                       this);
      gtk_box_pack_start(GTK_BOX(root), scrolled(filesView, 130), TRUE, TRUE, 0);

      loadSettings();
      applySort(false);
      applySort(true);
      loadExistingFiles();

      Tools t;
      PathStr why;
      if (!locateTools(t, why))
         setStatus(why);

      gtk_widget_show_all(root); // children visible; the window itself stays hidden until show()
      return true;
   }
};

GtkGui::GtkGui(std::function<void()> onClosed) : d(new Impl) { d->onClosed = std::move(onClosed); }

GtkGui::~GtkGui() {
   if (d->win) {
      d->saveSettings();
      d->stopWorkers();
      if (d->idleId)
         g_source_remove(d->idleId);
      // Drop idle callbacks that were queued for this object.
      while (g_idle_remove_by_data(d.get())) {
      }
      gtk_widget_destroy(d->win);
      d->win = nullptr;
      while (gtk_events_pending())
         gtk_main_iteration_do(FALSE);
   }
}

bool GtkGui::create() { return d->win ? true : d->build(); }

bool GtkGui::setTransient(unsigned long x11Window) {
   if (!d->win)
      return false;
   GdkDisplay *disp = gtk_widget_get_display(d->win);
   if (!GDK_IS_X11_DISPLAY(disp))
      return false;
   gtk_widget_realize(d->win);
   GdkWindow *foreign = gdk_x11_window_foreign_new_for_display(disp, (Window)x11Window);
   if (!foreign)
      return false;
   gdk_window_set_transient_for(gtk_widget_get_window(d->win), foreign);
   g_object_unref(foreign);
   return true;
}

void GtkGui::setTitle(const char *title) {
   if (d->win && title)
      gtk_window_set_title(GTK_WINDOW(d->win), title);
}

void GtkGui::show() {
   if (!d->win)
      return;
   gtk_window_present(GTK_WINDOW(d->win));
}

void GtkGui::hide() {
   if (!d->win)
      return;
   d->saveSettings();
   gtk_widget_hide(d->win);
}

void GtkGui::pump() {
   for (int i = 0; i < 50 && gtk_events_pending(); ++i)
      gtk_main_iteration_do(FALSE);
}

} // namespace ytb
