using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Text.Json;
using SystemJsonNode = System.Text.Json.Nodes.JsonNode;
using SystemJsonObject = System.Text.Json.Nodes.JsonObject;
using SystemJsonArray = System.Text.Json.Nodes.JsonArray;
using AutomationTool;
using EpicGames.Core;
using UnrealBuildTool;
using AutomationScripts;

namespace CrimeFactory.Automation
{
	public class PD3Params
	{
		public bool CopyToGameDirectory;
		public bool StartGame;
		public string GameDirectory;
		public string LaunchGameType;
        public string ZipExportDirectory;
        public bool CreateUtilityArchive;
        public string ArchiveUtility = "WinRAR";
        public string ArchiveExecutable;
	}

	internal sealed class CrimeFactoryPALPackagingManifest
	{
		public string SourceFile { get; set; } = "";
		public string PluginName { get; set; } = "";
		public string PAL { get; set; } = "";
		public int ChunkId { get; set; } = -1;
		public List<string> ExternalCookPackages { get; } =
			new List<string>();
		public List<string> ExpectedChunkPackages { get; } =
			new List<string>();
		public List<CrimeFactoryAdditionalPakFile> AdditionalPakFiles { get; } =
			new List<CrimeFactoryAdditionalPakFile>();
		// PASS 2 uses a small cooker command line plus a disk-backed package list.
		// Keep both locations so we can prove the request reached the cooker.
		public string Pass2PackageManifestFile { get; set; } = "";
		public string Pass2CommandFile { get; set; } = "";
	}

	internal sealed class CrimeFactoryAdditionalPakFile
	{
		public string Source { get; set; } = "";
		public string StagedPath { get; set; } = "";
	}

	/// <summary>
	/// Writes a small structured result file that the editor-side CrimeFactory UI
	/// can consume when UAT finishes. This avoids forcing users to dig through
	/// thousands of UAT lines just to find the phase and actual exception.
	/// </summary>
	internal sealed class CrimeFactoryPackageResult
	{
		private readonly string ResultFile;
		private readonly Stopwatch Stopwatch = Stopwatch.StartNew();

		public string PluginName { get; set; } = "";
		public string Phase { get; private set; } = "Initialization";
		public string Mode { get; set; } = "Unknown";
		public int ChunkId { get; set; } = -1;

		public CrimeFactoryPackageResult(string resultFile)
		{
			ResultFile = resultFile;
		}

		public void SetPhase(string phase)
		{
			Phase = string.IsNullOrWhiteSpace(phase)
				? "Unknown"
				: phase;
		}

		private static string GetUATLogFolder()
		{
			return Environment.GetEnvironmentVariable("uebp_LogFolder") ?? "";
		}

		private string FindMostRelevantLogFile()
		{
			try
			{
				string logFolder = GetUATLogFolder();
				if (string.IsNullOrWhiteSpace(logFolder) ||
				    !Directory.Exists(logFolder))
				{
					return "";
				}

				string[] files = Directory.GetFiles(
					logFolder,
					"*.txt",
					SearchOption.TopDirectoryOnly
				);

				if (files.Length == 0)
				{
					return "";
				}

				IEnumerable<string> preferred = files;

				if (Phase.IndexOf(
						"Cook",
						StringComparison.OrdinalIgnoreCase) >= 0)
				{
					var cookFiles = files.Where(
						file => Path.GetFileName(file).StartsWith(
							"Cook",
							StringComparison.OrdinalIgnoreCase
						)
					);

					if (cookFiles.Any())
					{
						preferred = cookFiles;
					}
				}
				else if (Phase.IndexOf(
						"Build",
						StringComparison.OrdinalIgnoreCase) >= 0)
				{
					var buildFiles = files.Where(
						file =>
						{
							string name = Path.GetFileName(file);
							return name.StartsWith(
								"UBT",
								StringComparison.OrdinalIgnoreCase
							) ||
							name.IndexOf(
								"Build",
								StringComparison.OrdinalIgnoreCase
							) >= 0;
						}
					);

					if (buildFiles.Any())
					{
						preferred = buildFiles;
					}
				}

				return preferred
					.OrderByDescending(File.GetLastWriteTimeUtc)
					.FirstOrDefault() ?? "";
			}
			catch
			{
				return "";
			}
		}

		private void Write(
			string status,
			string message,
			Exception exception)
		{
			if (string.IsNullOrWhiteSpace(ResultFile))
			{
				return;
			}

			try
			{
				string directory = Path.GetDirectoryName(ResultFile);
				if (!string.IsNullOrWhiteSpace(directory))
				{
					Directory.CreateDirectory(directory);
				}

				string logFolder = GetUATLogFolder();
				string logFile = FindMostRelevantLogFile();

				using (var writer = new JsonWriter(ResultFile))
				{
					writer.WriteObjectStart();

					writer.WriteValue("Status", status ?? "");
					writer.WriteValue("PluginName", PluginName ?? "");
					writer.WriteValue("Phase", Phase ?? "");
					writer.WriteValue("Mode", Mode ?? "");
					writer.WriteValue("ChunkId", ChunkId.ToString());
					writer.WriteValue("Message", message ?? "");
					writer.WriteValue(
						"ExceptionType",
						exception?.GetType().FullName ?? ""
					);
					writer.WriteValue(
						"Details",
						exception?.ToString() ?? ""
					);
					writer.WriteValue("LogFolder", logFolder ?? "");
					writer.WriteValue("LogFile", logFile ?? "");
					writer.WriteValue(
						"DurationSeconds",
						Stopwatch.Elapsed.TotalSeconds.ToString("F2")
					);
					writer.WriteValue(
						"TimestampUtc",
						DateTime.UtcNow.ToString("O")
					);

					writer.WriteObjectEnd();
				}
			}
			catch
			{
				// Never mask the real packaging failure because diagnostic
				// result-file writing itself failed.
			}
		}

		public void WriteSuccess()
		{
			Write(
				"Success",
				"Packaging completed successfully.",
				null
			);
		}

		public void WriteFailure(Exception exception)
		{
			Write(
				"Failure",
				exception?.Message ?? "AutomationTool failed.",
				exception
			);
		}
	}

	public class PackagePlugin : BuildCommand
	{
		private const string DesktopGameExecutableName = "PAYDAY3.exe";
		private const string WinGDKGameExecutableName = "PAYDAY3-WinGDK-Shipping.exe";
		private const string WinGDKClientExecutableName = "PAYDAY3Client-WinGDK-Shipping.exe";
		private const string WinGDKLaunchHelperExecutableName = "GameLaunchHelper.exe";
		private const string SteamAppId = "1272080";
		private const string ModDeployRoot = "Mods";
		private const string DefaultBaseReleaseVersion = "SOMERELEASE1";

		private sealed class PD3GameLayout
		{
			public string InstallDirectory;
			public string GameContentDirectory;
			public string ExecutablePath;
			public string PaksDirectory;
			public string ModsDirectory;
			public bool IsMicrosoftStore;
		}

		private static PD3GameLayout TryCreateGameLayout(
			string installDirectory,
			string gameContentDirectory,
			string executablePath,
			bool isMicrosoftStore)
		{
			if (string.IsNullOrWhiteSpace(installDirectory) ||
			    string.IsNullOrWhiteSpace(gameContentDirectory) ||
			    string.IsNullOrWhiteSpace(executablePath))
			{
				return null;
			}

			var paksDirectory = Path.Combine(
				gameContentDirectory,
				"Content",
				"Paks");

			if (!Directory.Exists(installDirectory) ||
			    !Directory.Exists(gameContentDirectory) ||
			    !File.Exists(executablePath) ||
			    !Directory.Exists(paksDirectory))
			{
				return null;
			}

			return new PD3GameLayout
			{
				InstallDirectory = Path.GetFullPath(installDirectory),
				GameContentDirectory = Path.GetFullPath(gameContentDirectory),
				ExecutablePath = Path.GetFullPath(executablePath),
				PaksDirectory = Path.GetFullPath(paksDirectory),
				ModsDirectory = Path.GetFullPath(
					Path.Combine(gameContentDirectory, ModDeployRoot)),
				IsMicrosoftStore = isMicrosoftStore
			};
		}

		private static PD3GameLayout TryCreateWinGDKGameLayout(
			string installDirectory,
			string gameContentDirectory)
		{
			var winGDKDirectory = Path.Combine(
				gameContentDirectory,
				"Binaries",
				"WinGDK");

			foreach (var executableName in new[]
			         {
				         WinGDKGameExecutableName,
				         WinGDKClientExecutableName
			         })
			{
				var layout = TryCreateGameLayout(
					installDirectory,
					gameContentDirectory,
					Path.Combine(winGDKDirectory, executableName),
					true);

				if (layout != null)
				{
					return layout;
				}
			}

			return null;
		}

		private static PD3GameLayout ResolvePAYDAY3GameLayout(
			string gameDirectory)
		{
			if (string.IsNullOrWhiteSpace(gameDirectory))
			{
				return null;
			}

			try
			{
				var inputDirectory = Path.GetFullPath(
					gameDirectory.Trim().Trim('"'));

				if (!Directory.Exists(inputDirectory))
				{
					return null;
				}

				// Steam / Epic install root.
				var layout = TryCreateGameLayout(
					inputDirectory,
					Path.Combine(inputDirectory, "PAYDAY3"),
					Path.Combine(inputDirectory, DesktopGameExecutableName),
					false);

				if (layout != null)
				{
					return layout;
				}

				// Also accept the desktop PAYDAY3 content root itself.
				var parentDirectory = Directory.GetParent(inputDirectory)?.FullName;
				if (!string.IsNullOrWhiteSpace(parentDirectory))
				{
					layout = TryCreateGameLayout(
						parentDirectory,
						inputDirectory,
						Path.Combine(parentDirectory, DesktopGameExecutableName),
						false);

					if (layout != null)
					{
						return layout;
					}
				}

				// Microsoft Store / Xbox install root.
				layout = TryCreateWinGDKGameLayout(
					inputDirectory,
					Path.Combine(inputDirectory, "Content", "PAYDAY3"));

				if (layout != null)
				{
					return layout;
				}

				// Also accept <Root>/Content/PAYDAY3 directly.
				var contentParent = Directory.GetParent(inputDirectory)?.Parent?.FullName;
				if (!string.IsNullOrWhiteSpace(contentParent))
				{
					layout = TryCreateWinGDKGameLayout(
						contentParent,
						inputDirectory);

					if (layout != null)
					{
						return layout;
					}
				}

				// And accept <Root>/Content/PAYDAY3/Binaries/WinGDK.
				if (string.Equals(
						Path.GetFileName(inputDirectory),
						"WinGDK",
						StringComparison.OrdinalIgnoreCase))
				{
					var gameContentDirectory = Path.GetFullPath(
						Path.Combine(inputDirectory, "..", ".."));
					var installDirectory = Path.GetFullPath(
						Path.Combine(gameContentDirectory, "..", ".."));

					layout = TryCreateWinGDKGameLayout(
						installDirectory,
						gameContentDirectory);

					if (layout != null)
					{
						return layout;
					}
				}
			}
			catch (Exception)
			{
				return null;
			}

			return null;
		}

		private static PD3GameLayout RequirePAYDAY3GameLayout(
			string gameDirectory)
		{
			var layout = ResolvePAYDAY3GameLayout(gameDirectory);
			if (layout != null)
			{
				return layout;
			}

			throw new AutomationException(
				"Provided -GameDir is invalid. Expected either a Steam/Epic PAYDAY3.exe install or a Microsoft Store/Xbox install containing Content\\PAYDAY3\\Binaries\\WinGDK\\PAYDAY3-WinGDK-Shipping.exe. Provided: '{0}'",
				gameDirectory ?? "");
		}

		private static string NormalizeLaunchGameType(
			string launchGameType)
		{
			if (string.IsNullOrWhiteSpace(launchGameType))
			{
				// Backwards compatibility with older CrimeFactory editor builds that
				// only supplied -LaunchGame and implicitly meant Steam.
				return "Steam";
			}

			if (launchGameType.Equals(
					"Steam",
					StringComparison.OrdinalIgnoreCase))
			{
				return "Steam";
			}

			if (launchGameType.Equals(
					"MicrosoftStore",
					StringComparison.OrdinalIgnoreCase) ||
				launchGameType.Equals(
					"Xbox",
					StringComparison.OrdinalIgnoreCase))
			{
				return "MicrosoftStore";
			}

			if (launchGameType.Equals(
					"Epic",
					StringComparison.OrdinalIgnoreCase) ||
				launchGameType.Equals(
					"EpicGames",
					StringComparison.OrdinalIgnoreCase))
			{
				return "Epic";
			}

			throw new AutomationException(
				"Invalid -LaunchGameType '{0}'. Expected Steam, MicrosoftStore, or Epic.",
				launchGameType
			);
		}

