param(
    [Parameter(Mandatory=$true)][string]$WorkspacePath,
    [switch]$OpenCopy,
    [switch]$RunAttenuatorAnalysis,
    [Nullable[double]]$LossDb,
    [switch]$CaptureRun
)
$ErrorActionPreference = 'Stop'
if ($null -ne $LossDb -and (-not $RunAttenuatorAnalysis -or
    [double]::IsNaN($LossDb) -or [double]::IsInfinity($LossDb) -or $LossDb -lt 0 -or $LossDb -gt 100)) {
    throw 'LossDb requires RunAttenuatorAnalysis and a finite value from 0 to 100 dB.'
}
$projectRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$allowedRoot = [IO.Path]::GetFullPath((Join-Path $projectRoot 'build-reference')) + '\'
$resolvedPath = (Resolve-Path -LiteralPath $WorkspacePath).Path
if (-not $resolvedPath.StartsWith($allowedRoot, [StringComparison]::OrdinalIgnoreCase) -or
    [IO.Path]::GetExtension($resolvedPath) -ne '.wsv' -or
    -not [IO.Path]::GetFileName($resolvedPath).StartsWith('RFModel_')) {
    throw 'Only RFModel_*.wsv copies inside build-reference are accepted.'
}
$interop = 'C:\Program Files\Keysight\SystemVue2023\Examples\RF Architecture Design\RF Design Kit\Scripting\Excel_to_RF\Interop.GENESYS.dll'
Add-Type -Path $interop
Add-Type -ReferencedAssemblies $interop -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public static class ReferenceWorkspaceInspector
{
    public static string RunStartedUtc;
    public static string RunReturnedUtc;
    public static string ManagerErrors;

    public sealed class Node
    {
        public string path;
        public string methods;
        public string[] variables;
        public object data;
        public int[] dimensions;
        public string timestamp;
    }

    private static void Visit(GENESYS.IItem item, string path, int depth, List<Node> nodes)
    {
        if (path.Contains("System1_Sch1_Data") && path.EndsWith("/Eqns"))
        {
            depth = 3;
        }
        if (path.EndsWith("/Sch1/PartList/Attn"))
        {
            depth = 2;
        }
        if (nodes.Count >= 500)
        {
            throw new InvalidOperationException("Reference inspection node limit exceeded");
        }
        var variables = new string[item.GetVarCount()];
        for (int index = 0; index < variables.Length; ++index)
        {
            variables[index] = item.GetVarName(index) + ":" + item.GetVarType(index);
        }
        var node = new Node { path = path, methods = item.GetMethodList(), variables = variables };
        for (int index = 0; index < variables.Length; ++index)
        {
            string name = item.GetVarName(index);
            if (name == "TimeStamp")
            {
                node.timestamp = Convert.ToString(item.GetVarValue(index));
            }
            if (name == "Data")
            {
                object value = item.GetVarValue(index);
                var array = value as Array;
                if (array != null)
                {
                    node.dimensions = new int[array.Rank];
                    for (int dimension = 0; dimension < array.Rank; ++dimension)
                    {
                        node.dimensions[dimension] = array.GetLength(dimension);
                    }
                    if (array.Length <= 4096)
                    {
                        var values = new List<object>();
                        foreach (object entry in array)
                        {
                            values.Add(entry is ValueType || entry is string ? entry : "unsupported");
                        }
                        node.data = values.ToArray();
                    }
                }
                else if (value is ValueType || value is string)
                {
                    node.data = value;
                }
            }
        }
        nodes.Add(node);
        if (depth == 0)
        {
            return;
        }
        for (int index = 0; index < item.GetItemCount(); ++index)
        {
            var child = item.GetItemByIndex(index);
            try
            {
                Visit(child, path + "/" + child.GetName(), depth - 1, nodes);
            }
            finally
            {
                Marshal.ReleaseComObject(child);
            }
        }
    }

    public static Node[] Inspect(string path, bool open, bool run, double lossDb)
    {
        Console.Error.WriteLine("phase: attach-active-instance");
        object active = Marshal.GetActiveObject("Genesys.Application");
        try
        {
            var application = (GENESYS.Application)active;
            var manager = application.Manager;
            try
            {
                string expectedName = System.IO.Path.GetFileNameWithoutExtension(path);
                if (open)
                {
                    if (manager.GetWorkspaceCount() != 0)
                    {
                        throw new InvalidOperationException(
                            "FileOpen can replace the active workspace; launch a dedicated reference instance instead");
                    }
                    Console.Error.WriteLine("phase: open-copy-in-empty-instance");
                    manager.FileOpen(path);
                }
                Console.Error.WriteLine("phase: find-reference-workspace");
                for (int index = 0; index < manager.GetWorkspaceCount(); ++index)
                {
                    var workspace = manager.GetWorkspaceByIndex(index);
                    try
                    {
                        var item = (GENESYS.IItem)workspace;
                        if (item.GetName() != expectedName)
                        {
                            continue;
                        }
                        Console.Error.WriteLine("phase: inspect-reference-objects");
                        if (run)
                        {
                            if (expectedName != "RFModel_AttenuatorNoise" || manager.GetWorkspaceCount() != 1)
                            {
                                throw new InvalidOperationException("Analysis requires the sole dedicated attenuator workspace");
                            }
                            string setup = "wsdoc=Application.Manager.GetWorkspaceByIndex(0)\r\n";
                            if (!Double.IsNaN(lossDb))
                            {
                                setup += "wsdoc.Designs.Sch1.PartList.Attn.ParamSet.L.Set(\"" +
                                    lossDb.ToString("R", System.Globalization.CultureInfo.InvariantCulture) +
                                    "\")\r\n";
                            }
                            RunStartedUtc = DateTime.UtcNow.ToString("o");
                            Console.Error.WriteLine("phase: run-analysis " + RunStartedUtc);
                            application.RunScript(
                                setup + "wsdoc.Designs.System1.RunAnalysis()\r\n",
                                GENESYS.ScriptLanguage.genLangVBScript);
                            RunReturnedUtc = DateTime.UtcNow.ToString("o");
                            ManagerErrors = manager.GetErrors();
                            Console.Error.WriteLine("phase: analysis-returned " + RunReturnedUtc);
                            Console.Error.WriteLine("manager-errors: " + ManagerErrors);
                        }
                        var nodes = new List<Node>();
                        Visit(item, expectedName, 4, nodes);
                        return nodes.ToArray();
                    }
                    finally
                    {
                        Marshal.ReleaseComObject(workspace);
                    }
                }
                throw new InvalidOperationException("Reference workspace not found after lookup");
            }
            finally
            {
                Marshal.ReleaseComObject(manager);
            }
        }
        finally
        {
            Marshal.ReleaseComObject(active);
        }
    }
}
'@
$loss = if ($null -eq $LossDb) { [double]::NaN } else { [double]$LossDb }
$nodes = [ReferenceWorkspaceInspector]::Inspect($resolvedPath, $OpenCopy.IsPresent,
    $RunAttenuatorAnalysis.IsPresent, $loss)
if ($CaptureRun) {
    [ordered]@{
        run_started_utc = [ReferenceWorkspaceInspector]::RunStartedUtc
        run_returned_utc = [ReferenceWorkspaceInspector]::RunReturnedUtc
        manager_errors = [ReferenceWorkspaceInspector]::ManagerErrors
        nodes = $nodes
    } | ConvertTo-Json -Depth 10
} else {
    $nodes | ConvertTo-Json -Depth 8
}
