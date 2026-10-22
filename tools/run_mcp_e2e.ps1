# End-to-end test of the RE+ MCP server. Reloaded.Editor.Mcp.exe serves MCP on
# stdio, driven by the JSON-RPC steps of tests/mcp/e2e/*.jsonl, against a real
# editor started through the launcher in a disposable copy of the game (made
# as run_native_map_recovery_test.ps1 makes its own). Each step's response is
# checked against its expectations; see tests/McpBridgeNativeNotes.md.
#   -Native        runs the native probe's MCP bridge section instead
#                  (tests/WorkflowNativeTests.h, [test] mcp_bridge_only=1), in
#                  run_native_map_recovery_test.ps1's disposable install.
#   -CheckScripts  only reads the step files and tests the expectation checker;
#                  it needs no game and runs on any PowerShell 5.1 or later.
param(
    [string]$EditorDll,
    [string]$McpExe,
    [string]$Launcher,
    [string]$GameSystem = $env:SCCT,
    [string[]]$Script = @(),
    [ValidatePattern('^[A-Za-z_][A-Za-z0-9_]*$')][string]$MapFile = 'VersD',
    [int]$StartSeconds = 300,
    [int]$TimeoutSeconds = 7200,
    [switch]$PrepareOnly,
    [switch]$KeepEditor,
    [switch]$Native,
    [switch]$VisibleEditor,
    [switch]$CheckScripts
)
$ErrorActionPreference = 'Stop'
$repository = Split-Path -Parent $PSScriptRoot

# JSON, paths into it and the expectation checker. C# 5, which Windows
# PowerShell's Add-Type compiles; PowerShell's own JSON cmdlets differ between
# versions (7 turns "2025-06-18" into a date; 5.1 refuses keys that differ only
# in case), so neither reads the server's replies.
$checkerSource = @'
using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.Text;
using System.Text.RegularExpressions;

namespace McpE2e
{
    public static class Json
    {
        // ---------------------------------------------------------- parsing
        public static object Parse(string text)
        {
            int at = 0;
            object value = Value(text, ref at);
            Space(text, ref at);
            if (at != text.Length) throw Error(at, "text after the value");
            return value;
        }
        static FormatException Error(int at, string what) { return new FormatException("Not JSON: " + what + " at character " + at + "."); }
        static void Space(string s, ref int at) { while (at < s.Length && (s[at] == ' ' || s[at] == '\t' || s[at] == '\r' || s[at] == '\n')) ++at; }
        static bool Digit(string s, int at) { return at < s.Length && s[at] >= '0' && s[at] <= '9'; }
        static object Value(string s, ref int at)
        {
            Space(s, ref at);
            if (at >= s.Length) throw Error(at, "a value is missing");
            char c = s[at];
            if (c == '{')
            {
                Dictionary<string, object> map = new Dictionary<string, object>(StringComparer.Ordinal);
                ++at; Space(s, ref at);
                if (at < s.Length && s[at] == '}') { ++at; return map; }
                for (;;)
                {
                    Space(s, ref at);
                    if (at >= s.Length || s[at] != '"') throw Error(at, "a key is expected");
                    string key = Text(s, ref at);
                    Space(s, ref at);
                    if (at >= s.Length || s[at] != ':') throw Error(at, "':' is expected");
                    ++at;
                    map[key] = Value(s, ref at);
                    Space(s, ref at);
                    if (at < s.Length && s[at] == ',') { ++at; continue; }
                    if (at < s.Length && s[at] == '}') { ++at; return map; }
                    throw Error(at, "',' or '}' is expected");
                }
            }
            if (c == '[')
            {
                List<object> list = new List<object>();
                ++at; Space(s, ref at);
                if (at < s.Length && s[at] == ']') { ++at; return list; }
                for (;;)
                {
                    list.Add(Value(s, ref at));
                    Space(s, ref at);
                    if (at < s.Length && s[at] == ',') { ++at; continue; }
                    if (at < s.Length && s[at] == ']') { ++at; return list; }
                    throw Error(at, "',' or ']' is expected");
                }
            }
            if (c == '"') return Text(s, ref at);
            if (Word(s, ref at, "true")) return true;
            if (Word(s, ref at, "false")) return false;
            if (Word(s, ref at, "null")) return null;
            return Number(s, ref at);
        }
        static bool Word(string s, ref int at, string word)
        {
            if (at + word.Length > s.Length || string.CompareOrdinal(s, at, word, 0, word.Length) != 0) return false;
            at += word.Length;
            return true;
        }
        static object Number(string s, ref int at)
        {
            int start = at;
            bool integral = true;
            if (at < s.Length && s[at] == '-') ++at;
            while (Digit(s, at)) ++at;
            if (at < s.Length && s[at] == '.') { integral = false; ++at; while (Digit(s, at)) ++at; }
            if (at < s.Length && (s[at] == 'e' || s[at] == 'E'))
            {
                integral = false; ++at;
                if (at < s.Length && (s[at] == '+' || s[at] == '-')) ++at;
                while (Digit(s, at)) ++at;
            }
            string text = s.Substring(start, at - start);
            if (text.Length == 0 || text == "-") throw Error(start, "a value is expected");
            if (integral)
            {
                long whole;
                if (long.TryParse(text, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out whole)) return whole;
                ulong large;
                if (ulong.TryParse(text, NumberStyles.None, CultureInfo.InvariantCulture, out large)) return large;
            }
            double real;
            if (double.TryParse(text, NumberStyles.Float, CultureInfo.InvariantCulture, out real)) return real;
            throw Error(start, "a number is malformed");
        }
        static string Text(string s, ref int at)
        {
            ++at;
            StringBuilder b = new StringBuilder();
            while (at < s.Length)
            {
                char c = s[at++];
                if (c == '"') return b.ToString();
                if (c != '\\') { b.Append(c); continue; }
                if (at >= s.Length) break;
                char e = s[at++];
                switch (e)
                {
                    case '"': b.Append('"'); break;
                    case '\\': b.Append('\\'); break;
                    case '/': b.Append('/'); break;
                    case 'b': b.Append('\b'); break;
                    case 'f': b.Append('\f'); break;
                    case 'n': b.Append('\n'); break;
                    case 'r': b.Append('\r'); break;
                    case 't': b.Append('\t'); break;
                    case 'u':
                        if (at + 4 > s.Length) throw Error(at, "a \\u escape is cut short");
                        b.Append((char)int.Parse(s.Substring(at, 4), NumberStyles.HexNumber, CultureInfo.InvariantCulture));
                        at += 4;
                        break;
                    default: throw Error(at - 1, "an unknown escape");
                }
            }
            throw Error(at, "a string is not closed");
        }