		private static string FindEpicPAYDAY3LaunchURL()
		{
			var manifestsDirectory =
				Path.Combine(
					Environment.GetFolderPath(
						Environment.SpecialFolder.CommonApplicationData),
					"Epic",
					"EpicGamesLauncher",
					"Data",
					"Manifests"
				);

			if (!Directory.Exists(manifestsDirectory))
			{
				return null;
			}

			foreach (var manifestFile in
			         Directory.EnumerateFiles(
				         manifestsDirectory,
				         "*.item",
				         SearchOption.TopDirectoryOnly))
			{
				try
				{
					using var document =
						JsonDocument.Parse(
							File.ReadAllText(manifestFile));

					var root = document.RootElement;

					if (!root.TryGetProperty(
							"DisplayName",
							out var displayNameElement))
					{
						continue;
					}

					var displayName =
						displayNameElement.GetString() ?? "";

					if (!displayName.Equals(
							"PAYDAY 3",
							StringComparison.OrdinalIgnoreCase))
					{
						continue;
					}

					string catalogNamespace =
						root.TryGetProperty(
							"CatalogNamespace",
							out var namespaceElement)
							? namespaceElement.GetString()
							: null;

					string catalogItemId =
						root.TryGetProperty(
							"CatalogItemId",
							out var catalogElement)
							? catalogElement.GetString()
							: null;

					string appName =
						root.TryGetProperty(
							"AppName",
							out var appElement)
							? appElement.GetString()
							: null;

					if (!string.IsNullOrWhiteSpace(catalogNamespace) &&
					    !string.IsNullOrWhiteSpace(catalogItemId) &&
					    !string.IsNullOrWhiteSpace(appName))
					{
						return string.Format(
							"com.epicgames.launcher://apps/{0}%3A{1}%3A{2}?action=launch&silent=true",
							Uri.EscapeDataString(catalogNamespace),
							Uri.EscapeDataString(catalogItemId),
							Uri.EscapeDataString(appName)
						);
					}

					// Epic still supports the legacy Artifact/AppName-only URI.
					if (!string.IsNullOrWhiteSpace(appName))
					{
						return string.Format(
							"com.epicgames.launcher://apps/{0}?action=launch&silent=true",
							Uri.EscapeDataString(appName)
						);
					}
				}
				catch (Exception exception)
				{
					LogWarning(
						"Could not inspect Epic manifest '{0}': {1}",
						manifestFile,
						exception.Message
					);
				}
			}

			return null;
		}

		private static string FindMicrosoftStorePAYDAY3Aumid()
		{
			try
			{
				var startInfo =
					new ProcessStartInfo
					{
						FileName = "powershell.exe",
						UseShellExecute = false,
						RedirectStandardOutput = true,
						RedirectStandardError = true,
						CreateNoWindow = true
					};

				startInfo.ArgumentList.Add("-NoProfile");
				startInfo.ArgumentList.Add("-NonInteractive");
				startInfo.ArgumentList.Add("-Command");
				startInfo.ArgumentList.Add(
					"$app = Get-StartApps | " +
					"Where-Object { $_.Name -like 'PAYDAY 3*' -and $_.AppID -like '*!*' } | " +
					"Select-Object -First 1; " +
					"if ($null -ne $app) { [Console]::Out.Write($app.AppID) }"
				);

				using var process = Process.Start(startInfo);
				if (process == null)
				{
					return null;
				}

				var output = process.StandardOutput.ReadToEnd();
				var error = process.StandardError.ReadToEnd();

				process.WaitForExit(10000);

				if (process.ExitCode != 0)
				{
					LogWarning(
						"Microsoft Store/Xbox launch detection failed: {0}",
						error.Trim()
					);
					return null;
				}

				return string.IsNullOrWhiteSpace(output)
					? null
					: output.Trim();
			}
			catch (Exception exception)
			{
				LogWarning(
					"Microsoft Store/Xbox launch detection failed: {0}",
					exception.Message
				);
				return null;
			}
		}

		private static void LaunchDirectPAYDAY3Executable(
			string gameDirectory)
		{
			var layout = RequirePAYDAY3GameLayout(gameDirectory);
			var workingDirectory =
				Path.GetDirectoryName(layout.ExecutablePath) ??
				layout.InstallDirectory;

			Process.Start(
				new ProcessStartInfo(layout.ExecutablePath)
				{
					UseShellExecute = true,
					WorkingDirectory = workingDirectory
				}
			);
		}

		private static void LaunchMicrosoftStorePAYDAY3(
			string gameDirectory)
		{
			var layout = RequirePAYDAY3GameLayout(gameDirectory);
			var launchHelperPath = Path.Combine(
				layout.InstallDirectory,
				WinGDKLaunchHelperExecutableName);

			if (!File.Exists(launchHelperPath))
			{
				throw new AutomationException(
					"Microsoft Store/Xbox PAYDAY 3 launch helper was not found at '{0}'. CrimeFactory intentionally does not launch the WinGDK shipping executable directly.",
					launchHelperPath);
			}

			LogInformation(
				"Launching PAYDAY 3 through Microsoft Store/Xbox GameLaunchHelper: {0}",
				launchHelperPath);

			Process.Start(
				new ProcessStartInfo(launchHelperPath)
				{
					UseShellExecute = true,
					WorkingDirectory = layout.InstallDirectory
				}
			);
		}

		private static void LaunchPAYDAY3(
			PD3Params pd3Params)
		{
			var launchType =
				NormalizeLaunchGameType(
					pd3Params.LaunchGameType);

			if (launchType == "Steam")
			{
				Process.Start(
					new ProcessStartInfo(
						$"steam://rungameid/{SteamAppId}")
					{
						UseShellExecute = true
					}
				);

				return;
			}

			if (launchType == "Epic")
			{
				var epicLaunchURL =
					FindEpicPAYDAY3LaunchURL();

				if (!string.IsNullOrWhiteSpace(epicLaunchURL))
				{
					LogInformation(
						"Launching PAYDAY 3 through Epic Games Launcher."
					);

					Process.Start(
						new ProcessStartInfo(epicLaunchURL)
						{
							UseShellExecute = true
						}
					);

					return;
				}

				LogWarning(
					"PAYDAY 3 was not found in Epic's installed-game manifests. Falling back to the executable resolved from GameDir."
				);

				LaunchDirectPAYDAY3Executable(
					pd3Params.GameDirectory);
				return;
			}

			if (launchType == "MicrosoftStore")
			{
				LaunchMicrosoftStorePAYDAY3(
					pd3Params.GameDirectory);
				return;
			}

			throw new AutomationException(
				"Unsupported PAYDAY 3 launch type '{0}'.",
				launchType
			);
		}

		private static int GetRequestedChunkId(BuildCommand cmd)
		{
			var value = cmd.ParseOptionalStringParam("ChunkId");
			if (string.IsNullOrWhiteSpace(value))
			{
				return -1;
			}

			if (!int.TryParse(value, out int chunkId) || chunkId < 0)
			{
				throw new AutomationException(
					"Invalid -ChunkId value '{0}'. Expected an integer >= 0.",
					value
				);
			}

			return chunkId;
		}

		private static string GetPluginName(BuildCommand cmd)
		{
			var pluginName = cmd.ParseRequiredStringParam("PluginName");
			if (string.IsNullOrWhiteSpace(pluginName))
			{
				throw new AutomationException(
					"-PluginName must name a mod plugin to package"
				);
			}

			return pluginName;
		}

		private static void CopyPluginToTheGameDir(
			string gameDir,
			FileReference projectFile,
			string pluginName,
			string pluginDirectory)
		{
			// Resolve the runtime Mods directory for either:
			//   Steam/Epic:       <Root>/PAYDAY3/Mods
			//   Microsoft Store:  <Root>/Content/PAYDAY3/Mods
			var layout = RequirePAYDAY3GameLayout(gameDir);
			var modsDir = layout.ModsDirectory;
			var resultPluginDirectory = Path.Combine(modsDir, pluginName);

			if (DirectoryExists(resultPluginDirectory))
			{
				DeleteDirectory(resultPluginDirectory);
			}

			CreateDirectory(resultPluginDirectory);
			CopyDirectory_NoExceptions(pluginDirectory, resultPluginDirectory);
		}

		// --------------------------------------------------------------------
		// PASS 1: original CrimeFactory plugin DLC cook.
		//
		// This stays a DLC cook. It is used to produce the correctly structured
		// PAYDAY3/Mods/<PluginName> plugin shell, descriptor, binaries, config,
		// etc. We copy that staged plugin out before PASS 2 cleans the stage.
		// --------------------------------------------------------------------
		private static ProjectParams GetDLCParams(BuildCommand cmd)
		{
			var projectFileName = cmd.ParseRequiredStringParam("Project");
			LogInformation(projectFileName);

			var pluginName = GetPluginName(cmd);
			var projectFile = new FileReference(projectFileName);

			var basedOnReleaseVersion =
				cmd.ParseOptionalStringParam("BasedOnReleaseVersion");

			if (string.IsNullOrWhiteSpace(basedOnReleaseVersion))
			{
				basedOnReleaseVersion = DefaultBaseReleaseVersion;
			}

			var projectParameters = new ProjectParams(
				RawProjectPath: projectFile,
				Command: cmd,
				ClientTargetPlatforms: new List<TargetPlatformDescriptor>
				{
					new TargetPlatformDescriptor(UnrealTargetPlatform.Win64)
				},
				ClientConfigsToBuild: new List<UnrealTargetConfiguration>
				{
					UnrealTargetConfiguration.Shipping
				},

				// Original CrimeFactory DLC configuration.
				Cook: true,
				// Keep quoted LogCmds out of AdditionalCookerOptions. UAT appends this string
				// directly to the CookCommandlet command line, and the embedded quotes can
				// swallow -dlcname / -basedonreleaseversion / -abslog.
				AdditionalCookerOptions: "-AllowUncookedAssetReferences " +
					"-ini:Editor:[CookSettings]:CookContentMissingSeverity=Warning " +
					"-ini:Engine:[Core.Log]:LogAudiokineticTools=Fatal " +
					"-ini:Engine:[Core.Log]:LogBinkMoviePlayer=Fatal ",
				UnversionedCookedContent: true,
				SkipCookingEditorContent: true,
				DLCIncludeEngineContent: false,
				Compressed: true,
				Pak: true,
				IoStore: true,
				Stage: true,
				DLCName: pluginName,
				BasedOnReleaseVersion: basedOnReleaseVersion
			);

			projectParameters.ValidateAndLog();
			return projectParameters;
		}

		// --------------------------------------------------------------------
		// PASS 2: exact project chunk cook.
		//
		// There is intentionally NO DLCName here. The point of this second pass
		// is to allow the Asset Manager / PAL to build a chunk that can contain
		// packages from multiple mount points, including both:
		//
		//     /PDML/...
		//     /Game/...
		//
		// The resulting selected pakchunk is then copied INTO the plugin DLC
		// shell created in PASS 1.
		// --------------------------------------------------------------------
		private static List<string> ReadStringArray(
			SystemJsonObject root,
			string fieldName)
		{
			var result = new List<string>();

			SystemJsonArray values =
				root[fieldName] as SystemJsonArray;

			if (values == null)
			{
				return result;
			}

			foreach (SystemJsonNode valueNode in values)
			{
				if (valueNode == null)
				{
					continue;
				}

				string value = "";

				try
				{
					value = valueNode.GetValue<string>();
				}
				catch
				{
					value = "";
				}

				if (string.IsNullOrWhiteSpace(value))
				{
					continue;
				}

				value = value.Trim();

				if (!result.Contains(
						value,
						StringComparer.OrdinalIgnoreCase))
				{
					result.Add(value);
				}
			}

			return result;
		}

