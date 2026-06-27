using System.Diagnostics;

namespace Monolith.Settings.Services;

internal static class StartupTrace
{
    private static readonly object Gate = new();
    private static readonly Stopwatch Clock = Stopwatch.StartNew();
    private static readonly List<string> Marks = new();
    private static bool flushed;

    public static void Mark(string name)
    {
        string line = $"{Clock.ElapsedMilliseconds,6} ms  {name}";
        lock (Gate)
        {
            if (flushed)
            {
                TryAppendLines(new[] { line });
                return;
            }

            Marks.Add(line);
        }
    }

    public static void MarkDuration(string name, long elapsedMs)
    {
        string line = $"{Clock.ElapsedMilliseconds,6} ms  {name}: {elapsedMs} ms";
        lock (Gate)
        {
            if (flushed)
            {
                TryAppendLines(new[] { line });
                return;
            }

            Marks.Add(line);
        }
    }

    public static void MarkProcessStart()
    {
        try
        {
            DateTime start = Process.GetCurrentProcess().StartTime.ToUniversalTime();
            long elapsed = (long)(DateTime.UtcNow - start).TotalMilliseconds;
            MarkDuration("process launch to App ctor", elapsed);
        }
        catch
        {
            Mark("process launch timestamp unavailable");
        }
    }

    public static void Flush()
    {
        List<string> snapshot;
        lock (Gate)
        {
            if (flushed)
                return;

            flushed = true;
            snapshot = Marks.ToList();
        }

        try
        {
            string header = $"--- {DateTimeOffset.Now:yyyy-MM-dd HH:mm:ss.fff zzz} ---";
            TryAppendLines(new[] { header }.Concat(snapshot));
        }
        catch
        {
            // Startup tracing must never block Settings from opening.
        }
    }

    private static void AppendLines(IEnumerable<string> lines)
    {
        string dir = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "Monolith");
        Directory.CreateDirectory(dir);
        string path = Path.Combine(dir, "settings-startup.log");
        RotateIfTooLarge(path, maxBytes: 1024 * 1024, backups: 3);
        File.AppendAllLines(path, lines);
    }

    private static void RotateIfTooLarge(string path, long maxBytes, int backups)
    {
        try
        {
            FileInfo info = new(path);
            if (!info.Exists || info.Length <= maxBytes)
                return;

            for (int i = backups; i >= 1; --i)
            {
                string src = i == 1 ? path : $"{path}.{i - 1}";
                string dst = $"{path}.{i}";
                if (i == backups && File.Exists(dst))
                    File.Delete(dst);
                if (File.Exists(src))
                    File.Move(src, dst, overwrite: true);
            }
        }
        catch
        {
            // Log retention is best-effort and must never block startup.
        }
    }

    private static void TryAppendLines(IEnumerable<string> lines)
    {
        try
        {
            AppendLines(lines);
        }
        catch
        {
        }
    }
}