        // ---------------------------------------------------------- writing
        public static string Write(object value)
        {
            StringBuilder b = new StringBuilder();
            Write(b, value);
            return b.ToString();
        }
        static void Write(StringBuilder b, object v)
        {
            v = Unwrap(v);
            if (v == null) { b.Append("null"); return; }
            if (v is bool) { b.Append((bool)v ? "true" : "false"); return; }
            string s = v as string;
            if (s != null) { Quote(b, s); return; }
            if (v is double || v is float || v is decimal) { b.Append(Convert.ToDouble(v, CultureInfo.InvariantCulture).ToString("R", CultureInfo.InvariantCulture)); return; }
            if (IsNumber(v)) { b.Append(Convert.ToString(v, CultureInfo.InvariantCulture)); return; }
            IDictionary<string, object> map = AsMap(v);
            if (map != null)
            {
                b.Append('{');
                bool first = true;
                foreach (KeyValuePair<string, object> p in map)
                {
                    if (!first) b.Append(',');
                    first = false;
                    Quote(b, p.Key);
                    b.Append(':');
                    Write(b, p.Value);
                }
                b.Append('}');
                return;
            }
            IList<object> list = AsList(v);
            if (list != null)
            {
                b.Append('[');
                for (int i = 0; i < list.Count; ++i) { if (i > 0) b.Append(','); Write(b, list[i]); }
                b.Append(']');
                return;
            }
            Quote(b, Convert.ToString(v, CultureInfo.InvariantCulture));
        }
        static void Quote(StringBuilder b, string s)
        {
            b.Append('"');
            foreach (char c in s)
            {
                if (c == '"') b.Append("\\\"");
                else if (c == '\\') b.Append("\\\\");
                else if (c == '\n') b.Append("\\n");
                else if (c == '\r') b.Append("\\r");
                else if (c == '\t') b.Append("\\t");
                else if (c < 0x20) b.Append("\\u").Append(((int)c).ToString("x4", CultureInfo.InvariantCulture));
                else b.Append(c);
            }
            b.Append('"');
        }

        // ------------------------------------------------------------ values
        // PowerShell hands over its own wrappers, hashtables and arrays.
        public static object Unwrap(object v)
        {
            if (v != null && v.GetType().FullName == "System.Management.Automation.PSObject")
                v = v.GetType().GetProperty("BaseObject").GetValue(v, null);
            return v;
        }
        static bool IsNumber(object v)
        {
            return v is int || v is long || v is uint || v is ulong || v is short || v is ushort || v is byte || v is sbyte || v is double || v is float || v is decimal;
        }
        static IDictionary<string, object> AsMap(object v)
        {
            v = Unwrap(v);
            IDictionary<string, object> map = v as IDictionary<string, object>;
            if (map != null) return map;
            IDictionary any = v as IDictionary;
            if (any == null) return null;
            Dictionary<string, object> copy = new Dictionary<string, object>(StringComparer.Ordinal);
            foreach (DictionaryEntry e in any) copy[Convert.ToString(e.Key, CultureInfo.InvariantCulture)] = e.Value;
            return copy;
        }
        static IList<object> AsList(object v)
        {
            v = Unwrap(v);
            if (v == null || v is string || AsMap(v) != null) return null;
            IList<object> list = v as IList<object>;
            if (list != null) return list;
            IEnumerable items = v as IEnumerable;
            if (items == null) return null;
            List<object> copy = new List<object>();
            foreach (object item in items) copy.Add(item);
            return copy;
        }
        public static string Kind(object v)
        {
            v = Unwrap(v);
            if (v == null) return "null";
            if (v is bool) return "boolean";
            if (v is string) return "string";
            if (IsNumber(v)) return "number";
            if (AsMap(v) != null) return "object";
            if (AsList(v) != null) return "array";
            return "string";
        }
        // A value as text: strings as they are, the rest as JSON.
        public static string Plain(object v)
        {
            v = Unwrap(v);
            string s = v as string;
            return s != null ? s : Write(v);
        }
        public static string Short(object v)
        {
            string text = Write(v);
            return text.Length <= 240 ? text : text.Substring(0, 240) + "... (" + text.Length + " characters)";
        }
        // A copy with long strings cut, for the transcript.
        public static object Strip(object v, int max)
        {
            v = Unwrap(v);
            string s = v as string;
            if (s != null) return s.Length <= max ? s : s.Substring(0, max) + "... (" + s.Length + " characters)";
            IDictionary<string, object> map = AsMap(v);
            if (map != null)
            {
                Dictionary<string, object> copy = new Dictionary<string, object>(StringComparer.Ordinal);
                foreach (KeyValuePair<string, object> p in map) copy[p.Key] = Strip(p.Value, max);
                return copy;
            }
            IList<object> list = AsList(v);
            if (list != null)
            {
                List<object> copy = new List<object>();
                foreach (object item in list) copy.Add(Strip(item, max));
                return copy;
            }
            return v;
        }
        static int CompareNumbers(object a, object b)
        {
            if (a is double || b is double || a is float || b is float)
                return Convert.ToDouble(a, CultureInfo.InvariantCulture).CompareTo(Convert.ToDouble(b, CultureInfo.InvariantCulture));
            return Convert.ToDecimal(a, CultureInfo.InvariantCulture).CompareTo(Convert.ToDecimal(b, CultureInfo.InvariantCulture));
        }
        public static bool Same(object a, object b)
        {
            a = Unwrap(a); b = Unwrap(b);
            if (a == null || b == null) return a == null && b == null;
            if (IsNumber(a) && IsNumber(b)) return CompareNumbers(a, b) == 0;
            if (a is bool && b is bool) return (bool)a == (bool)b;
            string sa = a as string, sb = b as string;
            if (sa != null || sb != null) return sa != null && sb != null && string.Equals(sa, sb, StringComparison.Ordinal);
            IDictionary<string, object> ma = AsMap(a), mb = AsMap(b);
            if (ma != null || mb != null)
            {
                if (ma == null || mb == null || ma.Count != mb.Count) return false;
                foreach (KeyValuePair<string, object> p in ma)
                {
                    object other;
                    if (!mb.TryGetValue(p.Key, out other) || !Same(p.Value, other)) return false;
                }
                return true;
            }
            IList<object> la = AsList(a), lb = AsList(b);
            if (la == null || lb == null || la.Count != lb.Count) return false;
            for (int i = 0; i < la.Count; ++i) if (!Same(la[i], lb[i])) return false;
            return true;
        }