		private static CrimeFactoryPALPackagingManifest
			LoadPALPackagingManifest(
				BuildCommand cmd,
				string pluginName,
				int chunkId)
		{
			string manifestFile =
				cmd.ParseOptionalStringParam(
					"CrimeFactoryPALManifestFile");

			if (string.IsNullOrWhiteSpace(manifestFile))
			{
				throw new AutomationException(
					"Two-stage packaging requires -CrimeFactoryPALManifestFile. "
					+ "The editor did not provide the PAL's deterministic "
					+ "package list."
				);
			}

			if (!FileExists(manifestFile))
			{
				throw new AutomationException(
					"PAL packaging manifest does not exist: {0}",
					manifestFile
				);
			}

			SystemJsonNode parsed;

			try
			{
				parsed = SystemJsonNode.Parse(
					File.ReadAllText(manifestFile));
			}
			catch (Exception exception)
			{
				throw new AutomationException(
					"Could not parse PAL packaging manifest '{0}': {1}",
					manifestFile,
					exception.Message
				);
			}

			SystemJsonObject root =
				parsed as SystemJsonObject;

			if (root == null)
			{
				throw new AutomationException(
					"PAL packaging manifest '{0}' is not a JSON object.",
					manifestFile
				);
			}

			var result =
				new CrimeFactoryPALPackagingManifest
				{
					SourceFile = manifestFile,
					PluginName =
						root["PluginName"]?.GetValue<string>() ?? "",
					PAL =
						root["PAL"]?.GetValue<string>() ?? "",
					ChunkId =
						root["ChunkId"]?.GetValue<int>() ?? -1
				};

			result.ExternalCookPackages.AddRange(
				ReadStringArray(
					root,
					"ExternalCookPackages"));

			result.ExpectedChunkPackages.AddRange(
				ReadStringArray(
					root,
					"ExpectedChunkPackages"));

			SystemJsonArray additionalPakFiles =
				root["AdditionalPakFiles"] as SystemJsonArray;
			if (additionalPakFiles != null)
			{
				foreach (SystemJsonNode node in additionalPakFiles)
				{
					SystemJsonObject file = node as SystemJsonObject;
					if (file == null)
					{
						continue;
					}

					string source =
						file["Source"]?.GetValue<string>() ?? "";
					string stagedPath =
						file["StagedPath"]?.GetValue<string>() ?? "";

					if (!string.IsNullOrWhiteSpace(source) &&
						!string.IsNullOrWhiteSpace(stagedPath))
					{
						result.AdditionalPakFiles.Add(
							new CrimeFactoryAdditionalPakFile
							{
								Source = source,
								StagedPath = stagedPath
							});
					}
				}
			}

			if (!result.PluginName.Equals(
					pluginName,
					StringComparison.OrdinalIgnoreCase))
			{
				throw new AutomationException(
					"PAL packaging manifest belongs to plugin '{0}', "
					+ "but UAT is packaging '{1}'.",
					result.PluginName,
					pluginName
				);
			}

			if (result.ChunkId != chunkId)
			{
				throw new AutomationException(
					"PAL packaging manifest expects Chunk {0}, "
					+ "but UAT was asked for Chunk {1}.",
					result.ChunkId,
					chunkId
				);
			}

			if (result.ExternalCookPackages.Count == 0)
			{
				throw new AutomationException(
					"PAL packaging manifest for '{0}' has no external "
					+ "replacement packages for PASS 2.",
					pluginName
				);
			}

			if (result.ExpectedChunkPackages.Count == 0)
			{
				throw new AutomationException(
					"PAL packaging manifest for '{0}' has no expected "
					+ "Chunk {1} packages.",
					pluginName,
					chunkId
				);
			}

			return result;
		}

		private static string BuildExactPass2CookOptions(
			CrimeFactoryPALPackagingManifest manifest)
		{
			if (manifest == null ||
			    manifest.ExpectedChunkPackages.Count == 0)
			{
				return "";
			}

			var exactPackages = new List<string>();

			foreach (string packageName in
				manifest.ExpectedChunkPackages)
			{
				if (string.IsNullOrWhiteSpace(packageName) ||
				    packageName.Any(character => char.IsWhiteSpace(character) ||
				        character == '+' || character == '"' || character == '=' || character == '\0') ||
				    !packageName.StartsWith(
					    "/",
					    StringComparison.Ordinal))
				{
					throw new AutomationException(
						"Invalid expected PASS 2 package in PAL manifest: '{0}'",
						packageName ?? ""
					);
				}

				if (!exactPackages.Contains(
						packageName,
						StringComparer.OrdinalIgnoreCase))
				{
					exactPackages.Add(packageName);
				}
			}

			foreach (string externalPackage in
				manifest.ExternalCookPackages)
			{
				if (!exactPackages.Contains(
						externalPackage,
						StringComparer.OrdinalIgnoreCase))
				{
					throw new AutomationException(
						"External PASS 2 package '{0}' is missing from ExpectedChunkPackages. "
						+ "Refusing to run an ambiguous cook.",
						externalPackage
					);
				}
			}

			/*
			 * PASS 2 is a surgical replacement cook. The PAL manifest already
			 * contains the complete package set that is allowed into the chunk.
			 *
			 * -cooksinglepackagenorefs tells CookCommandlet to save only packages
			 * supplied explicitly in the startup workload. Referenced vanilla PAYDAY 3
			 * packages remain imports instead of becoming unsolicited cook targets.
			 *
			 * UE 5.5's BlockOnPrebootCookGate accepts -Package tokens through
			 * -CookWaitOnCommandFile and adds them directly to StartupOptions.CookMaps.
			 * Unlike -CmdLineFile, this never expands into FCommandLine's fixed
			 * 16,384-character buffer (or Windows' 32,767-character process limit).
			 * This file is prepared before launch and consumed/deleted by FLockFile.
			 */
			/*
			 * Do not add -CookSinglePackage here. Wwise recognizes that flag and
			 * starts its resource cooker, but UE then expands this surgical cook into
			 * hundreds of startup/engine packages. CrimeFactory packages the already
			 * generated BNK/WEM files itself, so PASS 2 only needs the exact AkEvent
			 * UObject plus the rest of the deterministic PAL package set.
			 */
			string commandDirectory = Path.Combine(
				Path.GetDirectoryName(Path.GetFullPath(manifest.SourceFile)),
				"CookInputs");
			Directory.CreateDirectory(commandDirectory);

			// This human-readable manifest remains on disk after the cook for
			// diagnostics. Every line is a validated long package name.
			string packageManifestFile = Path.Combine(commandDirectory,
				"Pass2-Chunk" + manifest.ChunkId + "-Packages.manifest");
			var utf8 = new System.Text.UTF8Encoding(false);
			File.WriteAllLines(packageManifestFile, exactPackages, utf8);
			manifest.Pass2PackageManifestFile = packageManifestFile;

			// UE 5.5's preboot cook gate consumes -Package=<LongPackageName>
			// requests from its command file. The transient command file MUST be
			// created before launching UnrealEditor-Cmd; FLockFile deletes it after
			// consuming the requests. A GUID prevents stale requests from previous
			// runs being silently reused.
			string commandFile = Path.Combine(commandDirectory,
				"Pass2-" + Guid.NewGuid().ToString("N") + ".txt");
			File.WriteAllLines(commandFile,
				exactPackages.Select(packageName => "-Package=" + packageName),
				utf8);
			manifest.Pass2CommandFile = commandFile;

			LogInformation(
				"PASS 2: wrote {0} exact packages to manifest: {1}",
				exactPackages.Count, packageManifestFile);
			LogInformation(
				"PASS 2: cook command file (preboot gate): {0}",
				commandFile);

			// No package names are added to the process command line. UAT
			// trims outer whitespace from AdditionalCookerOptions, so the
			// filename is followed by another real flag.
			return " -CookWaitOnCommandFile=" +
				CommandUtils.MakePathSafeToUseWithCommandLine(commandFile) +
				" -cooksinglepackagenorefs";
		}


		private static void EnsurePass2CookInputWasConsumed(
			CrimeFactoryPALPackagingManifest manifest)
		{
			if (manifest == null ||
			    string.IsNullOrWhiteSpace(manifest.Pass2CommandFile))
			{
				throw new AutomationException(
					"PASS 2 has no cook command file. Refusing to stage an unverified chunk.");
			}

			// The preboot gate removes the file once the cooker has read it.
			// A remaining file means that the cook command ignored the input,
			// even if it returned a success code (for example by cooking defaults).
			if (File.Exists(manifest.Pass2CommandFile))
			{
				throw new AutomationException(
					"PASS 2 cook returned without consuming its exact-package command file: {0}. "
					+ "Verify that this UE 5.5 cooker supports -CookWaitOnCommandFile; "
					+ "CrimeFactory will not package a potentially incorrect chunk. "
					+ "Requested package names are preserved in: {1}",
					manifest.Pass2CommandFile,
					manifest.Pass2PackageManifestFile);
			}

			LogInformation("PASS 2: verified cooker consumed package input ({0}).",
				manifest.Pass2PackageManifestFile);
		}

		private static string NormalizeManifestLine(
			string value)
		{
			return (value ?? "")
				.Trim()
				.Trim('"')
				.Replace('\\', '/')
				.ToLowerInvariant();
		}

		private static List<string> BuildPackageManifestMatchTokens(
			string packageName,
			string pluginName)
		{
			var tokens = new List<string>();

			string normalizedPackage =
				NormalizeManifestLine(packageName);

			if (string.IsNullOrWhiteSpace(normalizedPackage))
			{
				return tokens;
			}

			tokens.Add(normalizedPackage);

			if (normalizedPackage.StartsWith("/game/"))
			{
				string relative =
					normalizedPackage.Substring(
						"/game/".Length);

				tokens.Add(
					"/content/" +
					relative +
					".uasset");

				tokens.Add(
					"/content/" +
					relative);
			}
			else
			{
				string withoutLeadingSlash =
					normalizedPackage.TrimStart('/');

				int slashIndex =
					withoutLeadingSlash.IndexOf('/');

				string mountName =
					slashIndex >= 0
						? withoutLeadingSlash.Substring(
							0,
							slashIndex)
						: withoutLeadingSlash;

				string relative =
					slashIndex >= 0
						? withoutLeadingSlash.Substring(
							slashIndex + 1)
						: "";

				if (!string.IsNullOrWhiteSpace(mountName) &&
				    !string.IsNullOrWhiteSpace(relative))
				{
					tokens.Add(
						"/mods/" +
						mountName +
						"/content/" +
						relative +
						".uasset");

					tokens.Add(
						"/mods/" +
						mountName +
						"/content/" +
						relative);
				}
			}

			return tokens
				.Distinct(
					StringComparer.OrdinalIgnoreCase)
				.ToList();
		}

		private static bool IsManifestTokenTerminator(
			char value)
		{
			return char.IsWhiteSpace(value) ||
				value == '\"' ||
				value == '\'';
		}

		private static bool ManifestLineContainsExactToken(
			string normalizedLine,
			string token)
		{
			if (string.IsNullOrWhiteSpace(normalizedLine) ||
			    string.IsNullOrWhiteSpace(token))
			{
				return false;
			}

			/*
			 * Chunk manifests contain cooked filenames rather than just long
			 * package names, so a token may legitimately be followed by a
			 * cooked package extension. It must NOT, however, match another
			 * package whose name merely starts with this package name.
			 *
			 * Example:
			 *   WBP_MenuRoot.uasset          -> match
			 *   WBP_MenuRoot.uexp            -> match
			 *   WBP_MenuRoot_Button.uasset   -> do not match
			 */
			string[] cookedPackageExtensions =
			{
				".uasset",
				".umap",
				".uexp",
				".ubulk",
				".uptnl",
				".m.ubulk"
			};

			int searchStart = 0;

			while (searchStart < normalizedLine.Length)
			{
				int tokenIndex = normalizedLine.IndexOf(
					token,
					searchStart,
					StringComparison.Ordinal);

				if (tokenIndex < 0)
				{
					return false;
				}

				int tokenEnd = tokenIndex + token.Length;

				if (tokenEnd == normalizedLine.Length ||
				    IsManifestTokenTerminator(normalizedLine[tokenEnd]))
				{
					return true;
				}

				if (!token.EndsWith(
						".uasset",
						StringComparison.OrdinalIgnoreCase))
				{
					foreach (string extension in cookedPackageExtensions)
					{
						if (!normalizedLine.Substring(tokenEnd).StartsWith(
								extension,
								StringComparison.Ordinal))
						{
							continue;
						}

						int extensionEnd = tokenEnd + extension.Length;

						if (extensionEnd == normalizedLine.Length ||
						    IsManifestTokenTerminator(
							    normalizedLine[extensionEnd]))
						{
							return true;
						}
					}
				}

				searchStart = tokenIndex + 1;
			}

			return false;
		}

