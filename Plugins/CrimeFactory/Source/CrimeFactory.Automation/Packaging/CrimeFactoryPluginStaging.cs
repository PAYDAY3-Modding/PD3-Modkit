using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using AutomationTool;
using EpicGames.Core;
using UnrealBuildTool;

namespace CrimeFactory.Automation
{
	/// <summary>Validates the resolved references UAT will consume, without rewriting build artifacts.</summary>
	internal static class CrimeFactoryPluginStaging
	{
		internal static void Validate(FileReference projectFile, DeploymentContext context,
			FileReference activeModDescriptor)
		{
			CommandUtils.LogInformation("[CrimeFactory] Validating plugin staging references. Active mod: {0}",
				activeModDescriptor?.GetFileNameWithoutExtension() ?? "(none)");

			var references = new Dictionary<FileReference, HashSet<string>>();
			void AddDescriptor(FileReference file, string source)
			{
				if (!file.HasExtension(".uplugin")) return;
				if (!references.TryGetValue(file, out var sources))
					references.Add(file, sources = new HashSet<string>());
				sources.Add(source);
			}

			foreach (StageTarget target in context.StageTargets)
			{
				TargetReceipt receipt = target.Receipt;
				string receiptPath = TargetReceipt.GetDefaultPath(
					receipt.ProjectFile?.Directory ?? context.EngineRoot, receipt.TargetName,
					receipt.Platform, receipt.Configuration, receipt.Architectures).FullName;
				foreach (RuntimeDependency dependency in receipt.RuntimeDependencies)
					AddDescriptor(dependency.Path, receiptPath + " -> RuntimeDependencies");
			}
			foreach (var file in context.FilesToStage.UFSFiles)
				AddDescriptor(file.Value, "DeploymentContext.FilesToStage.UFSFiles[" + file.Key + "]");
			if (activeModDescriptor != null)
				AddDescriptor(activeModDescriptor, "CrimeFactory active mod / ProjectParams.DLCFile (PASS 1)");

			// A mod is optional only if neither the project nor a retained plugin requires it.
			// Use UE's platform/target/configuration filters, including Optional, rather than
			// treating every old /Mods entry (or every enabled editor-only reference) alike.
			var requiredBy = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
			foreach (StageTarget target in context.StageTargets)
			{
				// Plugins in a UE5.5 receipt record explicit TargetRules.EnablePlugins.
				// They are requirements, unlike the automatically discovered mod
				// descriptors also present in RuntimeDependencies.
				foreach (var plugin in target.Receipt.PluginNameToEnabledState.Where(plugin => plugin.Value))
					requiredBy.TryAdd(plugin.Key, target.Receipt.TargetName + ".target -> Plugins[" + plugin.Key + "] (TargetRules.EnablePlugins)");
			}
			void AddRequirements(IEnumerable<PluginReferenceDescriptor> plugins, string source)
			{
				if (plugins == null) return;
				foreach (PluginReferenceDescriptor plugin in plugins)
				{
					if (!plugin.bOptional && context.StageTargets.Any(target =>
						plugin.IsEnabledForPlatform(target.Receipt.Platform) &&
						plugin.IsEnabledForTarget(target.Receipt.TargetType) &&
						plugin.IsEnabledForTargetConfiguration(target.Receipt.Configuration)))
						requiredBy.TryAdd(plugin.Name, source + " -> Plugins[" + plugin.Name + "]");
				}
			}
			AddRequirements(ProjectDescriptor.FromFile(projectFile).Plugins, projectFile.FullName);
			foreach (FileReference descriptor in references.Keys.Where(file => File.Exists(file.FullName)))
				AddRequirements(PluginDescriptor.FromFile(descriptor).Plugins, descriptor.FullName);

			var obsoleteRoots = new HashSet<DirectoryReference>();
			DirectoryReference modsDirectory = DirectoryReference.Combine(projectFile.Directory, "Mods");
			foreach (var reference in references)
			{
				FileReference descriptor = reference.Key;
				if (File.Exists(descriptor.FullName)) continue;
				string name = descriptor.GetFileNameWithoutExtension();
				string source = string.Join("; ", reference.Value);
				string reason = null;
				if (descriptor == activeModDescriptor)
					reason = "it is the mod currently being packaged";
				else if (requiredBy.TryGetValue(name, out string requirement))
					reason = "it is required by " + requirement;
				else if (descriptor != FileReference.Combine(modsDirectory, name, name + ".uplugin"))
					reason = "it is not a conventional project Mods/<Name>/<Name>.uplugin descriptor";
				else if (Directory.Exists(descriptor.Directory.FullName))
					reason = "its directory still exists; this may be an incomplete installation rather than a deleted mod";

				if (reason != null)
					throw new AutomationException(
						"[CrimeFactory] Missing plugin '{0}'. Descriptor: {1}. Reference source: {2}. " +
						"Cannot safely remove this reference because {3}. Restore/reinstall the plugin at this path, " +
						"or remove its requirement using the project/plugin settings before packaging again.",
						name, descriptor.FullName, source, reason);

				obsoleteRoots.Add(descriptor.Directory);
			}

			// Commit only after every missing descriptor has been classified. These are
			// freshly loaded, per-package receipt objects, not the on-disk Shipping receipt.
			foreach (DirectoryReference root in obsoleteRoots)
			{
				FileReference descriptor = FileReference.Combine(root, root.GetDirectoryName() + ".uplugin");
				int removed = 0;
				foreach (StageTarget target in context.StageTargets)
					removed += target.Receipt.RuntimeDependencies.RemoveAll(dependency => dependency.Path.IsUnderDirectory(root));
				foreach (var file in context.FilesToStage.UFSFiles.Where(file => file.Value.IsUnderDirectory(root)).ToList())
				{
					context.FilesToStage.UFSFiles.Remove(file.Key);
					++removed;
				}
				CommandUtils.LogInformation(
					"[CrimeFactory] Invalidated obsolete deleted mod '{0}': {1}. Reference source: {2}. " +
					"Removed {3} staging reference(s); preserved the build receipt on disk.",
					descriptor.GetFileNameWithoutExtension(), descriptor.FullName,
					string.Join("; ", references[descriptor]), removed);
			}
			CommandUtils.LogInformation("[CrimeFactory] Plugin staging validation completed ({0} obsolete mod(s)).", obsoleteRoots.Count);
		}
	}
}