        // ---------------------------------------------------- ${name} values
        static readonly Regex Variable = new Regex(@"\$\{([A-Za-z_][A-Za-z0-9_]*)\}");
        static object Lookup(IDictionary<string, object> values, string name)
        {
            object v;
            if (values == null || !values.TryGetValue(name, out v)) throw new KeyNotFoundException("The step names ${" + name + "}, which nothing has set: the runner sets the game's paths and EditorPid, and a step's save sets the rest.");
            return Unwrap(v);
        }
        // A string that is one ${name} becomes that value, of its own type;
        // ${name} inside a longer string becomes its text.
        public static object Substitute(object value, IDictionary<string, object> values)
        {
            value = Unwrap(value);
            string s = value as string;
            if (s != null)
            {
                Match whole = Variable.Match(s);
                if (whole.Success && whole.Index == 0 && whole.Length == s.Length) return Lookup(values, whole.Groups[1].Value);
                return Variable.Replace(s, delegate(Match m) { return Plain(Lookup(values, m.Groups[1].Value)); });
            }
            IDictionary<string, object> map = AsMap(value);
            if (map != null)
            {
                Dictionary<string, object> copy = new Dictionary<string, object>(StringComparer.Ordinal);
                foreach (KeyValuePair<string, object> p in map) copy[p.Key] = Substitute(p.Value, values);
                return copy;
            }
            IList<object> list = AsList(value);
            if (list != null)
            {
                List<object> copy = new List<object>();
                foreach (object item in list) copy.Add(Substitute(item, values));
                return copy;
            }
            return value;
        }

        // ------------------------------------------------------------- paths
        // a.b[0].c, list[*] (every item), list[?key=value] (the items whose key
        // has that value), map['key.with.dots'], [-1] (the last); a leading $ is
        // ignored. A path with [*] or [?...] selects many values.
        class Step { public string Key; public int Index; public bool All; public string FilterKey, FilterValue; }
        static List<Step> Steps(string path)
        {
            List<Step> steps = new List<Step>();
            int at = 0;
            if (path.StartsWith("$")) at = 1;
            while (at < path.Length)
            {
                char c = path[at];
                if (c == '.') { ++at; continue; }
                if (c == '[')
                {
                    if (at + 1 < path.Length && (path[at + 1] == '\'' || path[at + 1] == '"'))
                    {
                        char quote = path[at + 1];
                        int closing = path.IndexOf(quote, at + 2);
                        if (closing < 0 || closing + 1 >= path.Length || path[closing + 1] != ']') throw new FormatException("Path " + path + ": a quoted key is not closed.");
                        Step keyed = new Step(); keyed.Key = path.Substring(at + 2, closing - at - 2);
                        steps.Add(keyed);
                        at = closing + 2;
                        continue;
                    }
                    int close = path.IndexOf(']', at);
                    if (close < 0) throw new FormatException("Path " + path + ": '[' is not closed.");
                    string inside = path.Substring(at + 1, close - at - 1);
                    Step step = new Step();
                    if (inside == "*") step.All = true;
                    else if (inside.StartsWith("?"))
                    {
                        int equals = inside.IndexOf('=');
                        if (equals < 2) throw new FormatException("Path " + path + ": a filter is [?key=value].");
                        step.FilterKey = inside.Substring(1, equals - 1);
                        step.FilterValue = inside.Substring(equals + 1);
                    }
                    else if (!int.TryParse(inside, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out step.Index))
                        throw new FormatException("Path " + path + ": [" + inside + "] is not an index, [*] or [?key=value].");
                    steps.Add(step);
                    at = close + 1;
                    continue;
                }
                int end = at;
                while (end < path.Length && path[end] != '.' && path[end] != '[') ++end;
                Step named = new Step(); named.Key = path.Substring(at, end - at);
                steps.Add(named);
                at = end;
            }
            return steps;
        }
        public static bool IsMany(string path)
        {
            foreach (Step step in Steps(path)) if (step.All || step.FilterKey != null) return true;
            return false;
        }
        public static List<object> Select(object root, string path)
        {
            List<object> current = new List<object>();
            current.Add(Unwrap(root));
            foreach (Step step in Steps(path))
            {
                List<object> next = new List<object>();
                foreach (object value in current)
                {
                    if (step.Key != null)
                    {
                        IDictionary<string, object> map = AsMap(value);
                        object found;
                        if (map != null && map.TryGetValue(step.Key, out found)) next.Add(Unwrap(found));
                        continue;
                    }
                    IList<object> list = AsList(value);
                    if (list == null) continue;
                    if (step.All) { foreach (object item in list) next.Add(Unwrap(item)); }
                    else if (step.FilterKey != null)
                    {
                        foreach (object item in list)
                        {
                            IDictionary<string, object> entry = AsMap(item);
                            object field;
                            if (entry != null && entry.TryGetValue(step.FilterKey, out field) && Plain(field) == step.FilterValue) next.Add(Unwrap(item));
                        }
                    }
                    else
                    {
                        int i = step.Index < 0 ? list.Count + step.Index : step.Index;
                        if (i >= 0 && i < list.Count) next.Add(Unwrap(list[i]));
                    }
                }
                current = next;
            }
            return current;
        }