		private static bool ManifestLineMatchesPackage(
			string manifestLine,
			string packageName,
			string pluginName)
		{
			string normalizedLine =
				NormalizeManifestLine(manifestLine);

			foreach (string token in
				BuildPackageManifestMatchTokens(
					packageName,
					pluginName))
			{
				if (ManifestLineContainsExactToken(
						normalizedLine,
						token))
				{
					return true;
				}
			}

			return false;
		}

		private static string GetChunkManifestDirectory(
			ProjectParams chunkParams)
		{
			string projectDirectory =
				Path.GetDirectoryName(
					chunkParams.RawProjectPath.ToString());

			string projectName =
				chunkParams.RawProjectPath
					.GetFileNameWithoutAnyExtensions();

			return Path.Combine(
				projectDirectory,
				"Saved",
				"Cooked",
				"Windows",
				projectName,
				"Metadata",
				"ChunkManifest"
			);
		}

		// These are SavePackage2's explicit non-runtime results, not generic
		// missing-package warnings, load failures or NeverCook policy rejections.
		private static string GetEditorOnlyCookPath(string line)
		{
			string[] prefixes = {
				"LogSavePackage: Verbose: No exports found (or all exports are editor-only) for ",
				"LogSavePackage: Verbose: Package marked as editor-only: "
			};
			const string suffix = ". Package will not be saved.";
			foreach (string prefix in prefixes)
			{
				int start = line.IndexOf(prefix, StringComparison.Ordinal);
				if (start >= 0 && line.EndsWith(suffix, StringComparison.Ordinal))
					return line.Substring(start + prefix.Length,
						line.Length - start - prefix.Length - suffix.Length);
			}
			return null;
		}

		private static string FindExactPass2CookLog(ProjectParams chunkParams)
		{
			string inputId = System.Text.RegularExpressions.Regex.Match(
				chunkParams.AdditionalCookerOptions, @"Pass2-[a-f0-9]{32}\.txt").Value;
			string logFolder = Environment.GetEnvironmentVariable("uebp_LogFolder");
			if (!string.IsNullOrEmpty(inputId) && Directory.Exists(logFolder))
			{
				foreach (string path in Directory.GetFiles(logFolder, "Cook*.txt")
					.OrderByDescending(File.GetLastWriteTimeUtc))
					if (File.ReadAllText(path).Contains(inputId, StringComparison.Ordinal))
						return path;
			}
			throw new AutomationException("Cannot find the current PASS 2 cook log for {0}. Refusing to infer runtime exclusions.", inputId);
		}

		private static void EnsureExpectedPackagesInChunkManifest(
			ProjectParams chunkParams,
			CrimeFactoryPALPackagingManifest manifest,
			string pluginName,
			int chunkId)
		{
			string cookLog = FindExactPass2CookLog(chunkParams);
			var editorOnlyPaths = File.ReadLines(cookLog).Select(GetEditorOnlyCookPath)
				.Where(path => path != null).ToList();
			var editorOnlyPackages = new List<string>();
			int requestedCount = manifest.ExpectedChunkPackages.Count;
			string manifestDirectory =
				GetChunkManifestDirectory(chunkParams);

			if (!Directory.Exists(manifestDirectory))
			{
				throw new AutomationException(
					"PASS 2 cook completed but the ChunkManifest directory "
					+ "was not created: {0}",
					manifestDirectory
				);
			}

			string selectedManifest =
				Path.Combine(
					manifestDirectory,
					$"pakchunk{chunkId}.txt");

			var allManifestFiles =
				Directory.GetFiles(
					manifestDirectory,
					"pakchunk*.txt",
					SearchOption.TopDirectoryOnly)
				.Where(
					file =>
						!Path.GetFileName(file).Equals(
							"pakchunklist.txt",
							StringComparison.OrdinalIgnoreCase))
				.ToArray();

			if (allManifestFiles.Length == 0)
			{
				throw new AutomationException(
					"PASS 2 cook generated no pakchunk manifests in: {0}",
					manifestDirectory
				);
			}

			var manifestLines =
				new Dictionary<string, List<string>>(
					StringComparer.OrdinalIgnoreCase);

			foreach (string manifestFile in allManifestFiles)
			{
				manifestLines[manifestFile] =
					File.ReadAllLines(manifestFile)
						.Where(
							line =>
								!string.IsNullOrWhiteSpace(line))
						.ToList();
			}

			List<string> originalSelectedLines;

			if (manifestLines.TryGetValue(
					selectedManifest,
					out List<string> existingSelectedLines))
			{
				originalSelectedLines =
					new List<string>(existingSelectedLines);
			}
			else
			{
				originalSelectedLines =
					new List<string>();
			}

			/*
			 * PASS 2 now uses -cooksinglepackagenorefs with the PAL's complete
			 * deterministic package list, so dependency packages should never be
			 * cooked in the first place. Keep this exact-manifest rebuild anyway as
			 * a second safety boundary: even if a future cooker/chunking change adds
			 * an unsolicited entry, it cannot leak into the final replacement chunk.
			 */
			var exactSelectedLines =
				new List<string>();

			var missingFromCook =
				new List<string>();

			foreach (string expectedPackage in
				manifest.ExpectedChunkPackages)
			{
				var packageLines =
					new List<string>();

				foreach (var pair in manifestLines)
				{
					foreach (string line in pair.Value)
					{
						if (ManifestLineMatchesPackage(
								line,
								expectedPackage,
								pluginName))
						{
							packageLines.Add(line);
						}
					}
				}

				packageLines =
					packageLines
						.Distinct(
							StringComparer.OrdinalIgnoreCase)
						.ToList();

				if (packageLines.Count == 0)
				{
					if (!manifest.ExternalCookPackages.Contains(expectedPackage, StringComparer.OrdinalIgnoreCase) &&
						editorOnlyPaths.Any(path => ManifestLineMatchesPackage(path, expectedPackage, pluginName)))
					{
						editorOnlyPackages.Add(expectedPackage);
						LogInformation("PASS 2: cooker explicitly excluded non-runtime package: {0}", expectedPackage);
						continue;
					}
					missingFromCook.Add(
						expectedPackage);

					continue;
				}

				exactSelectedLines.AddRange(
					packageLines);

				LogInformation(
					"PASS 2: exact Chunk {0} package: {1} ({2} cooked manifest entr{3})",
					chunkId,
					expectedPackage,
					packageLines.Count,
					packageLines.Count == 1 ? "y" : "ies"
				);
			}

			// Preserve the original PAL JSON and record every runtime decision separately.
			File.WriteAllText(manifest.SourceFile + ".RuntimeAudit.json", JsonSerializer.Serialize(new {
				PluginName = pluginName, CookLog = cookLog, RequestedCount = requestedCount,
				EditorOnlyPackages = editorOnlyPackages, MissingPackages = missingFromCook,
				CookedPackages = manifest.ExpectedChunkPackages.Except(editorOnlyPackages, StringComparer.OrdinalIgnoreCase)
					.Except(missingFromCook, StringComparer.OrdinalIgnoreCase).ToList()
			}, new JsonSerializerOptions { WriteIndented = true }));

			if (missingFromCook.Count > 0)
			{
				throw new AutomationException(
					"PASS 2 did not cook {0} PAL-managed package(s):\n  {1}\n"
					+ "CrimeFactory refuses to build an incomplete replacement mod.",
					missingFromCook.Count,
					string.Join(
						"\n  ",
						missingFromCook)
				);
			}
			manifest.ExpectedChunkPackages.RemoveAll(package => editorOnlyPackages.Contains(package, StringComparer.OrdinalIgnoreCase));

			exactSelectedLines =
				exactSelectedLines
					.Distinct(
						StringComparer.OrdinalIgnoreCase)
					.ToList();

			int removedDependencyEntries =
				originalSelectedLines.Count(
					line =>
						!exactSelectedLines.Contains(
							line,
							StringComparer.OrdinalIgnoreCase));

			Directory.CreateDirectory(
				Path.GetDirectoryName(selectedManifest));

			/*
			 * Replace, do not append. pakchunk<id>.txt must contain ONLY packages
			 * explicitly represented by the PAL inspector's managed candidates.
			 */
			File.WriteAllLines(
				selectedManifest,
				exactSelectedLines);

			List<string> verifiedLines =
				File.ReadAllLines(selectedManifest)
					.Where(
						line =>
							!string.IsNullOrWhiteSpace(line))
					.ToList();

			var stillMissing =
				manifest.ExpectedChunkPackages
					.Where(
						expectedPackage =>
							!verifiedLines.Any(
								line =>
									ManifestLineMatchesPackage(
										line,
										expectedPackage,
										pluginName)))
					.ToList();

			if (stillMissing.Count > 0)
			{
				throw new AutomationException(
					"Chunk {0} verification failed. Missing {1} expected "
					+ "package(s):\n  {2}",
					chunkId,
					stillMissing.Count,
					string.Join(
						"\n  ",
						stillMissing)
				);
			}

			/*
			 * Also verify the opposite direction: every line left in the
			 * selected chunk must belong to one of the PAL's expected packages.
			 * This prevents a future cooker/chunking change from silently
			 * reintroducing dependency packages.
			 */
			var unexpectedLines =
				verifiedLines
					.Where(
						line =>
							!manifest.ExpectedChunkPackages.Any(
								expectedPackage =>
									ManifestLineMatchesPackage(
										line,
										expectedPackage,
										pluginName)))
					.ToList();

			if (unexpectedLines.Count > 0)
			{
				throw new AutomationException(
					"Chunk {0} exact-output verification failed. "
					+ "{1} unexpected manifest entr{2} remain:\n  {3}",
					chunkId,
					unexpectedLines.Count,
					unexpectedLines.Count == 1 ? "y" : "ies",
					string.Join(
						"\n  ",
						unexpectedLines)
				);
			}

			LogInformation(
				"PASS 2: verified exact Chunk {0}: {1}/{1} PAL package(s), "
				+ "{2} manifest entr{3}, {4} dependency entr{5} removed.",
				chunkId,
				manifest.ExpectedChunkPackages.Count,
				verifiedLines.Count,
				verifiedLines.Count == 1 ? "y" : "ies",
				removedDependencyEntries,
				removedDependencyEntries == 1 ? "y" : "ies"
			);
		}

		private static ProjectParams GetChunkParams(
			BuildCommand cmd,
			CrimeFactoryPALPackagingManifest palManifest)
		{
			var projectFileName = cmd.ParseRequiredStringParam("Project");
			var projectFile = new FileReference(projectFileName);

			string additionalCookerOptions =
				"-AllowUncookedAssetReferences "
				+ "-ini:Editor:[CookSettings]:CookContentMissingSeverity=Warning "
				+ "-ini:Engine:[Core.Log]:LogAudiokineticTools=Fatal "
				+ "-ini:Engine:[Core.Log]:LogBinkMoviePlayer=Fatal "
				+ "-ini:Engine:[Core.Log]:LogSavePackage=Verbose "

				+ BuildExactPass2CookOptions(palManifest);

			LogInformation(
				"PASS 2: exact-package cook enabled. {0} package(s) may be cooked; references outside this set remain uncooked imports.",
				palManifest.ExpectedChunkPackages.Count
			);

			foreach (string packageName in palManifest.ExpectedChunkPackages)
			{
				LogInformation(
					"PASS 2 exact package: {0}",
					packageName
				);
			}

			var projectParameters = new ProjectParams(
				RawProjectPath: projectFile,
				Command: cmd,
				ClientTargetPlatforms: new List<TargetPlatformDescriptor>
				{
					new TargetPlatformDescriptor(UnrealTargetPlatform.Win64)
				},
				ClientConfigsToBuild: new List<UnrealTargetConfiguration>
				{
					UnrealTargetConfiguration.Shipping
				},

				Build: true,
				SkipBuildEditor: true,
				Cook: true,
				AdditionalCookerOptions: additionalCookerOptions,
				UnversionedCookedContent: true,
				SkipCookingEditorContent: true,
				Compressed: true,
				Pak: true,
				IoStore: true,
				Stage: true,

				// Required for Asset Manager chunk manifests.
				Manifests: true
			);

			projectParameters.ValidateAndLog();
			return projectParameters;
		}

