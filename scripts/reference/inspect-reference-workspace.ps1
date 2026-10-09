param(
    [Parameter(Mandatory=$true)][string]$WorkspacePath,
    [switch]$OpenCopy,
    [switch]$RunAttenuatorAnalysis,
    [switch]$RunAntennaAnalysis,
    [switch]$RunCompressionAnalysis,
    [Nullable[double]]$LossDb,
    [Nullable[double]]$TemperatureK,
    [Nullable[double]]$SourcePowerDbm,
    [ValidateSet(50, 100)][int]$CompressionRisoDb = 100,
    [ValidateSet('sample', 'antenna', 'limiter')][string]$CompressionProfile = 'sample',
    [ValidateSet(22, 23, 26)][int]$CompressionOpsatDbm = 23,
    [switch]$PreserveManagerMessages,
    [switch]$CompressionTwoTone,
    [Nullable[double]]$CompressionSecondPowerDbm,
    [Nullable[double]]$CompressionFirstPhaseDeg,
    [Nullable[double]]$CompressionSecondPhaseDeg,
    [switch]$CaptureRun
)
$ErrorActionPreference = 'Stop'
foreach ($phase in @($CompressionFirstPhaseDeg, $CompressionSecondPhaseDeg)) {
    if ($null -ne $phase -and (-not $CompressionTwoTone -or [double]::IsNaN($phase) -or
        [double]::IsInfinity($phase) -or $phase -lt -360 -or $phase -gt 360)) {
        throw 'Phase overrides require two-tone mode and finite values from -360 to 360 degrees.'
    }
}
if ($null -ne $CompressionSecondPowerDbm -and (-not $CompressionTwoTone -or
    [double]::IsNaN($CompressionSecondPowerDbm) -or [double]::IsInfinity($CompressionSecondPowerDbm) -or
    $CompressionSecondPowerDbm -lt -200 -or $CompressionSecondPowerDbm -gt 30)) {
    throw 'CompressionSecondPowerDbm requires two-tone mode and a finite value from -200 to 30 dBm.'
}
if ($CompressionTwoTone -and (-not $RunCompressionAnalysis -or $CompressionProfile -notin @('sample', 'limiter') -or
    $PreserveManagerMessages -or $null -eq $SourcePowerDbm -or $PSBoundParameters.ContainsKey('CompressionOpsatDbm'))) {
    throw 'CompressionTwoTone requires sample/limiter compression, explicit first-tone power and no diagnostic/OPSAT override.'
}
if ($PreserveManagerMessages -and (-not $RunCompressionAnalysis -or $CompressionProfile -ne 'sample')) {
    throw 'PreserveManagerMessages requires sample-profile RunCompressionAnalysis.'
}
if ($PSBoundParameters.ContainsKey('CompressionOpsatDbm') -and
    (-not $RunCompressionAnalysis -or $CompressionProfile -ne 'sample')) {
    throw 'CompressionOpsatDbm requires sample-profile RunCompressionAnalysis.'
}
if ($PSBoundParameters.ContainsKey('CompressionProfile') -and -not $RunCompressionAnalysis) {
    throw 'CompressionProfile requires RunCompressionAnalysis.'
}
if ($PSBoundParameters.ContainsKey('CompressionRisoDb') -and -not $RunCompressionAnalysis) {
    throw 'CompressionRisoDb requires RunCompressionAnalysis.'
}
if ($RunCompressionAnalysis -and ($RunAntennaAnalysis -or $RunAttenuatorAnalysis -or
    $null -ne $LossDb -or $null -ne $TemperatureK)) {
    throw 'Compression analysis cannot be combined with other case overrides.'
}
[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)
if ($RunAntennaAnalysis -and ($RunAttenuatorAnalysis -or $null -ne $LossDb -or
    $null -ne $TemperatureK)) {
    throw 'Antenna reference analysis cannot use attenuator parameter overrides.'
}
if ($null -ne $SourcePowerDbm -and ((-not $RunAttenuatorAnalysis -and -not $RunAntennaAnalysis -and -not $RunCompressionAnalysis) -or
    [double]::IsNaN($SourcePowerDbm) -or [double]::IsInfinity($SourcePowerDbm) -or
    $SourcePowerDbm -lt -200 -or $SourcePowerDbm -gt 30)) {
    throw 'SourcePowerDbm requires RunAttenuatorAnalysis and a finite value from -200 to 30 dBm.'
}
if ($null -ne $TemperatureK -and (-not $RunAttenuatorAnalysis -or
    [double]::IsNaN($TemperatureK) -or [double]::IsInfinity($TemperatureK) -or
    $TemperatureK -le 0 -or $TemperatureK -gt 1000)) {
    throw 'TemperatureK requires RunAttenuatorAnalysis and a finite value in (0, 1000] K.'
}
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
        public string data_entry;
        public string data_source;
        public string evaluation_error;
    }

    private static void Visit(GENESYS.IItem item, string path, int depth, List<Node> nodes)
    {
        if ((path.Contains("System1_Sch1_Data") || path.Contains("System1_Data")) && path.EndsWith("/Eqns"))
        {
            depth = 3;
        }
        if (path.Contains("/Sch1/PartList/") && path.Split('/').Length == 5)
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
        // Some PartParam objects expose only their expression, without a Data
        // property. Read the documented evaluated value through IDispatch.
        if (node.data == null && path.Contains("/ParamSet/") &&
            node.methods.Contains("GetValue()"))
        {
            try
            {
                object value = item.GetType().InvokeMember("GetValue",
                    System.Reflection.BindingFlags.InvokeMethod, null, item, new object[0]);
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

    public static Node[] Inspect(string path, bool open, bool run, bool antenna, double lossDb, double temperatureK,
        double sourcePowerDbm, bool compression, int compressionRisoDb, string compressionProfile,
        int compressionOpsatDbm, bool preserveManagerMessages, bool compressionTwoTone,
        double secondPowerDbm, double firstPhaseDeg, double secondPhaseDeg)
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
                    application.RunScript("OpenWorkspace(\"" + path.Replace("\"", "\"\"") + "\")",
                        GENESYS.ScriptLanguage.genLangVBScript);
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
                            string permittedName = compression ? "RFModel_AmplifierCompression" :
                                (antenna ? "RFModel_AntennaNoise" : "RFModel_AttenuatorNoise");
                            if (expectedName != permittedName || manager.GetWorkspaceCount() != 1)
                            {
                                throw new InvalidOperationException("Analysis requires the sole dedicated reference workspace");
                            }
                            string setup = "wsdoc=Application.Manager.GetWorkspaceByIndex(0)\r\n";
                            if (compression)
                            {
                                // Make previously implicit defaults explicit for the controlled experiment.
                                string amp = "wsdoc.Designs.Sch1.PartList.RFAmp.ParamSet.";
                                bool antennaProfile = compressionProfile == "antenna";
                                bool limiterProfile = compressionProfile == "limiter";
                                string gain = antennaProfile ? "30" : limiterProfile ? "10" : "20";
                                string p1db = antennaProfile ? "60" : limiterProfile ? "15" : "20";
                                string saturation = antennaProfile ? "63" : limiterProfile ? "18" : compressionOpsatDbm.ToString();
                                string nf = antennaProfile ? (10 * Math.Log10(1 + 700.0 / 290.0)).ToString(
                                    "R", System.Globalization.CultureInfo.InvariantCulture) : "3";
                                setup += amp + "G.Set(\"" + gain + "\")\r\n" +
                                    amp + "NF.Set(\"" + nf + "\")\r\n" +
                                    amp + "OP1dB.Set(\"" + p1db + "\")\r\n" +
                                    amp + "OPSAT.Set(\"" + saturation + "\")\r\n" +
                                    amp + "OIP2.Set(\"" + (antennaProfile ? "80" : "40") + "\")\r\n" +
                                    amp + "OIP3.Set(\"" + (antennaProfile ? "70" : "30") + "\")\r\n" +
                                    "wsdoc.Designs.Sch1.PartList.Source.ParamSet.Freq.Set(\"" +
                                    (antennaProfile ? "5000" : "1000") + "\")\r\n" +
                                    amp + "RISO.Set(\"" + compressionRisoDb.ToString() + "\")\r\n" +
                                    amp + "ZIN.Set(\"50\")\r\n" +
                                    amp + "ZOUT.Set(\"50\")\r\n" +
                                    "wsdoc.Designs.Sch1.PartList.Source.ParamSet.R.Set(\"50\")\r\n" +
                                    "wsdoc.Designs.Sch1.PartList.Out.ParamSet.ZO.Set(\"50\")\r\n";
                                string source = "wsdoc.Designs.Sch1.PartList.Source.ParamSet.";
                                string phases = "[" + firstPhaseDeg.ToString("R", System.Globalization.CultureInfo.InvariantCulture) +
                                    ";" + secondPhaseDeg.ToString("R", System.Globalization.CultureInfo.InvariantCulture) + "]";
                                // Set both modes explicitly so a later single-tone run restores all arrays.
                                setup += source + "Name.Set(\"" + (compressionTwoTone ? "=[\"\"Source1\"\",\"\"Source2\"\"]" : "Source1") + "\")\r\n" +
                                    source + "Enable.Set(\"" + (compressionTwoTone ? "[1;1]" : "[1]") + "\")\r\n" +
                                    source + "SrcType.Set(\"" + (compressionTwoTone ? "[0;0]" : "[0]") + "\")\r\n" +
                                    source + "EnablePN.Set(\"" + (compressionTwoTone ? "[0;0]" : "[0]") + "\")\r\n" +
                                    source + "MultiCarrier.Set(\"" + (compressionTwoTone ? "[0;0]" : "[0]") + "\")\r\n" +
                                    source + "Phase.Set(\"" + (compressionTwoTone ? phases : "[0]") + "\")\r\n" +
                                    source + "BW.Set(\"" + (compressionTwoTone ? "[1;1]" : "1") + "\")\r\n";
                                if (compressionTwoTone) {
                                    setup += source + "Freq.Set(\"[1000;1100]\")\r\n";
                                }
                                setup += "wsdoc.Designs.System1.Path0.PathFreq.Set(\"" +
                                    (compressionTwoTone ? "1000" : "") + "\")\r\n";
                            }
                            if (!Double.IsNaN(lossDb))
                            {
                                setup += "wsdoc.Designs.Sch1.PartList.Attn.ParamSet.L.Set(\"" +
                                    lossDb.ToString("R", System.Globalization.CultureInfo.InvariantCulture) +
                                    "\")\r\n";
                            }
                            if (!Double.IsNaN(temperatureK))
                            {
                                setup += "wsdoc.Designs.System1.RoomTemp.Set(\"" +
                                    (temperatureK - 273.15).ToString("R", System.Globalization.CultureInfo.InvariantCulture) +
                                    "\")\r\n";
                            }
                            if (!Double.IsNaN(sourcePowerDbm))
                            {
                                string powerText = sourcePowerDbm.ToString("R", System.Globalization.CultureInfo.InvariantCulture);
                                string secondPowerText = Double.IsNaN(secondPowerDbm) ? powerText :
                                    secondPowerDbm.ToString("R", System.Globalization.CultureInfo.InvariantCulture);
                                string sourcePath = antenna
                                    ? "wsdoc.GetItemByName(\"RF Design\").GetItemByName(\"Sch1\").PartList.Source"
                                    : "wsdoc.Designs.Sch1.PartList.Source";
                                // Preserve the second (noise) source entry from the official case.
                                setup += sourcePath + ".ParamSet.Pwr.Set(\"" +
                                    (antenna ? "[" + powerText + ";-50]" : compressionTwoTone
                                        ? "[" + powerText + ";" + secondPowerText + "]" : powerText) + "\")\r\n";
                            }
                            RunStartedUtc = DateTime.UtcNow.ToString("o");
                            Console.Error.WriteLine("phase: run-analysis " + RunStartedUtc);
                            string analysis = antenna
                                ? "wsdoc.GetItemByName(\"RF Design\").GetItemByName(\"System1\").RunAnalysis()\r\n"
                                : "wsdoc.Designs.System1.RunAnalysis()\r\n";
                            application.RunScript(
                                setup + analysis,
                                GENESYS.ScriptLanguage.genLangVBScript);
                            RunReturnedUtc = DateTime.UtcNow.ToString("o");
                            ManagerErrors = manager.GetErrors();
                            Console.Error.WriteLine("phase: analysis-returned " + RunReturnedUtc);
                            Console.Error.WriteLine("manager-errors: " + ManagerErrors);
                        }
                        var nodes = new List<Node>();
                        Visit(item, expectedName, 4, nodes);
                        if (run)
                        {
                            string datasetPath = compression
                                ? expectedName + "/Designs/System1_Data_Folder/System1_Data_Path1"
                                : antenna
                                ? expectedName + "/RF Design/System1_Data_Folder/System1_Data_Path1"
                                : expectedName + "/Designs/System1_Data_Folder/System1_Sch1_Data_Path1";
                            var datasets = nodes.FindAll(node => node.path == datasetPath);
                            long timestamp;
                            var epoch = new DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc);
                            double started = (DateTime.Parse(RunStartedUtc, null,
                                System.Globalization.DateTimeStyles.RoundtripKind).ToUniversalTime() - epoch).TotalSeconds;
                            double returned = (DateTime.Parse(RunReturnedUtc, null,
                                System.Globalization.DateTimeStyles.RoundtripKind).ToUniversalTime() - epoch).TotalSeconds;
                            if (datasets.Count != 1 || !Int64.TryParse(datasets[0].timestamp, out timestamp) ||
                                timestamp < Math.Floor(started) || timestamp > Math.Ceiling(returned))
                            {
                                throw new InvalidOperationException("Analysis did not produce a fresh reference dataset");
                            }
                            if (!String.IsNullOrWhiteSpace(ManagerErrors) && !preserveManagerMessages)
                            {
                                throw new InvalidOperationException("Reference analysis reported manager errors");
                            }
                        }
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
$temperature = if ($null -eq $TemperatureK) { [double]::NaN } else { [double]$TemperatureK }
$power = if ($null -eq $SourcePowerDbm) { [double]::NaN } else { [double]$SourcePowerDbm }
$secondPower = if ($null -eq $CompressionSecondPowerDbm) { [double]::NaN } else { [double]$CompressionSecondPowerDbm }
$firstPhase = if ($null -eq $CompressionFirstPhaseDeg) { 0.0 } else { [double]$CompressionFirstPhaseDeg }
$secondPhase = if ($null -eq $CompressionSecondPhaseDeg) { 0.0 } else { [double]$CompressionSecondPhaseDeg }
$nodes = [ReferenceWorkspaceInspector]::Inspect($resolvedPath, $OpenCopy.IsPresent,
    ($RunAttenuatorAnalysis.IsPresent -or $RunAntennaAnalysis.IsPresent -or $RunCompressionAnalysis.IsPresent),
    $RunAntennaAnalysis.IsPresent, $loss, $temperature, $power, $RunCompressionAnalysis.IsPresent,
    $CompressionRisoDb, $CompressionProfile, $CompressionOpsatDbm, $PreserveManagerMessages.IsPresent,
    $CompressionTwoTone.IsPresent, $secondPower, $firstPhase, $secondPhase)
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