        // ------------------------------------------------------ expectations
        // One expectation: {path, then any of exists, equals, notEquals, gt,
        // ge, lt, le, matches, type, oneOf, contains, notContains, length,
        // minLength, maxLength, png:{minEdge, longestEdgeAtLeast, width,
        // height}, message}. "" when it holds, else what is wrong.
        static readonly string[] Ops = { "exists", "equals", "notEquals", "gt", "ge", "lt", "le", "matches", "type", "oneOf", "contains", "notContains", "length", "minLength", "maxLength", "png" };
        public static string Check(object root, IDictionary<string, object> expectation)
        {
            object given;
            string path = expectation.TryGetValue("path", out given) ? Unwrap(given) as string : null;
            if (string.IsNullOrEmpty(path)) return "an expectation without a path";
            List<object> found = Select(root, path);
            bool many = IsMany(path);
            foreach (KeyValuePair<string, object> p in expectation)
            {
                if (p.Key == "path" || p.Key == "message") continue;
                if (Array.IndexOf(Ops, p.Key) < 0) return "unknown expectation \"" + p.Key + "\" for " + path;
                string why = Op(p.Key, Unwrap(p.Value), found, many, path);
                if (why.Length > 0) return why;
            }
            return "";
        }
        static string Said(List<object> found, bool many) { return many ? Short(found) : Short(found[0]); }
        static string Op(string op, object want, List<object> found, bool many, string path)
        {
            if (op == "exists")
            {
                bool wanted = Convert.ToBoolean(want, CultureInfo.InvariantCulture);
                if (wanted && found.Count == 0) return path + " is missing";
                if (!wanted && found.Count > 0) return path + " is there: " + Said(found, many);
                return "";
            }
            if (found.Count == 0 && !(many && (op == "length" || op == "maxLength" || op == "notContains")))
                return path + " is missing (wanted " + op + " " + Short(want) + ")";
            switch (op)
            {
                case "equals":
                    foreach (object v in found) if (!Same(v, want)) return path + " is " + Short(v) + ", not " + Short(want);
                    return "";
                case "notEquals":
                    foreach (object v in found) if (Same(v, want)) return path + " is " + Short(v);
                    return "";
                case "gt": case "ge": case "lt": case "le":
                    if (!IsNumber(want)) return op + " for " + path + " needs a number";
                    foreach (object v in found)
                    {
                        if (!IsNumber(v)) return path + " is " + Short(v) + ", not a number";
                        int c = CompareNumbers(v, want);
                        bool holds = op == "gt" ? c > 0 : op == "ge" ? c >= 0 : op == "lt" ? c < 0 : c <= 0;
                        if (!holds) return path + " is " + Short(v) + ", not " + op + " " + Short(want);
                    }
                    return "";
                case "matches":
                    foreach (object v in found)
                        if (v == null || !Regex.IsMatch(Plain(v), Plain(want))) return path + " is " + Short(v) + ", which does not match " + Plain(want);
                    return "";
                case "type":
                    foreach (object v in found) if (Kind(v) != Plain(want)) return path + " is " + Kind(v) + " " + Short(v) + ", not " + Plain(want);
                    return "";
                case "oneOf":
                {
                    IList<object> choices = AsList(want);
                    if (choices == null) return "oneOf for " + path + " needs a list";
                    foreach (object v in found)
                    {
                        bool any = false;
                        foreach (object choice in choices) any = any || Same(v, choice);
                        if (!any) return path + " is " + Short(v) + ", none of " + Short(want);
                    }
                    return "";
                }
                case "contains": case "notContains":
                {
                    bool has = false;
                    if (many) { foreach (object v in found) has = has || Same(v, want); }
                    else
                    {
                        object v = found[0];
                        string text = v as string;
                        IDictionary<string, object> map = AsMap(v);
                        IList<object> list = AsList(v);
                        if (text != null) has = text.Contains(Plain(want));
                        else if (map != null) has = map.ContainsKey(Plain(want));
                        else if (list != null) { foreach (object item in list) has = has || Same(item, want); }
                    }
                    if (op == "contains" && !has) return path + " has no " + Short(want) + ": " + Said(found, many);
                    if (op == "notContains" && has) return path + " has " + Short(want);
                    return "";
                }
                case "length": case "minLength": case "maxLength":
                {
                    if (!IsNumber(want)) return op + " for " + path + " needs a number";
                    long n;
                    if (many) n = found.Count;
                    else
                    {
                        object v = found[0];
                        IList<object> list = AsList(v);
                        IDictionary<string, object> map = AsMap(v);
                        string text = v as string;
                        if (text != null) n = text.Length;
                        else if (map != null) n = map.Count;
                        else if (list != null) n = list.Count;
                        else return path + " is " + Short(v) + ", which has no length";
                    }
                    long limit = Convert.ToInt64(want, CultureInfo.InvariantCulture);
                    bool holds = op == "length" ? n == limit : op == "minLength" ? n >= limit : n <= limit;
                    return holds ? "" : path + " has " + n + " (" + op + " " + limit + ")";
                }
                case "png":
                {
                    IDictionary<string, object> rules = AsMap(want);
                    if (rules == null) return "png for " + path + " takes {minEdge, longestEdgeAtLeast, width, height}";
                    foreach (object v in found)
                    {
                        string why = Png(Plain(v), rules, path);
                        if (why.Length > 0) return why;
                    }
                    return "";
                }
            }
            return "unknown expectation " + op;
        }
        // A base64 PNG's size from its IHDR, against the rules.
        static string Png(string base64, IDictionary<string, object> rules, string path)
        {
            byte[] bytes;
            try { bytes = Convert.FromBase64String(base64); }
            catch (FormatException) { return path + " is not base64"; }
            byte[] signature = { 0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a };
            if (bytes.Length < 24) return path + " is too short for a PNG (" + bytes.Length + " bytes)";
            for (int i = 0; i < 8; ++i) if (bytes[i] != signature[i]) return path + " is not a PNG";
            if (Encoding.ASCII.GetString(bytes, 12, 4) != "IHDR") return path + " is a PNG without IHDR first";
            long width = ((long)bytes[16] << 24) | ((long)bytes[17] << 16) | ((long)bytes[18] << 8) | bytes[19];
            long height = ((long)bytes[20] << 24) | ((long)bytes[21] << 16) | ((long)bytes[22] << 8) | bytes[23];
            foreach (KeyValuePair<string, object> rule in rules)
            {
                long n = Convert.ToInt64(Unwrap(rule.Value), CultureInfo.InvariantCulture);
                bool holds;
                if (rule.Key == "minEdge") holds = Math.Min(width, height) >= n;
                else if (rule.Key == "longestEdgeAtLeast") holds = Math.Max(width, height) >= n;
                else if (rule.Key == "width") holds = width == n;
                else if (rule.Key == "height") holds = height == n;
                else return "png for " + path + ": unknown rule " + rule.Key;
                if (!holds) return path + " is a " + width + "x" + height + " PNG (" + rule.Key + " " + n + ")";
            }
            return "";
        }
    }
}
'@
if (-not ('McpE2e.Json' -as [type])) { Add-Type -TypeDefinition $checkerSource -Language CSharp }

# The message of the innermost exception: a .NET method's own, not
# PowerShell's "Exception calling ..." around it.
function Get-Reason($record) {
    $exception = $record.Exception
    while ($exception.InnerException) { $exception = $exception.InnerException }
    return $exception.Message
}
function Get-Field($map, [string]$key, $default = $null) {
    if ($null -ne $map -and $map.ContainsKey($key)) { return $map[$key] }
    return $default
}

# The steps of one .jsonl file: one JSON object a line; blank lines and lines
# starting with # or // are skipped. A step is {name, request:{method,
# params?}, expect?:[...], save?:{variable: path}, files?:[{path, exists,
# minBytes}], timeoutSeconds?, stopOnFail?, progress?, note?}.
function Read-Steps([string]$file) {
    $known = 'name', 'request', 'expect', 'save', 'files', 'timeoutSeconds', 'stopOnFail', 'progress', 'note'
    $lines = [IO.File]::ReadAllLines($file, [Text.Encoding]::UTF8)
    $steps = New-Object 'System.Collections.Generic.List[object]'
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $text = $lines[$i].Trim()
        if ($text.Length -eq 0 -or $text.StartsWith('#') -or $text.StartsWith('//')) { continue }
        $where = '{0}:{1}' -f [IO.Path]::GetFileName($file), ($i + 1)
        try { $step = [McpE2e.Json]::Parse($text) }
        catch { throw "${where}: $(Get-Reason $_)" }
        if ([McpE2e.Json]::Kind($step) -ne 'object') { throw "${where}: a step is a JSON object." }
        foreach ($key in $step.Keys) { if ($known -notcontains $key) { throw "${where}: unknown step field '$key' (a step has $($known -join ', '))." } }
        $name = Get-Field $step 'name'
        $request = Get-Field $step 'request'
        if ([McpE2e.Json]::Kind($name) -ne 'string' -or $name.Length -eq 0) { throw "${where}: a step needs a name." }
        if ([McpE2e.Json]::Kind($request) -ne 'object' -or [McpE2e.Json]::Kind((Get-Field $request 'method')) -ne 'string') { throw "${where}: step '$name' needs request.method." }
        foreach ($expectation in @(Get-Field $step 'expect' @())) {
            if ([McpE2e.Json]::Kind($expectation) -ne 'object' -or [McpE2e.Json]::Kind((Get-Field $expectation 'path')) -ne 'string') { throw "${where}: every expectation of '$name' needs a path." }
            [void][McpE2e.Json]::IsMany($expectation['path'])
        }
        $steps.Add([pscustomobject]@{ Origin = $where; Step = $step })
    }
    return , $steps
}