		private static void TryUpdateModulesFile(
			string filePath,
			string targetBuildId)
		{
			try
			{
				var modulesObject =
					JsonObject.Read(new FileReference(filePath));
				var modulesSubObject =
					modulesObject.GetObjectField("Modules");

				using (var writer = new JsonWriter(filePath))
				{
					writer.WriteObjectStart();
					writer.WriteValue("BuildId", targetBuildId);

					writer.WriteObjectStart("Modules");
					foreach (var moduleName in modulesSubObject.KeyNames)
					{
						var modulePath =
							modulesSubObject.GetStringField(moduleName);
						writer.WriteValue(moduleName, modulePath);
					}

					writer.WriteObjectEnd();
					writer.WriteObjectEnd();
				}
			}
			catch (Exception ex)
			{
				throw new AutomationException(
					"Failed to update modules file '{0}': {1}",
					filePath,
					ex.Message
				);
			}
		}

		private static void UpdateModulesBuildId(
			string stagingDir,
			string targetBuildId)
		{
			var foundFiles = FindFiles("*.modules", true, stagingDir);

			foreach (var modulesFile in foundFiles)
			{
				TryUpdateModulesFile(modulesFile, targetBuildId);
			}
		}

		private static IReadOnlyList<DeploymentContext>
			CreateDeploymentContexts(ProjectParams projectParams)
		{
			var contexts = new List<DeploymentContext>();

			if (!projectParams.NoClient)
			{
				contexts.AddRange(
					Project.CreateDeploymentContext(projectParams, false)
				);
			}

			if (projectParams.DedicatedServer)
			{
				contexts.AddRange(
					Project.CreateDeploymentContext(projectParams, true)
				);
			}

			return contexts;
		}

		private static void RemapCookedPluginsContentPaths(
			ProjectParams projectParams,
			IEnumerable<DeploymentContext> deploymentContexts)
		{
			foreach (var deploymentContext in deploymentContexts)
			{
				// Keep the original CrimeFactory remap. PASS 1 is still a real
				// plugin DLC and therefore still needs its runtime path moved to:
				//
				// ../../../PAYDAY3/Mods/<PluginName>
				var projectName =
					projectParams.RawProjectPath
						.GetFileNameWithoutAnyExtensions();

				string dlcSourceDirectory;

				if (projectParams.DLCFile.IsUnderDirectory(
					deploymentContext.EngineRoot))
				{
					dlcSourceDirectory = Path.Combine(
						"Engine",
						projectParams.DLCFile.Directory.ParentDirectory
							.MakeRelativeTo(deploymentContext.EngineRoot)
					);
				}
				else if (projectParams.DLCFile.IsUnderDirectory(
					deploymentContext.ProjectRoot))
				{
					dlcSourceDirectory = Path.Combine(
						projectName,
						projectParams.DLCFile.Directory.ParentDirectory
							.MakeRelativeTo(deploymentContext.ProjectRoot)
					);
				}
				else
				{
					throw new Exception(
						"Unknown DLC remap for DLC " +
						projectParams.DLCFile.GetFileNameWithoutExtension()
					);
				}

				var destinationModsDir =
					Path.Combine(projectName, ModDeployRoot);

				deploymentContext.RemapDirectories.Add(
					Tuple.Create(
						new StagedDirectoryReference(dlcSourceDirectory),
						new StagedDirectoryReference(destinationModsDir)
					)
				);
			}
		}

		private static void StageAdditionalPakFiles(
			DeploymentContext deploymentContext,
			IEnumerable<CrimeFactoryAdditionalPakFile> files)
		{
			if (files == null)
			{
				return;
			}

			var stagedPaths = new HashSet<string>(
				StringComparer.OrdinalIgnoreCase);

			foreach (CrimeFactoryAdditionalPakFile file in files)
			{
				if (file == null ||
					string.IsNullOrWhiteSpace(file.Source) ||
					string.IsNullOrWhiteSpace(file.StagedPath))
				{
					continue;
				}

				string source = Path.GetFullPath(file.Source);
				string stagedPath = file.StagedPath
					.Replace('\\', '/')
					.TrimStart('/');

				if (Path.IsPathRooted(stagedPath) ||
					stagedPath.Split('/').Any(part => part == ".."))
				{
					throw new AutomationException(
						"CrimeFactory refused invalid additional pak path: {0}",
						file.StagedPath);
				}

				if (!FileExists(source))
				{
					throw new AutomationException(
						"Wwise resource selected for packaging no longer exists: {0}",
						source);
				}

				if (!stagedPaths.Add(stagedPath))
				{
					continue;
				}

				deploymentContext.StageFile(
					StagedFileType.UFS,
					new FileReference(source),
					new StagedFileReference(stagedPath));

				LogInformation(
					"Wwise: staging into main mod pak: {0} -> {1}",
					source,
					stagedPath);
			}
		}

		internal static void CopyBuildToStagingDirectory(
			ProjectParams Params,
			IReadOnlyList<DeploymentContext> deploymentContexts,
			IEnumerable<CrimeFactoryAdditionalPakFile> additionalPakFiles = null,
			FileReference activeModDescriptor = null)
		{
			var platformCares =
				Params.ClientTargetPlatformInstances[0].RequiresPak(Params);

			var requiresPak =
				platformCares == Platform.PakType.Always ||
				(Params.Pak && platformCares != Platform.PakType.Never);

			if (requiresPak || (Params.Stage && !Params.SkipStage))
			{
				LogInformation(
					"********** STAGE COMMAND STARTED **********"
				);

				foreach (var sc in deploymentContexts)
				{
					CrimeFactoryPluginStaging.Validate(
						Params.RawProjectPath, sc, activeModDescriptor ?? Params.DLCFile);
					Project.CreateStagingManifest(Params, sc);
					StageAdditionalPakFiles(sc, additionalPakFiles);
					Project.CleanStagingDirectory(Params, sc);
				}

				foreach (var sc in deploymentContexts)
				{
					Project.ApplyStagingManifest(Params, sc);
				}

				LogInformation(
					"********** STAGE COMMAND COMPLETED **********"
				);
			}
		}

		private static string GetPluginPathRelativeToStageRoot(
			ProjectParams projectParams)
		{
			var projectName =
				projectParams.RawProjectPath
					.GetFileNameWithoutAnyExtensions();

			var dlcName =
				projectParams.DLCFile.GetFileNameWithoutAnyExtensions();

			return Path.Combine(
				projectName,
				ModDeployRoot,
				dlcName
			);
		}

		private static string GetWorkingPluginDirectory(
			ProjectParams projectParams,
			string pluginName)
		{
			var projectDirectory =
				Path.GetDirectoryName(
					projectParams.RawProjectPath.ToString()
				);

			return Path.Combine(
				projectDirectory,
				"Saved",
				"CrimeFactory",
				"TwoStage",
				pluginName
			);
		}

		private static string CopyDLCPluginToWorkingDirectory(
			ProjectParams dlcParams,
			IEnumerable<DeploymentContext> deploymentContexts,
			string pluginName)
		{
			var workingPluginDirectory =
				GetWorkingPluginDirectory(dlcParams, pluginName);

			if (DirectoryExists(workingPluginDirectory))
			{
				DeleteDirectory(workingPluginDirectory);
			}

			DeploymentContext sourceContext = null;
			foreach (var context in deploymentContexts)
			{
				if (context.FinalCookPlatform == "Windows")
				{
					sourceContext = context;
					break;
				}

				if (sourceContext == null)
				{
					sourceContext = context;
				}
			}

			if (sourceContext == null)
			{
				throw new AutomationException(
					"No deployment context was created for the DLC pass."
				);
			}

			var stagedPluginDirectory = Path.Combine(
				sourceContext.StageDirectory.ToString(),
				GetPluginPathRelativeToStageRoot(dlcParams)
			);

			if (!DirectoryExists(stagedPluginDirectory))
			{
				throw new AutomationException(
					"PASS 1 completed but the staged plugin directory " +
					"was not found: {0}",
					stagedPluginDirectory
				);
			}

			CreateDirectory(workingPluginDirectory);
			CopyDirectory_NoExceptions(
				stagedPluginDirectory,
				workingPluginDirectory
			);

			LogInformation(
				"PASS 1: copied plugin DLC shell to working directory: {0}",
				workingPluginDirectory
			);

			return workingPluginDirectory;
		}

		private static bool IsContainerFile(string filePath)
		{
			var extension =
				Path.GetExtension(filePath).ToLowerInvariant();

			return extension == ".pak" ||
			       extension == ".utoc" ||
			       extension == ".ucas" ||
			       extension == ".sig";
		}

		private static string GetPass1ContainerBaseRelativePath(
			string workingPluginDirectory)
		{
			/*
			 * PASS 1's plugin DLC normally produces something like:
			 *
			 *     Content/Paks/Windows/PDMLPAYDAY3-Windows.pak
			 *     Content/Paks/Windows/PDMLPAYDAY3-Windows.utoc
			 *     Content/Paks/Windows/PDMLPAYDAY3-Windows.ucas
			 *
			 * Return the path relative to Content/Paks WITHOUT the extension:
			 *
			 *     Windows/PDML-Windows
			 *
			 * PASS 2's IoStore files will be copied back to this exact
			 * directory/base name.
			 */
			var paksDirectory = Path.Combine(
				workingPluginDirectory,
				"Content",
				"Paks"
			);

			if (!DirectoryExists(paksDirectory))
			{
				return null;
			}

			// Prefer the UTOC because it is the file we are specifically
			// replacing with the PASS 2 IoStore container.
			string[] preferredExtensions =
			{
				".utoc",
				".ucas",
				".pak"
			};

			foreach (var preferredExtension in preferredExtensions)
			{
				foreach (var file in Directory.GetFiles(
					paksDirectory,
					"*",
					SearchOption.AllDirectories))
				{
					if (!Path.GetExtension(file).Equals(
						preferredExtension,
						StringComparison.OrdinalIgnoreCase))
					{
						continue;
					}

					var relativeFile =
						Path.GetRelativePath(paksDirectory, file);

					var relativeDirectory =
						Path.GetDirectoryName(relativeFile) ?? "";

					var baseName =
						Path.GetFileNameWithoutExtension(relativeFile);

					var relativeBase = string.IsNullOrWhiteSpace(
						relativeDirectory)
						? baseName
						: Path.Combine(relativeDirectory, baseName);

					LogInformation(
						"PASS 1 plugin container base captured: {0}",
						relativeBase
					);

					return relativeBase;
				}
			}

			return null;
		}

		private static string GetPatchContainerBaseName(string baseName)
		{
			if (string.IsNullOrWhiteSpace(baseName))
			{
				throw new AutomationException(
					"Cannot build a _P container name from an empty base name."
				);
			}

			// Normalise any accidental repeated suffixes first, then append one.
			while (baseName.EndsWith(
				"_P",
				StringComparison.OrdinalIgnoreCase))
			{
				baseName = baseName.Substring(0, baseName.Length - 2);
			}

			return baseName + "_P";
		}

		private static string GetPatchContainerFileName(
			string filePath,
			string pluginName)
		{
			var baseName = Path.GetFileNameWithoutExtension(filePath);
			var extension = Path.GetExtension(filePath);

			// IoStore partition files use names such as
			// FooPAYDAY3-Windows_s1.ucas. Preserve only the partition suffix;
			// the final public mod filename is always <PluginName>_P.
			string partitionSuffix = "";
			var partitionIndex = baseName.LastIndexOf(
				"_s",
				StringComparison.OrdinalIgnoreCase);

			if (partitionIndex >= 0 && partitionIndex + 2 < baseName.Length)
			{
				bool allDigits = true;
				for (int index = partitionIndex + 2; index < baseName.Length; index++)
				{
					if (!char.IsDigit(baseName[index]))
					{
						allDigits = false;
						break;
					}
				}

				if (allDigits)
				{
					partitionSuffix = baseName.Substring(partitionIndex);
				}
			}

			return
				GetPatchContainerBaseName(pluginName) +
				partitionSuffix +
				extension;
		}

		private static void ApplyPatchSuffixToPluginContainers(
			string pluginDirectory,
			string pluginName)
		{
			var paksDirectory = Path.Combine(
				pluginDirectory,
				"Content",
				"Paks"
			);

			if (!DirectoryExists(paksDirectory))
			{
				return;
			}

			foreach (var file in Directory.GetFiles(
				paksDirectory,
				"*",
				SearchOption.AllDirectories))
			{
				var extension = Path.GetExtension(file).ToLowerInvariant();
				if (extension != ".pak" &&
				    extension != ".utoc" &&
				    extension != ".ucas" &&
				    extension != ".sig")
				{
					continue;
				}

				var desiredName =
					GetPatchContainerFileName(
						file,
						pluginName
					);
				var currentName = Path.GetFileName(file);

				if (string.Equals(
					currentName,
					desiredName,
					StringComparison.OrdinalIgnoreCase))
				{
					continue;
				}

				var destination = Path.Combine(
					Path.GetDirectoryName(file),
					desiredName
				);

				if (FileExists(destination))
				{
					DeleteFile(destination);
				}

				File.Move(file, destination);

				LogInformation(
					"Applied CrimeFactory container name: {0} -> {1}",
					currentName,
					desiredName
				);
			}
		}

