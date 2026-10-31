using System;
using System.Diagnostics;
using System.IO;
using System.Linq;

namespace CrimeFactory.Automation
{
    /// <summary>Archives staged contents without adding a containing plugin directory.</summary>
    public static class CrimeFactoryArchive
    {
        public static string Extension(string utility) => utility switch
        {
            "WinRAR" => ".rar",
            "SevenZip" => ".7z",
            _ => throw new ArgumentException("Unknown CrimeFactory archive utility: " + utility)
        };

        public static void ValidateExecutable(string utility, string executable)
        {
            Extension(utility);
            if (string.IsNullOrWhiteSpace(executable) || !File.Exists(executable)
                || !string.Equals(Path.GetExtension(executable), ".exe", StringComparison.OrdinalIgnoreCase))
                throw new FileNotFoundException(utility + " executable is missing or invalid. Configure its Executable Path in CrimeFactory Project Settings.", executable);
        }

        public static void Create(string source, string destination, string utility, string executable, Action<string> log)
        {
            ValidateExecutable(utility, executable);
            source = Path.GetFullPath(source);
            executable = Path.GetFullPath(executable);
            destination = Path.GetFullPath(destination);
            if (!Directory.Exists(source)) throw new DirectoryNotFoundException("Archive staging directory is missing: " + source);
            var directory = Path.GetDirectoryName(destination);
            // A unique sibling preserves the previous archive if creation or validation fails.
            var temporary = Path.Combine(directory, Guid.NewGuid().ToString("N") + Extension(utility));
            try
            {
                Directory.CreateDirectory(directory);
                using (File.Create(temporary)) { } // Check destination access before launching.
                File.Delete(temporary);
                log("Archiving with " + utility + ": " + executable + " -> " + destination);
                if (utility == "SevenZip")
                    Run(executable, source, log, "a", "-t7z", "-y", "-bd", "--", temporary, "*");
                else
                    Run(executable, source, log, "a", "-afrar", "-r", "-y", "-ibck", "-cfg-", "--", temporary, "*");

                // Existence alone cannot distinguish a valid result from a stub executable's output.
                var signature = new byte[8];
                using (var stream = File.OpenRead(temporary))
                {
                    if (stream.Read(signature, 0, signature.Length) != signature.Length)
                        throw new InvalidDataException("Archive output is empty or truncated: " + temporary);
                }
                var expected = utility == "SevenZip"
                    ? new byte[] { 0x37, 0x7a, 0xbc, 0xaf, 0x27, 0x1c }
                    : new byte[] { 0x52, 0x61, 0x72, 0x21, 0x1a, 0x07 };
                if (!signature.Take(expected.Length).SequenceEqual(expected))
                    throw new InvalidDataException("Archive output has an invalid " + Extension(utility) + " signature.");
                if (utility == "SevenZip")
                    Run(executable, source, log, "t", "-y", "-bd", "--", temporary);
                else
                    Run(executable, source, log, "t", "-y", "-ibck", "-cfg-", "--", temporary);
                File.Move(temporary, destination, true);
                log("Created and verified archive: " + destination);
            }
            catch (Exception exception)
            {
                throw new IOException(utility + " archive creation failed for " + destination + ": " + exception.Message, exception);
            }
            finally
            {
                if (File.Exists(temporary)) File.Delete(temporary);
            }
        }

        private static void Run(string executable, string workingDirectory, Action<string> log, params string[] arguments)
        {
            var start = new ProcessStartInfo(executable)
            {
                WorkingDirectory = workingDirectory,
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true
            };
            // ArgumentList handles Windows quoting without passing through a shell.
            foreach (var argument in arguments) start.ArgumentList.Add(argument);
            log("Archive command: " + string.Join(" ", arguments.Select(argument => "\"" + argument + "\"")));
            using var process = Process.Start(start) ?? throw new IOException("Archive utility failed to start: " + executable);
            var output = process.StandardOutput.ReadToEndAsync();
            var error = process.StandardError.ReadToEndAsync();
            process.WaitForExit();
            var stdout = output.GetAwaiter().GetResult();
            var stderr = error.GetAwaiter().GetResult();
            if (!string.IsNullOrWhiteSpace(stdout)) log(stdout);
            if (!string.IsNullOrWhiteSpace(stderr)) log(stderr);
            log("Archive process exit code: " + process.ExitCode);
            if (process.ExitCode != 0)
                throw new IOException("Archive utility returned exit code " + process.ExitCode + ". " + stderr + " " + stdout);
        }
    }
}