# The checker on fixed answers, so a mistake in it shows without a game.
function Test-Checker {
    $png = [byte[]](0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0, 0, 0, 13, 0x49, 0x48, 0x44, 0x52, 0, 0, 1, 44, 0, 0, 0, 200, 8, 6, 0, 0, 0)
    $data = [Convert]::ToBase64String($png)
    $answer = [McpE2e.Json]::Parse('{"jsonrpc":"2.0","id":7,"result":{"isError":false,"protocolVersion":"2025-06-18","content":[{"type":"text","text":"Saved"},{"type":"image","mimeType":"image/png","data":"' + $data + '"}],' +
        '"structuredContent":{"summary":{"errors":0,"warnings":3,"profile":"release"},"actors":{"total":12,"classes":{"Engine.Light":3}},"file":"C:\\Game\\Packages\\MapsEd\\McpE2eD.sdc","level":18446744073709551615,"ratio":0.5,"tools":[{"name":"a"},{"name":"b"}]}},"_notifications":[{"method":"notifications/progress"}]}')
    $values = New-Object 'System.Collections.Generic.Dictionary[string,object]'
    $values['Total'] = [long]12
    $values['MapFile'] = 'McpE2eD'
    $cases = @(
        @('{"path":"result.isError","equals":false}', $true),
        @('{"path":"result.isError","equals":true}', $false),
        @('{"path":"result.protocolVersion","equals":"2025-06-18"}', $true),
        @('{"path":"result.structuredContent.summary.errors","equals":0,"type":"number"}', $true),
        @('{"path":"result.structuredContent.summary.warnings","gt":2,"le":3}', $true),
        @('{"path":"result.structuredContent.summary.warnings","lt":3}', $false),
        @('{"path":"result.structuredContent.actors.total","equals":"${Total}"}', $true),
        @('{"path":"result.structuredContent.actors.classes[''Engine.Light'']","equals":3}', $true),
        @('{"path":"result.structuredContent.actors.classes","contains":"Engine.Light"}', $true),
        @('{"path":"result.structuredContent.file","matches":"(?i)MapsEd[\\\\/]${MapFile}\\.sdc$"}', $true),
        @('{"path":"result.structuredContent.file","contains":"MapsEd"}', $true),
        @('{"path":"result.structuredContent.level","equals":18446744073709551615}', $true),
        @('{"path":"result.structuredContent.ratio","equals":0.5}', $true),
        @('{"path":"result.structuredContent.tools[*].name","contains":"b"}', $true),
        @('{"path":"result.structuredContent.tools[*].name","contains":"c"}', $false),
        @('{"path":"result.structuredContent.tools","minLength":2,"maxLength":2,"type":"array"}', $true),
        @('{"path":"result.structuredContent.tools[-1].name","equals":"b"}', $true),
        @('{"path":"result.content[?type=image].mimeType","equals":"image/png"}', $true),
        @('{"path":"result.content[?type=image].data","png":{"width":300,"height":200,"longestEdgeAtLeast":257}}', $true),
        @('{"path":"result.content[?type=image].data","png":{"minEdge":257}}', $false),
        @('{"path":"result.content[?type=text].text","oneOf":["Saved","Built"]}', $true),
        @('{"path":"result.content[?type=audio]","length":0}', $true),
        @('{"path":"result.missing","exists":false}', $true),
        @('{"path":"result.missing","equals":1}', $false),
        @('{"path":"_notifications[?method=notifications/progress]","minLength":1}', $true),
        @('{"path":"result.isError","bogus":1}', $false)
    )
    $failed = 0
    foreach ($case in $cases) {
        $expectation = [McpE2e.Json]::Substitute([McpE2e.Json]::Parse($case[0]), $values)
        $why = [McpE2e.Json]::Check($answer, $expectation)
        if (($why.Length -eq 0) -ne $case[1]) { $failed++; Write-Output ("  !! checker: {0} -> {1}" -f $case[0], $(if ($why) { $why } else { 'holds' })) }
    }
    $request = [McpE2e.Json]::Substitute([McpE2e.Json]::Parse('{"pid":"${Total}","path":"${MapFile}.sdc","list":[1,"${MapFile}"],"text":"line\n\"quoted\" \u00e9"}'), $values)
    $written = [McpE2e.Json]::Write($request)
    if ($written -ne ('{"pid":12,"path":"McpE2eD.sdc","list":[1,"McpE2eD"],"text":"line\n\"quoted\" ' + [char]0xe9 + '"}')) { $failed++; Write-Output "  !! checker: substitution wrote $written" }
    if (-not [McpE2e.Json]::Same([McpE2e.Json]::Parse($written), $request)) { $failed++; Write-Output '  !! checker: a written request does not read back the same' }
    $missing = $false
    try { [void][McpE2e.Json]::Substitute('${Nobody}', $values) } catch { $missing = $true }
    if (-not $missing) { $failed++; Write-Output '  !! checker: an unset ${name} was not refused' }
    if ($failed) { throw "The expectation checker failed $failed of its own tests." }
    Write-Output ('Expectation checker passed {0} tests.' -f ($cases.Count + 3))
}

function Resolve-Scripts {
    if ($Script.Count) { return @($Script | ForEach-Object { (Resolve-Path -LiteralPath $_).Path }) }
    return @(Get-ChildItem -LiteralPath (Join-Path $repository 'tests\mcp\e2e') -Filter '*.jsonl' | Sort-Object Name | ForEach-Object { $_.FullName })
}

if ($CheckScripts) {
    Test-Checker
    foreach ($file in Resolve-Scripts) {
        $steps = Read-Steps $file
        Write-Output ('{0}: {1} steps' -f [IO.Path]::GetFileName($file), $steps.Count)
    }
    return
}