		private static void RenamePass1PakFilesToPatchSuffix(
			string workingPluginDirectory,
			string relativeContainerBase,
			string targetBaseName)
		{
			var paksDirectory = Path.Combine(
				workingPluginDirectory,
				"Content",
				"Paks"
			);

			var relativeDirectory =
				Path.GetDirectoryName(relativeContainerBase) ?? "";

			var directory = string.IsNullOrWhiteSpace(relativeDirectory)
				? paksDirectory
				: Path.Combine(paksDirectory, relativeDirectory);

			if (!DirectoryExists(directory))
			{
				return;
			}

			var originalBaseName =
				Path.GetFileName(relativeContainerBase);

			foreach (var extension in new[] { ".pak", ".sig" })
			{
				var sourceFile = Path.Combine(
					directory,
					originalBaseName + extension
				);

				if (!FileExists(sourceFile))
				{
					continue;
				}

				var destinationFile = Path.Combine(
					directory,
					targetBaseName + extension
				);

				if (FileExists(destinationFile))
				{
					DeleteFile(destinationFile);
				}

				File.Move(sourceFile, destinationFile);

				LogInformation(
					"Renamed PASS 1 plugin file: {0} -> {1}",
					Path.GetFileName(sourceFile),
					Path.GetFileName(destinationFile)
				);
			}
		}

		private static void RemovePass1IoStoreContainers(
			string workingPluginDirectory)
		{
			/*
			 * IMPORTANT:
			 *
			 * Keep PASS 1's .pak and .sig.
			 *
			 * In an IoStore plugin DLC the .pak contains the normal plugin
			 * staging payload such as AssetRegistry.bin and config/ini files.
			 * Replacing it with PASS 2's project chunk .pak loses that plugin
			 * metadata.
			 *
			 * Only the PASS 1 IoStore package data (.utoc/.ucas) is replaced.
			 */
			var paksDirectory = Path.Combine(
				workingPluginDirectory,
				"Content",
				"Paks"
			);

			if (!DirectoryExists(paksDirectory))
			{
				return;
			}

			foreach (var file in Directory.GetFiles(
				paksDirectory,
				"*",
				SearchOption.AllDirectories))
			{
				var extension =
					Path.GetExtension(file).ToLowerInvariant();

				if (extension != ".utoc" &&
				    extension != ".ucas")
				{
					continue;
				}

				LogInformation(
					"Removing PASS 1 IoStore container before replacement: {0}",
					file
				);

				File.Delete(file);
			}
		}

		private static List<string> FindChunkContainerFiles(
			IEnumerable<DeploymentContext> deploymentContexts,
			int chunkId)
		{
			var result = new List<string>();
			var prefix = "pakchunk" + chunkId + "-";

			foreach (var context in deploymentContexts)
			{
				var stageRoot = context.StageDirectory.ToString();

				if (!DirectoryExists(stageRoot))
				{
					continue;
				}

				foreach (var file in Directory.GetFiles(
					stageRoot,
					"*",
					SearchOption.AllDirectories))
				{
					var extension =
						Path.GetExtension(file).ToLowerInvariant();

					// PASS 2 contributes ONLY its IoStore package data.
					// PASS 1's plugin .pak is intentionally preserved.
					if (extension != ".utoc" &&
					    extension != ".ucas")
					{
						continue;
					}

					var fileName = Path.GetFileName(file);
					if (!fileName.StartsWith(
						prefix,
						StringComparison.OrdinalIgnoreCase))
					{
						continue;
					}

					bool alreadyAdded = false;
					foreach (var existing in result)
					{
						if (string.Equals(
							Path.GetFileName(existing),
							fileName,
							StringComparison.OrdinalIgnoreCase))
						{
							alreadyAdded = true;
							break;
						}
					}

					if (!alreadyAdded)
					{
						result.Add(file);
					}
				}
			}

			return result;
		}

		private static string GetRenamedChunkFileName(
			string sourceFile,
			int chunkId,
			string targetBaseName)
		{
			/*
			 * Examples:
			 *
			 * pakchunk9999-Windows.utoc
			 *     -> PDML-Windows.utoc
			 *
			 * pakchunk9999-Windows.ucas
			 *     -> PDML-Windows.ucas
			 *
			 * pakchunk9999-Windows_s1.ucas
			 *     -> PDML-Windows_s1.ucas
			 *
			 * Keeping the suffix makes this work if IoStore creates partitioned
			 * UCAS files.
			 */
			var sourceName = Path.GetFileNameWithoutExtension(sourceFile);
			var extension = Path.GetExtension(sourceFile);

			var sourceBasePrefix =
				"pakchunk" + chunkId + "-Windows";

			string suffix = "";

			if (sourceName.StartsWith(
				sourceBasePrefix,
				StringComparison.OrdinalIgnoreCase))
			{
				suffix = sourceName.Substring(sourceBasePrefix.Length);
			}
			else
			{
				// Fallback for a platform naming variation. Keep any text after
				// the first platform separator if possible.
				var dashIndex = sourceName.IndexOf('-');
				if (dashIndex >= 0)
				{
					var secondDashIndex =
						sourceName.IndexOf('-', dashIndex + 1);

					if (secondDashIndex >= 0)
					{
						suffix =
							sourceName.Substring(secondDashIndex + 1);
					}
				}
			}

			return targetBaseName + suffix + extension;
		}

		private static void InjectChunkIntoPluginDLC(
			string workingPluginDirectory,
			IEnumerable<DeploymentContext> chunkContexts,
			int chunkId,
			string pluginName)
		{
			var chunkFiles =
				FindChunkContainerFiles(chunkContexts, chunkId);

			if (chunkFiles.Count == 0)
			{
				throw new AutomationException(
					"PASS 2 did not produce any .utoc/.ucas files for " +
					"Chunk {0}. Check that Generate Chunks is enabled and " +
					"that the PAL is registered by Asset Manager.",
					chunkId
				);
			}

			bool hasUtoc = false;
			bool hasUcas = false;

			foreach (var file in chunkFiles)
			{
				var extension =
					Path.GetExtension(file).ToLowerInvariant();

				if (extension == ".utoc")
					hasUtoc = true;
				if (extension == ".ucas")
					hasUcas = true;
			}

			if (!hasUtoc || !hasUcas)
			{
				throw new AutomationException(
					"Chunk {0} was found, but its IoStore pair was incomplete. " +
					"Found UTOC={1}, UCAS={2}.",
					chunkId,
					hasUtoc,
					hasUcas
				);
			}

			var paksDirectory = Path.Combine(
				workingPluginDirectory,
				"Content",
				"Paks"
			);

			CreateDirectory(paksDirectory);

			/*
			 * Reuse PASS 1's exact relative location/base name.
			 *
			 * If PASS 1 was:
			 *
			 *     Content/Paks/Windows/PDMLPAYDAY3-Windows.utoc
			 *
			 * PASS 2 keeps the same directory, but the final public container
			 * basename is normalized to PDML_P.
			 */
			var relativeContainerBase =
				GetPass1ContainerBaseRelativePath(
					workingPluginDirectory
				);

			if (string.IsNullOrWhiteSpace(relativeContainerBase))
			{
				relativeContainerBase = Path.Combine(
					"Windows",
					pluginName + "-Windows"
				);

				LogWarning(
					"Could not find PASS 1 container naming. Falling back to: {0}",
					relativeContainerBase
				);
			}

			var targetRelativeDirectory =
				Path.GetDirectoryName(relativeContainerBase) ?? "";

			var originalBaseName =
				Path.GetFileName(relativeContainerBase);

			var targetBaseName =
				GetPatchContainerBaseName(pluginName);

			LogInformation(
				"Final CrimeFactory container base name: {0} -> {1}",
				originalBaseName,
				targetBaseName
			);

			var targetDirectory = string.IsNullOrWhiteSpace(
				targetRelativeDirectory)
				? paksDirectory
				: Path.Combine(
					paksDirectory,
					targetRelativeDirectory
				);

			CreateDirectory(targetDirectory);

			// Keep the preserved PASS 1 plugin .pak/.sig on the same patch-style
			// base name as the replacement .utoc/.ucas.
			RenamePass1PakFilesToPatchSuffix(
				workingPluginDirectory,
				relativeContainerBase,
				targetBaseName
			);

			/*
			 * Keep PASS 1:
			 *
			 *     .pak  -> AssetRegistry.bin, ini/config payload, etc.
			 *     .sig  -> matching PASS 1 pak signature if present.
			 *
			 * Replace only:
			 *
			 *     .utoc
			 *     .ucas
			 */
			RemovePass1IoStoreContainers(
				workingPluginDirectory
			);

			foreach (var sourceFile in chunkFiles)
			{
				var replacementName =
					GetRenamedChunkFileName(
						sourceFile,
						chunkId,
						targetBaseName
					);

				var destinationFile = Path.Combine(
					targetDirectory,
					replacementName
				);

				File.Copy(
					sourceFile,
					destinationFile,
					true
				);

				LogInformation(
					"PASS 2: replaced plugin DLC IoStore container with " +
					"Chunk {0}: {1} -> {2}",
					chunkId,
					Path.GetFileName(sourceFile),
					Path.GetRelativePath(
						paksDirectory,
						destinationFile
					)
				);
			}

			// Final safety pass: regardless of what PASS 1/PASS 2 called the files,
			// force the deployed/archive container set to use exactly one _P suffix.
			ForceFinalPatchContainerNames(
				targetDirectory,
				originalBaseName,
				targetBaseName,
				chunkId
			);

			LogInformation(
				"PASS 1 plugin .pak preserved so AssetRegistry.bin and " +
				"plugin ini/config files remain in the final mod."
			);
		}

		private static void ForceFinalPatchContainerNames(
			string targetDirectory,
			string originalBaseName,
			string targetBaseName,
			int chunkId)
		{
			if (!DirectoryExists(targetDirectory))
			{
				throw new AutomationException(
					"Final plugin container directory does not exist: {0}",
					targetDirectory
				);
			}

			foreach (var file in Directory.GetFiles(
				targetDirectory,
				"*",
				SearchOption.TopDirectoryOnly))
			{
				var extension = Path.GetExtension(file).ToLowerInvariant();
				if (extension != ".pak" &&
				    extension != ".utoc" &&
				    extension != ".ucas" &&
				    extension != ".sig")
				{
					continue;
				}

				var fileBase = Path.GetFileNameWithoutExtension(file);
				string desiredBase = null;

				if (fileBase.Equals(
					originalBaseName,
					StringComparison.OrdinalIgnoreCase))
				{
					desiredBase = targetBaseName;
				}
				else if (fileBase.StartsWith(
					"pakchunk" + chunkId + "-Windows",
					StringComparison.OrdinalIgnoreCase))
				{
					var chunkPrefix = "pakchunk" + chunkId + "-Windows";
					var suffix = fileBase.Substring(chunkPrefix.Length);
					desiredBase = targetBaseName + suffix;
				}
				else if (fileBase.StartsWith(
					targetBaseName,
					StringComparison.OrdinalIgnoreCase))
				{
					// Already correct, including any partition suffix such as _s1.
					continue;
				}

				if (string.IsNullOrWhiteSpace(desiredBase))
				{
					continue;
				}

				var destination = Path.Combine(
					targetDirectory,
					desiredBase + extension
				);

				if (string.Equals(
					file,
					destination,
					StringComparison.OrdinalIgnoreCase))
				{
					continue;
				}

				if (FileExists(destination))
				{
					DeleteFile(destination);
				}

				File.Move(file, destination);

				LogInformation(
					"Forced final container name: {0} -> {1}",
					Path.GetFileName(file),
					Path.GetFileName(destination)
				);
			}

			var expectedPak = Path.Combine(
				targetDirectory,
				targetBaseName + ".pak");
			var expectedUtoc = Path.Combine(
				targetDirectory,
				targetBaseName + ".utoc");
			var expectedUcas = Path.Combine(
				targetDirectory,
				targetBaseName + ".ucas");

			if (!FileExists(expectedPak) ||
			    !FileExists(expectedUtoc) ||
			    !FileExists(expectedUcas))
			{
				throw new AutomationException(
					"Final CrimeFactory container verification failed. Expected: {0}, {1}, {2}",
					expectedPak,
					expectedUtoc,
					expectedUcas
				);
			}

			LogInformation(
				"Verified final CrimeFactory container set: {0}",
				targetBaseName
			);
		}

