using System.Diagnostics;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;

var sw = Stopwatch.StartNew();

bool verbose = args.Contains("-v") || args.Contains("--verbose");
var positional = args.Where(a => !a.StartsWith('-')).ToArray();

if (positional.Length < 2 ||
    !Path.GetFileName(positional[0]).Equals("CMakeLists.txt", StringComparison.OrdinalIgnoreCase))
{
    Log.Error("Usage: <path/to/CMakeLists.txt> <outputDir> [-v|--verbose]");
    return 1;
}

string filePath = positional[0];
string outputDir = positional[1];
string sourceDir = Path.GetDirectoryName(Path.GetFullPath(filePath))!;

if (!File.Exists(filePath))
{
    Log.Error($"CMakeLists.txt not found: {filePath}");
    return 1;
}

Directory.CreateDirectory(outputDir);

var markerPattern = new Regex(@"\bK_(?:STRUCT|ENUM)\s*\(\s*\)");

var declPattern = new Regex(
    @"\bK_(?:STRUCT|ENUM)\s*\(\s*\)\s*" +
    @"(?:struct|enum\s+(?:class\s+|struct\s+)?\w+)" +
    @"[^;{}]*\{(?:[^{}]|\{[^{}]*\})*\}\s*;",
    RegexOptions.Singleline);

var nameStripPattern = new Regex(@"^\s*K_(?:STRUCT|ENUM)\s*\(\s*\)\s*");
var typeNamePattern = new Regex(@"^(struct|enum)\s+(?:class\s+|struct\s+)?(\w+)");

int generated = 0, upToDate = 0, warnings = 0, errors = 0;

Log.Info($"Scanning {sourceDir}");

foreach (var file in Directory.GetFiles(sourceDir, "*.h").OrderBy(f => f))
{
    string name = Path.GetFileName(file);

    try
    {
        var source = File.ReadAllText(file);

        var markerCount = markerPattern.Matches(source).Count;
        if (markerCount == 0)
            continue;

        var matches = declPattern.Matches(source);

        if (matches.Count < markerCount)
        {
            warnings++;
            Log.Warn($"{name}: {markerCount} K_STRUCT/K_ENUM markers but only {matches.Count} parsed. " +
                     "Check for a missing ';' after '}' or braces nested deeper than 2 levels.");
        }

        if (matches.Count == 0)
            continue;

        string checksum = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(source)));

        bool alreadyGenerated = Directory
            .GetFiles(outputDir, "*.generated.h")
            .Any(g => File.ReadLines(g).Take(3)
                .Any(line => line.Equals($"// Hash: {checksum}", StringComparison.OrdinalIgnoreCase)));

        if (alreadyGenerated)
        {
            upToDate++;
            if (verbose) Log.Dim($"{name}: up to date");
            continue;
        }

        var sb = new StringBuilder();
        sb.AppendLine("// Auto-generated. Do not edit.");
        sb.AppendLine($"// Source: {name}");
        sb.AppendLine($"// Hash: {checksum}");
        sb.AppendLine();

        var types = new List<string>();

        foreach (Match match in matches)
        {
            var declaration = nameStripPattern.Replace(match.Value, "");

            var t = typeNamePattern.Match(declaration);
            types.Add(t.Success ? $"{t.Groups[1].Value} {t.Groups[2].Value}" : "?");

            sb.AppendLine(declaration);
            sb.AppendLine();
        }

        var outputFile = Path.Combine(outputDir, Path.GetFileNameWithoutExtension(file) + ".generated.h");
        File.WriteAllText(outputFile, sb.ToString());

        generated++;
        Log.Ok($"{name} -> {Path.GetFileName(outputFile)}  [{string.Join(", ", types)}]");
    }
    catch (Exception ex)
    {
        errors++;
        Log.Error($"{name}: {ex.GetType().Name}: {ex.Message}");
    }
}

var parts = new List<string>();
if (generated > 0) parts.Add($"{generated} generated");
if (upToDate > 0) parts.Add($"{upToDate} up to date");
if (warnings > 0) parts.Add($"{warnings} warning{(warnings == 1 ? "" : "s")}");
if (errors > 0) parts.Add($"{errors} error{(errors == 1 ? "" : "s")}");
if (parts.Count == 0) parts.Add("nothing to do (no K_STRUCT/K_ENUM found)");

var summary = $"{string.Join(", ", parts)} in {sw.ElapsedMilliseconds}ms";
if (errors > 0) Log.Error(summary);
else if (warnings > 0) Log.Warn(summary);
else Log.Info(summary);

return errors > 0 ? 1 : 0;

static class Log
{
    static readonly bool Color =
        !Console.IsOutputRedirected && Environment.GetEnvironmentVariable("NO_COLOR") is null;

    static void Write(ConsoleColor color, string tag, string msg, bool err = false)
    {
        var w = err ? Console.Error : Console.Out;
        if (Color) Console.ForegroundColor = color;
        w.Write(tag);
        if (Color) Console.ResetColor();
        w.WriteLine(" " + msg);
    }

    public static void Info(string msg) => Write(ConsoleColor.Cyan, "[kgen]", msg);
    public static void Ok(string msg) => Write(ConsoleColor.Green, "[ ok ]", msg);
    public static void Warn(string msg) => Write(ConsoleColor.Yellow, "[warn]", msg);
    public static void Error(string msg) => Write(ConsoleColor.Red, "[fail]", msg, err: true);
    public static void Dim(string msg) => Write(ConsoleColor.DarkGray, "[ -- ]", msg);
}