if (-not ('McpE2e.Native' -as [type])) {
    Add-Type -Namespace McpE2e -Name Native -MemberDefinition @'
[DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
public static extern bool WritePrivateProfileString(string section, string key, string value, string file);
[DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
public static extern bool WaitNamedPipe(string name, uint timeout);
'@
}
function Set-Ini([string]$file, [string]$section, [string]$key, [string]$value) {
    if (-not [McpE2e.Native]::WritePrivateProfileString($section, $key, $value, $file)) { throw "Could not write [$section] $key in $file." }
}
# Whether an editor's bridge pipe exists, free or busy (ERROR_SEM_TIMEOUT).
function Test-Pipe([int]$processId) {
    if ([McpE2e.Native]::WaitNamedPipe("\\.\pipe\ReloadedEditor.Mcp.$processId", 1)) { return $true }
    return [Runtime.InteropServices.Marshal]::GetLastWin32Error() -eq 121
}
if (!$EditorDll) { $EditorDll = Join-Path $repository 'bin\Reloaded.Editor.dll' }
$nativeDll = (Resolve-Path -LiteralPath $EditorDll).Path
if (!$GameSystem) { throw 'Give -GameSystem (the game''s System folder) or set SCCT.' }
$installedSystem = (Resolve-Path -LiteralPath $GameSystem).Path
$installedRoot = Split-Path -Parent $installedSystem

# Stops the disposable editors, checking each one's executable first: this
# never closes an editor the user is running.
function Stop-TestEditors([string[]]$paths) {
    Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Path -and ($paths -contains $_.Path) } | ForEach-Object {
        Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue
        $_.WaitForExit(10000) | Out-Null
    }
}

if ($Native) {
    # The probe's section in run_native_map_recovery_test.ps1's install: that
    # script compiles the probe and prepares; this one selects the section,
    # turns the server on in the INI and runs it.
    $prepared = & (Join-Path $PSScriptRoot 'run_native_map_recovery_test.ps1') -EditorDll $nativeDll -GameSystem $installedSystem -GenerateFixture -WorkflowTools -PrepareOnly
    $rootLine = @($prepared | Where-Object { "$_" -match '^TestRoot=' }) | Select-Object -Last 1
    if (!$rootLine) { throw 'The native preparation did not report its TestRoot.' }
    $testRoot = "$rootLine".Substring(9)
    $testSystem = Join-Path $testRoot 'System'
    $config = Join-Path $testSystem 'native_recovery_test.ini'
    Set-Ini $config 'test' 'mcp_bridge_only' '1'
    Set-Ini $config 'test' 'visible_editor' $(if ($VisibleEditor) { '1' } else { '0' })
    Set-Ini (Join-Path $testSystem 'Reloaded_Editor.ini') 'Mcp' 'Enabled' '1'
    Write-Output "TestRoot=$testRoot"
    if ($PrepareOnly) { return }
    $testExecutable = Join-Path $testSystem 'ChaosTheory_Editor.exe'
    $launchedId = & (Join-Path $testSystem 'NativeMapRecoveryDriver.exe') $testExecutable (Join-Path $testSystem 'Reloaded.Editor.dll') (Join-Path $testSystem 'NativeMapRecoveryProbe.dll')
    if ($LASTEXITCODE) { throw "Isolated editor launch failed; files retained at $testRoot" }
    $editorProcessId = [int]($launchedId | Select-Object -Last 1)
    $report = Join-Path $testSystem 'native_recovery_report.txt'
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $streamed = 0
    try {
        while ([DateTime]::UtcNow -lt $deadline) {
            if (Test-Path -LiteralPath $report) {
                $lines = @(Get-Content -LiteralPath $report)
                for ($i = $streamed; $i -lt $lines.Count; $i++) {
                    $line = $lines[$i]
                    if ($line -match '^workflow_fail ') { Write-Output ('  !! FAILED CHECK: ' + $line.Substring(14)) }
                    elseif ($line -match '^workflow_check ') { Write-Output ('  ok ' + $line.Substring(15)) }
                    elseif ($line -notmatch '^workflow ') { Write-Output $line }
                }
                $streamed = $lines.Count
                if (($lines -join "`n") -match '(?m)^(PASS|FAIL) ') {
                    $failed = @($lines | Where-Object { $_ -match '^workflow_fail ' })
                    if ($failed.Count) { Write-Output ('' + $failed.Count + ' check(s) failed:'); $failed | ForEach-Object { Write-Output ('  ' + $_.Substring(14)) } }
                    if (($lines -join "`n") -match '(?m)^FAIL ') { throw "The MCP bridge native test failed; files retained at $testRoot" }
                    return
                }
            }
            if (!(Get-Process -Id $editorProcessId -ErrorAction SilentlyContinue)) { throw "The isolated editor exited before finishing; files retained at $testRoot" }
            $crash = Get-ChildItem -LiteralPath (Join-Path $testSystem 'Diagnostics') -Filter 'EditorCrash_*.log' -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($crash) { Get-Content -LiteralPath $crash.FullName -TotalCount 10; throw "The isolated editor crashed; report and dump retained at $testRoot" }
            Start-Sleep -Milliseconds 500
        }
        throw "The MCP bridge native test timed out; files retained at $testRoot"
    } finally { Stop-TestEditors @($testExecutable) }
}

# ------------------------------------------------- the disposable install
$scripts = Resolve-Scripts
if (!$scripts.Count) { throw 'No step files: tests\mcp\e2e\*.jsonl, or -Script.' }
$steps = New-Object 'System.Collections.Generic.List[object]'
foreach ($file in $scripts) { foreach ($step in (Read-Steps $file)) { $steps.Add($step) } }
if (!$McpExe) { $McpExe = Join-Path $repository 'bin\Reloaded.Editor.Mcp.exe' }
$mcpSource = (Resolve-Path -LiteralPath $McpExe).Path
if (!$Launcher) {
    $Launcher = Join-Path $repository 'bin\Reloaded_Editor.exe'
    if (!(Test-Path -LiteralPath $Launcher)) { $Launcher = Join-Path $installedSystem 'Reloaded_Editor.exe' }
}
$launcherSource = (Resolve-Path -LiteralPath $Launcher).Path
$testRoot = Join-Path $env:TEMP ('scct-mcp-e2e-' + [Guid]::NewGuid().ToString('N'))
$testSystem = Join-Path $testRoot 'System'
$testPackages = Join-Path $testRoot 'Packages'
$null = New-Item -ItemType Directory -Path $testSystem, $testPackages
# Every executable, DLL, configuration and map is a copy; only the immutable
# asset folders are shared, and no step writes to them.
Get-ChildItem -LiteralPath $installedSystem -File | Where-Object {
    $_.Extension -in '.exe', '.dll', '.ini', '.int', '.dat', '.config', '.bmp' -or $_.Name -eq 'ChaosTheory_Editor'
} | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $testSystem }
foreach ($name in 'EditorRes', '_PC_') {
    if (Test-Path -LiteralPath (Join-Path $installedSystem $name)) { Copy-Item -LiteralPath (Join-Path $installedSystem $name) -Destination $testSystem -Recurse }
}
foreach ($name in 'Animations', 'Sounds', 'Textures') {
    $assetSource = Join-Path (Join-Path $installedRoot 'Packages') $name
    if (Test-Path -LiteralPath $assetSource) { $null = New-Item -ItemType Junction -Path (Join-Path $testPackages $name) -Target $assetSource }
}
# A build may write the map's own mesh package: the copy gets its own folder.
$meshSource = Join-Path (Join-Path $installedRoot 'Packages') 'StaticMeshes'
if (Test-Path -LiteralPath $meshSource) { Copy-Item -LiteralPath $meshSource -Destination $testPackages -Recurse }
if (Test-Path -LiteralPath (Join-Path $installedRoot 'Menus')) { $null = New-Item -ItemType Junction -Path (Join-Path $testRoot 'Menus') -Target (Join-Path $installedRoot 'Menus') }
$testMapsEd = Join-Path $testPackages 'MapsEd'
$null = New-Item -ItemType Directory -Path (Join-Path $testPackages 'Maps'), $testMapsEd
Copy-Item -LiteralPath $nativeDll -Destination (Join-Path $testSystem 'Reloaded.Editor.dll') -Force
Copy-Item -LiteralPath $launcherSource -Destination (Join-Path $testSystem 'Reloaded_Editor.exe') -Force
Copy-Item -LiteralPath $mcpSource -Destination (Join-Path $testSystem 'Reloaded.Editor.Mcp.exe') -Force
$editorPaths = @((Join-Path $testSystem 'ChaosTheory_Editor.exe'), (Join-Path $testSystem 'ChaosTheory_Editor'))
$ini = Join-Path $testSystem 'Reloaded_Editor.ini'
# No update check or crash-recovery question at start-up, which would wait
# for a user; the MCP server on, approval, raw commands and Play Level off.
Set-Ini $ini 'Updates' 'CheckOnStartup' '0'
Set-Ini $ini 'CrashRecovery' 'OfferAtStartup' '0'
Set-Ini $ini 'Mcp' 'Enabled' '1'
Set-Ini $ini 'Mcp' 'RequireApproval' '0'
Set-Ini $ini 'Mcp' 'AllowRawExec' '0'
Set-Ini $ini 'Mcp' 'AllowPlay' '0'
Set-Ini $ini 'Mcp' 'WorkDir' (Join-Path $testRoot 'McpWork')
Write-Output "TestRoot=$testRoot"
if ($PrepareOnly) { return }

