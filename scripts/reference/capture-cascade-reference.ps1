param(
    [Parameter(Mandatory=$true)][string]$WorkspacePath,
    [ValidateRange(-60,-10)][double]$SourcePowerDbm = -30,
    [ValidateRange(-180,180)][double]$SourcePhaseDeg = 0,
    [ValidateSet(2,3)][int]$MaximumOrder = 3,
    [ValidateSet(0,10)][int]$SecondGainDb = 10,
    [ValidateSet(100,140)][int]$ReverseIsolationDb = 100,
    [ValidateSet(1,1000000)][double]$ChannelBandwidthHz = 1000000,
    [switch]$SecondarySpectrum,
    [switch]$TwoTone,
    [ValidateSet(-50,-140)][int]$SecondaryRangeDb = -50
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../build-reference'))
$resolved = (Resolve-Path -LiteralPath $WorkspacePath).Path
if ([IO.Path]::GetDirectoryName($resolved) -ne $root -or
    [IO.Path]::GetFileName($resolved) -ne 'RFModel_CascadeIntermods.wsv') {
    throw 'Expected the dedicated cascade copy directly inside build-reference.'
}
[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)
$interop = 'C:/Program Files/Keysight/SystemVue2023/Examples/RF Architecture Design/RF Design Kit/Scripting/Excel_to_RF/Interop.GENESYS.dll'
Add-Type -Path $interop
Add-Type -ReferencedAssemblies $interop -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Runtime.InteropServices;
public static class CascadeReference
{
    public static string Started, Returned, Errors, Script;
    public sealed class Node
    {
        public string path;
        public string methods;
        public string model;
        public string netlist;
        public string[] variables;
        public object data;
        public int[] dimensions;
        public string timestamp;
        public string data_entry;
        public string data_source;
        public string evaluation_error;
        public Dictionary<string, object> analysis_settings;
    }

    private static void Visit(GENESYS.IItem item, string path, int depth, List<Node> nodes)
    {
        if (path.Contains("System3_Design3_Data") && path.EndsWith("/Eqns"))
        {
            depth = 3;
        }
        if (path.Contains("/Design3/PartList/") && path.Split('/').Length == 5)
        {
            depth = 2;
        }
        if (nodes.Count >= 2000)
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
            if (name == "Model")
            {
                node.model = Convert.ToString(item.GetVarValue(index));
            }
            if (name == "Netlist")
            {
                node.netlist = Convert.ToString(item.GetVarValue(index));
            }
            if (path.EndsWith("/System3"))
            {
                if (node.analysis_settings == null)
                {
                    node.analysis_settings = new Dictionary<string, object>();
                }
                node.analysis_settings[name] = item.GetVarValue(index);
            }
            if (name == "DataEntry")
            {
                node.data_entry = Convert.ToString(item.GetVarValue(index));
            }
            if (name == "TimeStamp")
            {
                node.timestamp = Convert.ToString(item.GetVarValue(index));
            }
            if (name == "Data")
            {
                object value = item.GetVarValue(index);
                node.data_source = "Data property";
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
                            values.Add(entry is ValueType || entry is string ? entry
                                                                             : "unsupported");
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
        // Some PartParam objects expose only their expression, without a Data
        // property. Read the documented evaluated value through IDispatch.
        if (node.data == null && path.Contains("/ParamSet/") && node.methods.Contains("GetValue()"))
        {
            try
            {
                object value = item.GetType().InvokeMember(
                    "GetValue", System.Reflection.BindingFlags.InvokeMethod, null, item,
                    new object[0]);
                var array = value as Array;
                if (array != null && array.Length <= 4096)
                {
                    node.dimensions = new int[array.Rank];
                    for (int dimension = 0; dimension < array.Rank; ++dimension)
                    {
                        node.dimensions[dimension] = array.GetLength(dimension);
                    }
                    var entries = new List<object>();
                    foreach (object entry in array)
                    {
                        entries.Add(entry is ValueType || entry is string ? entry : "unsupported");
                    }
                    node.data = entries.ToArray();
                }
                else if (value is ValueType || value is string)
                {
                    node.data = value;
                }
                node.data_source = "GetValue method";
            }
            catch (Exception error)
            {
                node.evaluation_error = error.GetBaseException().Message;
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

    public static Node[] Run(double power, double phase, int order, int secondGain,
                             double channelBandwidth, bool secondary, int secondaryRange,
                             bool twoTone, int reverseIsolation)
    {
        object active = Marshal.GetActiveObject("Genesys.Application");
        try
        {
            var app = (GENESYS.Application)active;
            var manager = app.Manager;
            try
            {
                if (manager.GetWorkspaceCount() != 1)
                {
                    throw new InvalidOperationException("Expected sole dedicated workspace");
                }
                var workspace = manager.GetWorkspaceByIndex(0);
                try
                {
                    var root = (GENESYS.IItem)workspace;
                    if (root.GetName() != "RFModel_CascadeIntermods")
                    {
                        throw new InvalidOperationException(
                            "Unexpected workspace; no commands submitted");
                    }
                    string analysis = "wsdoc.Designs.System3";
                    string parts = "wsdoc.Designs.Design3.PartList.";
                    var calls = new List<string>();
                    foreach (string name in new[] { "CalcHarmonics", "CalcIntermods", "ShowTotals",
                                                    "CoherentIM" })
                    {
                        calls.Add(analysis + ".SetProperty(\"" + name + "\", enabled)");
                    }
                    foreach (string name in new[] { "AutoRecalc", "CalcNoise", "CalcPhaseNoise",
                                                    "UseSourcePts" })
                    {
                        calls.Add(analysis + ".SetProperty(\"" + name + "\", disabled)");
                    }
                    calls.Add(analysis + ".MaxOrder.Set(\"" + order + "\")");
                    calls.Add(analysis + ".Path0.PathFreq.Set(\"1000\")");
                    calls.Add(analysis + ".ChanBW.Set(\"" +
                              (channelBandwidth / 1e6).ToString("R", CultureInfo.InvariantCulture) +
                              "\")");
                    calls.Add(analysis + ".UseVolterra.Set(\"" + (secondary ? "1" : "0") + "\")");
                    calls.Add(analysis + ".UseWithin.Set(\"" + secondaryRange.ToString() + "\")");
                    for (int i = 1; i <= 2; ++i)
                    {
                        string amp = parts + "RFAmp" + i + ".ParamSet.";
                        string[] names = {
                            "G",        "NF",           "OP1dB",         "OPSAT",
                            "OIP2",     "OIP3",         "Zref",          "ZIN",
                            "ZOUT",     "RISO",         "PortParamType", "AMtoPM_Mode",
                            "EnablePN", "FrequencyMode"
                        };
                        string[] values = { i == 1 ? "10" : secondGain.ToString(),
                                            "3",
                                            "20",
                                            "23",
                                            "40",
                                            "30",
                                            "50",
                                            "50",
                                            "50",
                                            reverseIsolation.ToString(),
                                            "0",
                                            "0",
                                            "0",
                                            "0" };
                        for (int j = 0; j < names.Length; ++j)
                        {
                            calls.Add(amp + names[j] + ".Set(\"" + values[j] + "\")");
                        }
                    }
                    string source = parts + "Source.ParamSet.";
                    string[] sourceNames = { "Name",     "Enable", "SrcType", "MultiCarrier",
                                             "EnablePN", "Freq",   "BW",      "Pwr",
                                             "Phase",    "RefClk", "R",       "ZO" };
                    string[] sourceValues = { "Source1",
                                              "[1]",
                                              "[0]",
                                              "[0]",
                                              "[0]",
                                              "1000",
                                              "0.000001",
                                              power.ToString("R", CultureInfo.InvariantCulture),
                                              phase.ToString("R", CultureInfo.InvariantCulture),
                                              "",
                                              "50",
                                              "50" };
                    for (int i = 0; i < sourceNames.Length; ++i)
                    {
                        calls.Add(source + sourceNames[i] + ".Set(\"" + sourceValues[i] + "\")");
                    }
                    if (twoTone)
                    {
                        calls.Add(source + "Name.Set(\"=[\"\"Source1\"\",\"\"Source2\"\"]\")");
                        calls.Add(source + "Enable.Set(\"[1;1]\")");
                        calls.Add(source + "SrcType.Set(\"[0;0]\")");
                        calls.Add(source + "MultiCarrier.Set(\"[0;0]\")");
                        calls.Add(source + "EnablePN.Set(\"[0;0]\")");
                        calls.Add(source + "Freq.Set(\"[1000;1100]\")");
                        calls.Add(source + "Pwr.Set(\"[" +
                                  power.ToString("R", CultureInfo.InvariantCulture) + ";" +
                                  power.ToString("R", CultureInfo.InvariantCulture) + "]\")");
                        calls.Add(source + "Phase.Set(\"[0;" +
                                  phase.ToString("R", CultureInfo.InvariantCulture) + "]\")");
                        calls.Add(source + "RefClk.Set(\"=[\"\"\"\",\"\"\"\"]\")");
                    }
                    calls.Add(parts + "Port_2.ParamSet.ZO.Set(\"50\")");
                    calls.Add(analysis + ".ClearModelCache()");
                    calls.Add(analysis + ".RunAnalysis()");
                    Script = "Dim wsdoc, enabled, disabled\r\n" +
                             "Set wsdoc = Application.Manager.GetWorkspaceByIndex(0)\r\n" +
                             "enabled = CByte(1)\r\ndisabled = CByte(0)\r\n";
                    foreach (string call in calls)
                    {
                        Script += "Call " + call + "\r\n";
                    }
                    Started = DateTime.UtcNow.ToString("o");
                    Console.Error.WriteLine("phase: submit-cascade " + Started);
                    app.RunScript(Script, GENESYS.ScriptLanguage.genLangVBScript);
                    Returned = DateTime.UtcNow.ToString("o");
                    Errors = manager.GetErrors();
                    Console.Error.WriteLine("phase: analysis-returned " + Returned);
                    Console.Error.WriteLine("manager-errors: " + Errors);
                    var nodes = new List<Node>();
                    Visit(root, root.GetName(), 4, nodes);
                    return nodes.ToArray();
                }
                finally
                {
                    Marshal.ReleaseComObject(workspace);
                }
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
$sha = [Security.Cryptography.SHA256]::Create()
try {
    $workspaceHash = [BitConverter]::ToString($sha.ComputeHash([IO.File]::ReadAllBytes($resolved))).Replace('-', '').ToLowerInvariant()
}
finally {
    $sha.Dispose()
}
$nodes = [CascadeReference]::Run(
    $SourcePowerDbm,
    $SourcePhaseDeg,
    $MaximumOrder,
    $SecondGainDb,
    $ChannelBandwidthHz,
    $SecondarySpectrum.IsPresent,
    $SecondaryRangeDb,
    $TwoTone.IsPresent,
    $ReverseIsolationDb
)
[ordered]@{
    run_started_utc = [CascadeReference]::Started
    run_returned_utc = [CascadeReference]::Returned
    manager_errors = [CascadeReference]::Errors
    systemvue_version = [Diagnostics.FileVersionInfo]::GetVersionInfo('C:/Program Files/Keysight/SystemVue2023/Bin/SystemVue.exe').FileVersion
    script_language = 'VBScript'
    submitted_script = [CascadeReference]::Script
    source_power_dbm = $SourcePowerDbm
    source_phase_deg = $SourcePhaseDeg
    maximum_order = $MaximumOrder
    second_gain_db = $SecondGainDb
    reverse_isolation_db = $ReverseIsolationDb
    channel_bandwidth_hz = $ChannelBandwidthHz
    two_tone = $TwoTone.IsPresent
    secondary_spectrum = $SecondarySpectrum.IsPresent
    secondary_range_db = $SecondaryRangeDb
    workspace_sha256 = $workspaceHash
    nodes = $nodes
} | ConvertTo-Json -Depth 12