		private static void CopyWindowsPaksToWinGDK(string pluginDirectory)
		{
			var paksDirectory = Path.Combine(pluginDirectory, "Content", "Paks");
			var windowsDirectory = Path.Combine(paksDirectory, "Windows");
			if (!Directory.Exists(windowsDirectory))
			{
				return;
			}

			var winGDKDirectory = Path.Combine(paksDirectory, "WinGDK");

			// Replace the previous copy so removed or renamed containers cannot
			// survive a subsequent packaging run. Keep Windows and all file names.
			if (Directory.Exists(winGDKDirectory))
			{
				Directory.Delete(winGDKDirectory, true);
			}
			Directory.CreateDirectory(winGDKDirectory);

			foreach (var directory in Directory.GetDirectories(
				windowsDirectory, "*", SearchOption.AllDirectories))
			{
				Directory.CreateDirectory(Path.Combine(
					winGDKDirectory,
					Path.GetRelativePath(windowsDirectory, directory)));
			}

			var files = Directory.GetFiles(
				windowsDirectory, "*", SearchOption.AllDirectories);
			foreach (var file in files)
			{
				File.Copy(
					file,
					Path.Combine(winGDKDirectory, Path.GetRelativePath(windowsDirectory, file)),
					true);
			}

			LogInformation(
				"Copied {0} file(s) from Content/Paks/Windows to Content/Paks/WinGDK for {1}.",
				files.Length,
				Path.GetFileName(pluginDirectory));
		}

		private static void ArchiveWorkingPlugin(
			ProjectParams dlcParams,
			string workingPluginDirectory,
			string pluginName,
			string finalCookPlatform,
            PD3Params pd3Params)
		{
			var baseArchiveDirectory = CombinePaths(
				Path.GetDirectoryName(
					dlcParams.RawProjectPath.ToString()
				),
				"Saved",
				"ArchivedPlugins"
			);

			var archiveDirectory = Path.Combine(
				baseArchiveDirectory,
				finalCookPlatform
			);
			CreateDirectory(archiveDirectory);

			var zipFilePath = Path.Combine(
				archiveDirectory,
				pluginName + ".zip"
			);

			if (FileExists(zipFilePath))
			{
				DeleteFile(zipFilePath);
			}

			ZipFile.CreateFromDirectory(
				workingPluginDirectory,
				zipFilePath
			);

            CreateUtilityArchive(workingPluginDirectory, zipFilePath, pd3Params);

			LogInformation(
				"Archived two-stage plugin: {0}",
				zipFilePath
			);
		}

        private static void CreateUtilityArchive(string source, string zipPath, PD3Params settings)
        {
            if (!settings.CreateUtilityArchive) return;
            CrimeFactoryArchive.Create(source, Path.ChangeExtension(zipPath, CrimeFactoryArchive.Extension(settings.ArchiveUtility)),
                settings.ArchiveUtility, settings.ArchiveExecutable, message => LogInformation("{0}", message));
        }

        private static void ExportArchivedZip(ProjectParams projectParams, PD3Params pd3Params,
            string platform, string pluginName)
        {
            var destination = CrimeFactoryZipExport.Copy(Path.GetDirectoryName(projectParams.RawProjectPath.ToString()),
                pd3Params.ZipExportDirectory, platform, pluginName);
            if (destination != null) LogInformation("Exported mod ZIP: {0}", destination);
        }

		private static void DeployWorkingPlugin(
			ProjectParams dlcParams,
			PD3Params pd3Params,
			string pluginName,
			string workingPluginDirectory)
		{
			if (!pd3Params.CopyToGameDirectory)
			{
				return;
			}

			if (string.IsNullOrWhiteSpace(pd3Params.GameDirectory))
			{
				throw new AutomationException(
					"-CopyToGameDir was specified, but no game directory " +
					"path has been provided"
				);
			}

			CopyPluginToTheGameDir(
				pd3Params.GameDirectory,
				dlcParams.RawProjectPath,
				pluginName,
				workingPluginDirectory
			);
		}

		private static void ArchivePluginProject(
			ProjectParams projectParams,
			IEnumerable<DeploymentContext> deploymentContexts,
            PD3Params pd3Params)
		{
			var baseArchiveDirectory = CombinePaths(
				Path.GetDirectoryName(
					projectParams.RawProjectPath.ToString()
				),
				"Saved",
				"ArchivedPlugins"
			);

			foreach (var deploymentContext in deploymentContexts)
			{
				var stageRootDirectory =
					deploymentContext.StageDirectory;

				var relativePluginPath =
					GetPluginPathRelativeToStageRoot(projectParams);

				var stagePluginDirectory = Path.Combine(
					stageRootDirectory.ToString(),
					relativePluginPath
				);

				var archiveDirectory = Path.Combine(
					baseArchiveDirectory,
					deploymentContext.FinalCookPlatform
				);
				CreateDirectory(archiveDirectory);

				var zipFilePath = Path.Combine(
					archiveDirectory,
					projectParams.DLCFile
						.GetFileNameWithoutAnyExtensions() + ".zip"
				);

				if (FileExists(zipFilePath))
				{
					DeleteFile(zipFilePath);
				}

				ZipFile.CreateFromDirectory(
					stagePluginDirectory,
					zipFilePath
				);
                CreateUtilityArchive(stagePluginDirectory, zipFilePath, pd3Params);
			}
		}

		private static void DeployPluginProject(
			ProjectParams projectParams,
			IEnumerable<DeploymentContext> deploymentContexts,
			PD3Params pd3Params)
		{
			foreach (var deploymentContext in deploymentContexts)
			{
				if (pd3Params.CopyToGameDirectory &&
				    deploymentContext.FinalCookPlatform == "Windows")
				{
					var stageRootDirectory =
						deploymentContext.StageDirectory;

					var relativePluginPath =
						GetPluginPathRelativeToStageRoot(projectParams);

					var stagePluginDirectory = Path.Combine(
						stageRootDirectory.ToString(),
						relativePluginPath
					);

					if (pd3Params.GameDirectory == null)
					{
						throw new AutomationException(
							"-CopyToGameDir was specified, but no game " +
							"directory path has been provided"
						);
					}

					CopyPluginToTheGameDir(
						pd3Params.GameDirectory,
						projectParams.RawProjectPath,
						projectParams.DLCFile
							.GetFileNameWithoutAnyExtensions(),
						stagePluginDirectory
					);
				}
			}
		}

		private static void CleanStagingDirectories(
			IEnumerable<DeploymentContext> deploymentContexts)
		{
			if (deploymentContexts == null)
				return;

			foreach (var deploymentContext in deploymentContexts)
			{
				if (DirectoryExists(
					deploymentContext.StageDirectory.ToString()))
				{
					DeleteDirectory(
						deploymentContext.StageDirectory
					);
				}
			}
		}

		private static void CleanWorkingPluginDirectory(
			string workingPluginDirectory)
		{
			if (!string.IsNullOrWhiteSpace(workingPluginDirectory) &&
			    DirectoryExists(workingPluginDirectory))
			{
				DeleteDirectory(workingPluginDirectory);
			}
		}

		private static void ExecuteLegacyCook(
			BuildCommand cmd,
			PD3Params pd3Params,
			CrimeFactoryPackageResult result)
		{
			result.SetPhase("Single-stage: Prepare DLC cook");
			var projectParams = GetDLCParams(cmd);

			result.SetPhase("Single-stage: Cook");
			Project.Cook(projectParams);

			var deploymentContexts =
				CreateDeploymentContexts(projectParams);

			RemapCookedPluginsContentPaths(
				projectParams,
				deploymentContexts
			);

			try
			{
				result.SetPhase("Single-stage: Stage");
				CopyBuildToStagingDirectory(
					projectParams,
					deploymentContexts
				);

				result.SetPhase("Single-stage: Finalize containers");

				// Legacy/single-pass mods never enter PASS 2, so apply the
				// PAYDAY 3 patch suffix directly to their staged plugin containers.
				foreach (var deploymentContext in deploymentContexts)
				{
					var stagedPluginDirectory = Path.Combine(
						deploymentContext.StageDirectory.ToString(),
						GetPluginPathRelativeToStageRoot(projectParams)
					);

					ApplyPatchSuffixToPluginContainers(
						stagedPluginDirectory,
						projectParams.DLCFile
							.GetFileNameWithoutAnyExtensions()
					);

					CopyWindowsPaksToWinGDK(stagedPluginDirectory);
				}

				result.SetPhase("Archive mod");
				ArchivePluginProject(
					projectParams,
					deploymentContexts,
                    pd3Params
				);

                if (!string.IsNullOrWhiteSpace(pd3Params.ZipExportDirectory))
                {
                    result.SetPhase("Export ZIP");
                    foreach (var context in deploymentContexts)
                        ExportArchivedZip(projectParams, pd3Params, context.FinalCookPlatform,
                            projectParams.DLCFile.GetFileNameWithoutAnyExtensions());
                }

				result.SetPhase("Deploy mod to PAYDAY 3");
				DeployPluginProject(
					projectParams,
					deploymentContexts,
					pd3Params
				);
			}
			finally
			{
				CleanStagingDirectories(deploymentContexts);
			}
		}

		/// <summary>
		/// PASS 2 only needs a valid Shipping target receipt so Unreal can
		/// construct a normal project DeploymentContext. The receipt may contain
		/// content-only plugins that used to exist under <Project>/Mods.
		///
		/// Rebuilding the entire Shipping target just because /Mods changed is
		/// extremely expensive and unnecessary. Instead, temporarily strip all
		/// project /Mods plugin entries from the receipt while PASS 2 staging is
		/// created, then restore the receipt byte-for-byte afterwards.
		/// </summary>
		private static string SanitizeShippingReceiptForModStaging(
			string shippingReceipt,
			string projectDirectory)
		{
			if (!FileExists(shippingReceipt))
			{
				throw new AutomationException(
					"Cannot sanitize missing Shipping receipt: {0}",
					shippingReceipt
				);
			}

			string originalReceipt =
				File.ReadAllText(shippingReceipt);

			SystemJsonNode rootNode;

			try
			{
				rootNode = SystemJsonNode.Parse(originalReceipt);
			}
			catch (Exception exception)
			{
				throw new AutomationException(
					"Could not parse Shipping target receipt '{0}': {1}",
					shippingReceipt,
					exception.Message
				);
			}

			SystemJsonObject rootObject =
				rootNode as SystemJsonObject;

			if (rootObject == null)
			{
				throw new AutomationException(
					"Shipping target receipt '{0}' is not a JSON object.",
					shippingReceipt
				);
			}

			SystemJsonArray plugins =
				rootObject["Plugins"] as SystemJsonArray;

			if (plugins == null || plugins.Count == 0)
			{
				LogInformation(
					"PASS 2: Shipping receipt has no top-level Plugins entries to sanitize; RuntimeDependencies will be validated in the deployment context before staging."
				);

				return null;
			}

			int removedCount = 0;

			/*
			 * Receipt plugin entries normally contain a Descriptor path such as:
			 *
			 *   ../../../PAYDAY3/Mods/Test/Test.uplugin
			 *
			 * Use the serialized entry rather than depending on one exact UE
			 * receipt schema/property name. This keeps the check compatible if
			 * UE stores the descriptor under Descriptor/File/Path.
			 */
			for (int index = plugins.Count - 1;
				 index >= 0;
				 --index)
			{
				SystemJsonNode pluginNode =
					plugins[index];

				if (pluginNode == null)
				{
					continue;
				}

				string serialized =
					pluginNode.ToJsonString();

				string normalized =
					serialized
						.Replace("\\\\", "/")
						.Replace("\\", "/");

				bool isProjectMod =
					normalized.IndexOf(
						"/Mods/",
						StringComparison.OrdinalIgnoreCase
					) >= 0;

				if (!isProjectMod)
				{
					continue;
				}

				string pluginName = "";

				if (pluginNode is SystemJsonObject pluginObject)
				{
					try
					{
						pluginName =
							pluginObject["Name"]?.GetValue<string>()
							?? "";
					}
					catch
					{
						pluginName = "";
					}
				}

				LogInformation(
					"PASS 2: temporarily removing /Mods plugin from Shipping receipt: {0}",
					string.IsNullOrWhiteSpace(pluginName)
						? serialized
						: pluginName
				);

				plugins.RemoveAt(index);
				++removedCount;
			}

			if (removedCount == 0)
			{
				LogInformation(
					"PASS 2: Shipping receipt contains no /Mods plugin entries."
				);

				return null;
			}

			try
			{
				string sanitizedReceipt =
					rootObject.ToJsonString(
						new JsonSerializerOptions
						{
							WriteIndented = true
						}
					);

				File.WriteAllText(
					shippingReceipt,
					sanitizedReceipt
				);
			}
			catch (Exception exception)
			{
				throw new AutomationException(
					"Could not write sanitized Shipping target receipt '{0}': {1}",
					shippingReceipt,
					exception.Message
				);
			}

			LogInformation(
				"PASS 2: sanitized Shipping receipt for staging; removed {0} project /Mods plugin entr{1}.",
				removedCount,
				removedCount == 1 ? "y" : "ies"
			);

			return originalReceipt;
		}