$testMcp = Join-Path $testSystem 'Reloaded.Editor.Mcp.exe'
$transcript = Join-Path $testRoot 'mcp_e2e_transcript.txt'
$failures = New-Object 'System.Collections.Generic.List[string]'
$server = $null
$script:pending = $null
function Add-Transcript([string]$direction, [string]$text) {
    if ($direction -eq '<') {
        # Pictures and blobs are cut, so the transcript stays readable.
        try { $text = [McpE2e.Json]::Write([McpE2e.Json]::Strip([McpE2e.Json]::Parse($text), 400)) } catch { }
    }
    [IO.File]::AppendAllText($transcript, ('{0} {1} {2}' -f [DateTime]::Now.ToString('HH:mm:ss.fff'), $direction, $text) + "`r`n", [Text.Encoding]::UTF8)
}
function Send-Line([string]$text) {
    $bytes = [Text.Encoding]::UTF8.GetBytes($text + "`n")
    $server.StandardInput.BaseStream.Write($bytes, 0, $bytes.Length)
    $server.StandardInput.BaseStream.Flush()
    Add-Transcript '>' $text
}
# The next line the server wrote, read, or $null when none came in time. One
# read stays outstanding between calls, so no line is lost to a timeout.
function Read-Message([int]$milliseconds) {
    if ($null -eq $script:pending) { $script:pending = $server.StandardOutput.ReadLineAsync() }
    if (-not $script:pending.Wait($milliseconds)) { return $null }
    $line = $script:pending.Result
    $script:pending = $null
    if ($null -eq $line) { throw 'Reloaded.Editor.Mcp.exe closed its output (it exited).' }
    Add-Transcript '<' $line
    if ($line.Trim().Length -eq 0) { return $null }
    return , ([McpE2e.Json]::Parse($line))
}
function Add-Failure([string]$text) {
    $failures.Add($text)
    Write-Output ('  !! FAILED CHECK: ' + $text)
}

