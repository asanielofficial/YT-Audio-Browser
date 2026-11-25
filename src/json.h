#pragma once

// Small JSON reader for search API responses. No dependencies, no Windows headers.

#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace ytb {

struct Json {
   enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
   bool b = false;
   double n = 0;
   std::string s;
   std::vector<Json> a;
   std::vector<std::pair<std::string, Json>> o;

   const Json *get(const char *key) const {
      if (type != Obj)
         return nullptr;
      for (const auto &kv : o)
         if (kv.first == key)
            return &kv.second;
      return nullptr;
   }
   // Text of a string member; "" when missing or not a string. A first string
   // inside an array also counts (Internet Archive "creator" may be either).
   std::string str(const char *key) const {
      const Json *v = get(key);
      if (!v)
         return {};
      if (v->type == Str)
         return v->s;
      if (v->type == Arr && !v->a.empty() && v->a[0].type == Str)
         return v->a[0].s;
      return {};
   }
   double num(const char *key, double def = 0) const {
      const Json *v = get(key);
      if (!v)
         return def;
      if (v->type == Num)
         return v->n;
      if (v->type == Str)
         return std::atof(v->s.c_str());
      return def;
   }
};

class JsonParser {
 public:
   static bool parse(const std::string &text, Json &out) {
      JsonParser p(text);
      p.ws();
      if (!p.value(out, 0))
         return false;
      p.ws();
      return p.i == p.t.size();
   }

 private:
   explicit JsonParser(const std::string &text) : t(text) {}
   const std::string &t;
   size_t i = 0;

   void ws() {
      while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\n' || t[i] == '\r'))
         ++i;
   }
   bool lit(const char *w) {
      size_t n = 0;
      while (w[n])
         ++n;
      if (t.compare(i, n, w) != 0)
         return false;
      i += n;
      return true;
   }
   static void utf8(std::string &o, unsigned cp) {
      if (cp < 0x80) {
         o += (char)cp;
      } else if (cp < 0x800) {
         o += (char)(0xC0 | (cp >> 6));
         o += (char)(0x80 | (cp & 0x3F));
      } else if (cp < 0x10000) {
         o += (char)(0xE0 | (cp >> 12));
         o += (char)(0x80 | ((cp >> 6) & 0x3F));
         o += (char)(0x80 | (cp & 0x3F));
      } else {
         o += (char)(0xF0 | (cp >> 18));
         o += (char)(0x80 | ((cp >> 12) & 0x3F));
         o += (char)(0x80 | ((cp >> 6) & 0x3F));
         o += (char)(0x80 | (cp & 0x3F));
      }
   }
   bool hex4(unsigned &v) {
      if (i + 4 > t.size())
         return false;
      v = 0;
      for (int k = 0; k < 4; ++k) {
         char c = t[i++];
         v <<= 4;
         if (c >= '0' && c <= '9')
            v |= (unsigned)(c - '0');
         else if (c >= 'a' && c <= 'f')
            v |= (unsigned)(c - 'a' + 10);
         else if (c >= 'A' && c <= 'F')
            v |= (unsigned)(c - 'A' + 10);
         else
            return false;
      }
      return true;
   }
   bool string(std::string &o) {
      if (i >= t.size() || t[i] != '"')
         return false;
      ++i;
      while (i < t.size()) {
         char c = t[i++];
         if (c == '"')
            return true;
         if (c != '\\') {
            o += c;
            continue;
         }
         if (i >= t.size())
            return false;
         char e = t[i++];
         switch (e) {
         case '"': o += '"'; break;
         case '\\': o += '\\'; break;
         case '/': o += '/'; break;
         case 'b': o += '\b'; break;
         case 'f': o += '\f'; break;
         case 'n': o += '\n'; break;
         case 'r': o += '\r'; break;
         case 't': o += '\t'; break;
         case 'u': {
            unsigned cp;
            if (!hex4(cp))
               return false;
            if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < t.size() && t[i] == '\\' && t[i + 1] == 'u') {
               i += 2;
               unsigned lo;
               if (!hex4(lo))
                  return false;
               cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            }
            utf8(o, cp);
            break;
         }
         default: return false;
         }
      }
      return false;
   }
   bool value(Json &v, int depth) {
      if (depth > 64 || i >= t.size())
         return false;
      char c = t[i];
      if (c == '{') {
         ++i;
         v.type = Json::Obj;
         ws();
         if (i < t.size() && t[i] == '}') {
            ++i;
            return true;
         }
         for (;;) {
            ws();
            std::string k;
            if (!string(k))
               return false;
            ws();
            if (i >= t.size() || t[i++] != ':')
               return false;
            ws();
            Json child;
            if (!value(child, depth + 1))
               return false;
            v.o.emplace_back(std::move(k), std::move(child));
            ws();
            if (i >= t.size())
               return false;
            if (t[i] == ',') {
               ++i;
               continue;
            }
            if (t[i] == '}') {
               ++i;
               return true;
            }
            return false;
         }
      }
      if (c == '[') {
         ++i;
         v.type = Json::Arr;
         ws();
         if (i < t.size() && t[i] == ']') {
            ++i;
            return true;
         }
         for (;;) {
            ws();
            Json child;
            if (!value(child, depth + 1))
               return false;
            v.a.push_back(std::move(child));
            ws();
            if (i >= t.size())
               return false;
            if (t[i] == ',') {
               ++i;
               continue;
            }
            if (t[i] == ']') {
               ++i;
               return true;
            }
            return false;
         }
      }
      if (c == '"') {
         v.type = Json::Str;
         return string(v.s);
      }
      if (lit("true")) {
         v.type = Json::Bool;
         v.b = true;
         return true;
      }
      if (lit("false")) {
         v.type = Json::Bool;
         return true;
      }
      if (lit("null")) {
         v.type = Json::Null;
         return true;
      }
      size_t start = i;
      while (i < t.size() && (t[i] == '-' || t[i] == '+' || t[i] == '.' || t[i] == 'e' ||
                              t[i] == 'E' || (t[i] >= '0' && t[i] <= '9')))
         ++i;
      if (i == start)
         return false;
      v.type = Json::Num;
      v.n = std::atof(t.substr(start, i - start).c_str());
      return true;
   }
};

} // namespace ytb