		private static void RestoreShippingReceipt(
			string shippingReceipt,
			string originalReceipt)
		{
			if (string.IsNullOrWhiteSpace(shippingReceipt) ||
			    originalReceipt == null)
			{
				return;
			}

			try
			{
				File.WriteAllText(
					shippingReceipt,
					originalReceipt
				);

				LogInformation(
					"PASS 2: restored original Shipping target receipt."
				);
			}
			catch (Exception exception)
			{
				LogWarning(
					"PASS 2: failed to restore original Shipping target receipt '{0}': {1}",
					shippingReceipt,
					exception.Message
				);
			}
		}

		private static void ExecuteTwoStageCook(
			BuildCommand cmd,
			PD3Params pd3Params,
			string pluginName,
			int chunkId,
			CrimeFactoryPackageResult result)
		{
			IReadOnlyList<DeploymentContext> dlcContexts = null;
			IReadOnlyList<DeploymentContext> chunkContexts = null;
			string workingPluginDirectory = null;
			string shippingReceipt = null;
			string originalShippingReceipt = null;
			CrimeFactoryPALPackagingManifest palManifest = null;

			try
			{
				palManifest =
					LoadPALPackagingManifest(
						cmd,
						pluginName,
						chunkId
					);

				// ============================================================
				// PASS 1
				// ============================================================
				LogInformation(
					"=================================================="
				);
				LogInformation(
					"PASS 1/2: Plugin DLC cook for {0}",
					pluginName
				);
				LogInformation(
					"=================================================="
				);

				result.SetPhase("PASS 1: Prepare plugin DLC cook");
				var dlcParams = GetDLCParams(cmd);

				result.SetPhase("PASS 1: Cook plugin DLC");
				Project.Cook(dlcParams);

				dlcContexts =
					CreateDeploymentContexts(dlcParams);

				RemapCookedPluginsContentPaths(
					dlcParams,
					dlcContexts
				);

				result.SetPhase("PASS 1: Stage plugin DLC");
				CopyBuildToStagingDirectory(
					dlcParams,
					dlcContexts,
					palManifest.AdditionalPakFiles
				);

				result.SetPhase("PASS 1: Preserve plugin shell");
				workingPluginDirectory =
					CopyDLCPluginToWorkingDirectory(
						dlcParams,
						dlcContexts,
						pluginName
					);

				// PASS 2 uses the same standard staging root, so the plugin
				// shell must be outside it before continuing.
				CleanStagingDirectories(dlcContexts);

				// ============================================================
				// PASS 2
				// ============================================================
				LogInformation(
					"=================================================="
				);
				LogInformation(
					"PASS 2/2: PAL chunk cook for Chunk {0}",
					chunkId
				);
				LogInformation(
					"=================================================="
				);

				result.SetPhase("PASS 2: Prepare PAL chunk cook");

				LogInformation(
					"PASS 2 deterministic PAL input: {0} exact Chunk {1} package(s), "
					+ "including {2} external replacement package(s).",
					palManifest.ExpectedChunkPackages.Count,
					chunkId,
					palManifest.ExternalCookPackages.Count
				);

				var chunkParams =
					GetChunkParams(
						cmd,
						palManifest
					);

				/*
				 * Normal project staging needs a non-editor target receipt and its
				 * referenced executable. A modding project often only has
				 * PAYDAY3Editor built, which causes:
				 *
				 *   Missing PAYDAY3-Win64-Shipping.target
				 *   Missing UnrealGame binary
				 *
				 * Ensure the cooked client target receipt exists before staging. If it
				 * is already present, do not invoke UBT at all. If it is missing, build
				 * the game/Shipping target while explicitly skipping the Editor target.
				 * Nothing from
				 * this executable is copied into the final plugin DLC; we only take the
				 * selected PASS 2 container files.
				 */
				LogInformation(
					"PASS 2: ensuring the Shipping game target/receipt exists before staging"
				);

				var projectDirectory = Path.GetDirectoryName(
					chunkParams.RawProjectPath.ToString()
				);

				var projectName =
					chunkParams.RawProjectPath
						.GetFileNameWithoutAnyExtensions();

				shippingReceipt = Path.Combine(
					projectDirectory,
					"Binaries",
					"Win64",
					projectName + "-Win64-Shipping.target"
				);

				if (!FileExists(shippingReceipt))
				{
					LogInformation(
						"PASS 2: Shipping receipt does not exist. Building game target once: {0}",
						shippingReceipt
					);

					/*
					 * UE 5.5's ProjectBuildTargets enum does not expose a Client
					 * member. Use All, but GetChunkParams sets SkipBuildEditor=true,
					 * so Project.Build creates the cooked game/Shipping target
					 * without rebuilding PAYDAY3Editor.
					 *
					 * Once this receipt exists, normal mod creation/deletion no
					 * longer forces another C++ build.
					 */
					result.SetPhase("PASS 2: Build Shipping target");
					Project.Build(
						cmd,
						chunkParams,
						-1,
						ProjectBuildTargets.All
					);
				}
				else
				{
					LogInformation(
						"PASS 2: reusing existing Shipping receipt (no rebuild): {0}",
						shippingReceipt
					);
				}

				result.SetPhase("PASS 2: Cook PAL chunk");
				Project.Cook(chunkParams);

				result.SetPhase("PASS 2: Verify cooker consumed package manifest");
				EnsurePass2CookInputWasConsumed(palManifest);

				result.SetPhase("PASS 2: Verify PAL chunk");

				EnsureExpectedPackagesInChunkManifest(
					chunkParams,
					palManifest,
					pluginName,
					chunkId
				);

				/*
				 * Project.CreateDeploymentContext reads the Shipping receipt and
				 * retains its RuntimeDependencies in StageTargets. Preserve the legacy
				 * Plugins-array sanitization here; resolved runtime dependencies are
				 * validated separately immediately before CreateStagingManifest.
				 */
				result.SetPhase("PASS 2: Sanitize Shipping receipt");

				originalShippingReceipt =
					SanitizeShippingReceiptForModStaging(
						shippingReceipt,
						projectDirectory
					);

				chunkContexts =
					CreateDeploymentContexts(chunkParams);

				// DO NOT call RemapCookedPluginsContentPaths here.
				// PASS 2 is a normal project/chunk cook, not DLC.
				result.SetPhase("PASS 2: Stage PAL chunk");
				CopyBuildToStagingDirectory(
					chunkParams,
					chunkContexts,
					activeModDescriptor: dlcParams.DLCFile
				);

				result.SetPhase("PASS 2: Merge IoStore containers");
				InjectChunkIntoPluginDLC(
					workingPluginDirectory,
					chunkContexts,
					chunkId,
					pluginName
				);

				string finalCookPlatform = "Windows";
				foreach (var context in dlcContexts)
				{
					if (!string.IsNullOrWhiteSpace(
						context.FinalCookPlatform))
					{
						finalCookPlatform =
							context.FinalCookPlatform;

						if (finalCookPlatform == "Windows")
							break;
					}
				}

				result.SetPhase("Prepare Windows and WinGDK folders");
				CopyWindowsPaksToWinGDK(workingPluginDirectory);

				// Archive/deploy the PASS 1 plugin shell after PASS 2's
				// pak/utoc/ucas have been injected into Content/Paks.
				result.SetPhase("Archive mod");
				ArchiveWorkingPlugin(
					dlcParams,
					workingPluginDirectory,
					pluginName,
					finalCookPlatform,
                    pd3Params
				);

                if (!string.IsNullOrWhiteSpace(pd3Params.ZipExportDirectory))
                {
                    result.SetPhase("Export ZIP");
                    ExportArchivedZip(dlcParams, pd3Params, finalCookPlatform, pluginName);
                }

				result.SetPhase("Deploy mod to PAYDAY 3");
				DeployWorkingPlugin(
					dlcParams,
					pd3Params,
					pluginName,
					workingPluginDirectory
				);
			}
			finally
			{
				if (palManifest != null &&
				    !string.IsNullOrWhiteSpace(palManifest.Pass2CommandFile) &&
				    File.Exists(palManifest.Pass2CommandFile))
				{
					// Avoid leaving stale preboot commands around on failure.
					try { File.Delete(palManifest.Pass2CommandFile); }
					catch (Exception exception)
					{
						LogWarning("Could not delete stale PASS 2 cook input: {0}",
							exception.Message);
					}
				}
				/*
				 * Receipt sanitization is deliberately temporary. The real target
				 * receipt is restored exactly as it was even if PASS 2 fails.
				 */
				RestoreShippingReceipt(
					shippingReceipt,
					originalShippingReceipt
				);

				CleanStagingDirectories(dlcContexts);
				CleanStagingDirectories(chunkContexts);
				CleanWorkingPluginDirectory(
					workingPluginDirectory
				);
			}
		}

		public override void ExecuteBuild()
		{
			string resultFile =
				ParseOptionalStringParam("CrimeFactoryResultFile");

			var result =
				new CrimeFactoryPackageResult(resultFile);

			try
			{
				result.SetPhase("Validate packaging request");

				var pluginName = GetPluginName(this);
				var chunkId = GetRequestedChunkId(this);

				result.PluginName = pluginName;
				result.ChunkId = chunkId;
				result.Mode =
					chunkId >= 0
						? "Two-stage"
						: "Single-stage";

				string gameDir =
					ParseOptionalStringParam("GameDir");

				result.SetPhase("Validate PAYDAY 3 path");

				if (!string.IsNullOrEmpty(gameDir))
				{
					var gameLayout = RequirePAYDAY3GameLayout(gameDir);
					gameDir = gameLayout.InstallDirectory;

					LogInformation(
						"Resolved PAYDAY 3 install: {0} ({1})",
						gameDir,
						gameLayout.IsMicrosoftStore
							? "Microsoft Store/Xbox WinGDK"
							: "Steam/Epic desktop"
					);
				}

				var pd3Params = new PD3Params
				{
					GameDirectory = gameDir,
					StartGame = ParseParam("LaunchGame"),
					LaunchGameType =
						ParseOptionalStringParam(
							"LaunchGameType"),
                    ZipExportDirectory = ParseOptionalStringParam("ZipExportDir"),
                    CreateUtilityArchive = ParseParam("CreateUtilityArchive"),
                    ArchiveUtility = ParseOptionalStringParam("ArchiveUtility") ?? "WinRAR",
                    ArchiveExecutable = ParseOptionalStringParam("ArchiveExecutable"),
					CopyToGameDirectory =
						ParseParam("CopyToGameDir")
				};

				if (chunkId >= 0)
				{
					LogInformation(
						"CrimeFactory two-stage mode: Plugin={0}, Chunk={1}",
						pluginName,
						chunkId
					);

					ExecuteTwoStageCook(
						this,
						pd3Params,
						pluginName,
						chunkId,
						result
					);
				}
				else
				{
					LogInformation(
						"No ChunkId supplied. Using original CrimeFactory DLC cook."
					);

					ExecuteLegacyCook(
						this,
						pd3Params,
						result
					);
				}

				if (pd3Params.StartGame)
				{
					result.SetPhase("Launch PAYDAY 3");

					LogInformation(
						"Launching PAYDAY 3 after packaging. Store={0}",
						NormalizeLaunchGameType(
							pd3Params.LaunchGameType)
					);

					LaunchPAYDAY3(pd3Params);
				}

				result.SetPhase("Completed");
				result.WriteSuccess();
			}
			catch (Exception exception)
			{
				result.WriteFailure(exception);
				throw;
			}
		}
	}
}