# Each step: its request (with ${name} values) to the server, then its
# response against its expectations; the server's notifications on the way
# are kept for the step's _notifications.
function Invoke-Steps($steps, $values, [int]$runSeconds) {
    $runDeadline = [DateTime]::UtcNow.AddSeconds($runSeconds)
    $nextId = 0
    foreach ($entry in $steps) {
        if ([DateTime]::UtcNow -gt $runDeadline) { Add-Failure "the run took longer than $runSeconds s; stopped before '$($entry.Step['name'])'"; break }
        $step = $entry.Step
        $name = $step['name']
        $before = $failures.Count
        $request = [McpE2e.Json]::Substitute($step['request'], $values)
        $method = $request['method']
        $notification = $method.StartsWith('notifications/')
        $request['jsonrpc'] = '2.0'
        if (-not $notification) {
            $nextId++
            $request['id'] = [long]$nextId
            # A progress token asks the server for notifications/progress.
            if ((Get-Field $step 'progress' $false) -and $method -eq 'tools/call') {
                $params = Get-Field $request 'params'
                $meta = New-Object 'System.Collections.Generic.Dictionary[string,object]'
                $meta['progressToken'] = 'e2e-' + $nextId
                $params['_meta'] = $meta
            }
        }
        Write-Output "step $name ($($entry.Origin))"
        Send-Line ([McpE2e.Json]::Write($request))
        if ($notification) { continue }
        $timeout = [int](Get-Field $step 'timeoutSeconds' 300)
        $deadline = [DateTime]::UtcNow.AddSeconds($timeout)
        $notes = New-Object 'System.Collections.Generic.List[object]'
        $response = $null
        while ($null -eq $response -and [DateTime]::UtcNow -lt $deadline) {
            $message = Read-Message 1000
            if ($null -eq $message) { continue }
            if ([McpE2e.Json]::Kind($message) -ne 'object') { Write-Output ('    (not a JSON-RPC message: ' + [McpE2e.Json]::Short($message) + ')'); continue }
            if ($message.ContainsKey('method')) {
                if ($message.ContainsKey('id')) {
                    # This client offers no roots, sampling or elicitation.
                    Send-Line ('{"jsonrpc":"2.0","id":' + [McpE2e.Json]::Write($message['id']) + ',"error":{"code":-32601,"message":"The e2e client does not answer ' + $message['method'] + '."}}')
                }
                else {
                    $notes.Add($message)
                    if ($message['method'] -eq 'notifications/message') { Write-Output ('    log: ' + [McpE2e.Json]::Short((Get-Field $message 'params'))) }
                }
                continue
            }
            if ($message.ContainsKey('id') -and [McpE2e.Json]::Same($message['id'], $request['id'])) { $response = $message }
            else { Write-Output ('    (a reply to another request: ' + [McpE2e.Json]::Short($message) + ')') }
        }
        if ($null -eq $response) { Add-Failure "${name}: no response within $timeout s" }
        else {
            $response['_notifications'] = $notes
            if ($response.ContainsKey('error')) { Write-Output ('    JSON-RPC error: ' + [McpE2e.Json]::Short($response['error'])) }
            $isError = [McpE2e.Json]::Select($response, 'result.isError')
            if ($isError.Count -and $isError[0] -eq $true) {
                $text = [McpE2e.Json]::Select($response, 'result.content[0].text')
                if ($text.Count) { Write-Output ('    tool error: ' + [McpE2e.Json]::Short($text[0])) }
            }
            foreach ($expectation in @(Get-Field $step 'expect' @())) {
                $why = $null
                try { $why = [McpE2e.Json]::Check($response, [McpE2e.Json]::Substitute($expectation, $values)) }
                catch { $why = Get-Reason $_ }
                $label = Get-Field $expectation 'message'
                $what = $(if ($label) { $label } else { [McpE2e.Json]::Write($expectation) })
                if ($why) { Add-Failure "${name}: $what ($why)" } else { Write-Output "  ok ${name}: $what" }
            }
            $save = Get-Field $step 'save'
            if ($save) {
                foreach ($variable in @($save.Keys)) {
                    $found = [McpE2e.Json]::Select($response, $save[$variable])
                    if ($found.Count) { $values[$variable] = $found[0]; Write-Output ('    {0} = {1}' -f $variable, [McpE2e.Json]::Short($found[0])) }
                    else { Add-Failure "${name}: nothing at $($save[$variable]) to save as $variable" }
                }
            }
        }
        foreach ($check in @(Get-Field $step 'files' @())) {
            $path = [McpE2e.Json]::Plain([McpE2e.Json]::Substitute($check['path'], $values))
            $exists = Test-Path -LiteralPath $path
            $wanted = [bool](Get-Field $check 'exists' $true)
            if ($exists -ne $wanted) { Add-Failure "${name}: $path $(if ($wanted) { 'is missing' } else { 'should not exist' })"; continue }
            $minimum = Get-Field $check 'minBytes'
            if ($exists -and $null -ne $minimum -and (Get-Item -LiteralPath $path).Length -lt [long]$minimum) { Add-Failure "${name}: $path is under $minimum bytes" }
            elseif ($wanted) { Write-Output "  ok ${name}: $path exists" }
        }
        if ($failures.Count -gt $before -and (Get-Field $step 'stopOnFail' $false)) { Write-Output "  stopping: '$name' must pass for the rest to mean anything"; break }
    }
}

try {
    # The server's own check first: it needs no editor.
    $selfTest = & $testMcp --self-test
    if ($LASTEXITCODE) { $selfTest | Write-Output; throw "Reloaded.Editor.Mcp.exe --self-test failed; files retained at $testRoot" }
    Write-Output ('  ok ' + (@($selfTest) | Select-Object -Last 1))

    # The editor, through the launcher as a user starts it, with -reMcp as
    # editor_launch passes it; ready once its bridge pipe is there.
    Start-Process -FilePath (Join-Path $testSystem 'Reloaded_Editor.exe') -ArgumentList '-reMcp' -WorkingDirectory $testSystem | Out-Null
    $deadline = [DateTime]::UtcNow.AddSeconds($StartSeconds)
    $editor = $null
    while ([DateTime]::UtcNow -lt $deadline) {
        $editor = Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Path -and ($editorPaths -contains $_.Path) } | Select-Object -First 1
        if ($editor -and (Test-Pipe $editor.Id)) { break }
        $crash = Get-ChildItem -LiteralPath (Join-Path $testSystem 'Diagnostics') -Filter 'EditorCrash_*.log' -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($crash) { Get-Content -LiteralPath $crash.FullName -TotalCount 10; throw "The editor crashed while starting; files retained at $testRoot" }
        $editor = $null
        Start-Sleep -Milliseconds 500
    }
    if (!$editor) { throw "The editor did not open its MCP pipe within $StartSeconds s (see $testSystem\Reloaded_Editor.exe.log); files retained at $testRoot" }
    Write-Output "  ok the editor (process $($editor.Id)) opened its MCP pipe"

    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $testMcp
    $info.WorkingDirectory = $testSystem
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardInput = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.StandardOutputEncoding = New-Object System.Text.UTF8Encoding($false)
    $info.StandardErrorEncoding = New-Object System.Text.UTF8Encoding($false)
    $server = [System.Diagnostics.Process]::Start($info)
    $stderr = $server.StandardError.ReadToEndAsync()

    $values = New-Object 'System.Collections.Generic.Dictionary[string,object]'
    $values['Repository'] = $repository
    $values['TestRoot'] = $testRoot
    $values['TestSystem'] = $testSystem
    $values['MapsEd'] = $testMapsEd
    $values['MapFile'] = $MapFile
    $values['EditorPid'] = [long]$editor.Id
    # A server that dies ends the steps; what it wrote to stderr says why.
    try { Invoke-Steps $steps $values $TimeoutSeconds }
    catch { Add-Failure (Get-Reason $_) }

    # End of input: the server finishes and exits by itself.
    try { $server.StandardInput.Close() } catch { }
    if (-not $server.WaitForExit(20000)) { Add-Failure 'Reloaded.Editor.Mcp.exe did not exit within 20 s of the end of its input'; $server.Kill() }
    elseif ($server.ExitCode -ne 0) { Add-Failure "Reloaded.Editor.Mcp.exe exited with code $($server.ExitCode)" }
    if ($stderr.Wait(5000)) { [IO.File]::WriteAllText((Join-Path $testRoot 'mcp_stderr.log'), $stderr.Result, [Text.Encoding]::UTF8) }
}
finally {
    if ($server -and -not $server.HasExited) { $server.Kill() }
    if (!$KeepEditor) { Stop-TestEditors $editorPaths }
}
Write-Output "Transcript: $transcript"
if ($failures.Count) {
    Write-Output ('' + $failures.Count + ' check(s) failed:')
    $failures | ForEach-Object { Write-Output ('  ' + $_) }
    throw "The MCP end-to-end test failed; files retained at $testRoot"
}
Write-Output "PASS MCP end-to-end: $($steps.Count) steps; files retained at $testRoot"
