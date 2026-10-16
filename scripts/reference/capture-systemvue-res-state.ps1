# Run with Windows PowerShell 5.1. Attaches to an existing instance only.
# Submits no analysis, explicit mutation, save, or clear-error API calls.
# Parameter getters can evaluate expressions and update caches/errors; this is
# a sequential parameter snapshot, not an atomic or side-effect-free read.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][ValidateNotNullOrEmpty()][string]$ExpectedWorkspace,
    [Parameter(Mandatory = $true)][ValidateNotNullOrEmpty()][string]$OutputStem
)
$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSVersion.Major -ne 5 -or $PSVersionTable.PSVersion.Minor -ne 1) {
    throw 'Use Windows PowerShell 5.1 (powershell.exe), not pwsh.'
}
if ($OutputStem -notmatch '^(?:[A-Za-z]:[\\/]|\\\\[^\\/]+[\\/][^\\/]+[\\/])') {
    throw 'OutputStem must be an absolute drive or UNC path, without an extension added by this script.'
}
$outputPath = [IO.Path]::GetFullPath($OutputStem + '.json')
$outputDirectory = [IO.Path]::GetDirectoryName($outputPath)
if (-not [IO.Directory]::Exists($outputDirectory)) { throw 'Output parent directory must already exist.' }
if (Test-Path -LiteralPath $outputPath) { throw 'Output exists; prior evidence will not be overwritten.' }
$interop = 'C:/Program Files/Keysight/SystemVue2023/Examples/RF Architecture Design/RF Design Kit/Scripting/Excel_to_RF/Interop.GENESYS.dll'
if (-not (Test-Path -LiteralPath $interop -PathType Leaf)) { throw 'Installed SystemVue 2023 Interop assembly is missing.' }
Add-Type -Path $interop
Add-Type -ReferencedAssemblies $interop -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Reflection;
using System.Runtime.InteropServices;

public static class RFModelResStateCapture
{
    sealed class Context
    {
        public bool Complete = true;
        public long Nodes;
        public bool ParameterEvaluationPerformed;
    }
    static Dictionary<string, object> Map(params object[] pairs)
    {
        var result = new Dictionary<string, object>();
        for (int i = 0; i < pairs.Length; i += 2) result.Add((string)pairs[i], pairs[i + 1]);
        return result;
    }
    static object Error(Exception error)
    {
        var cause = error.GetBaseException();
        return Map("exception_type", error.GetType().FullName, "message", error.Message,
                   "hresult", error.HResult, "base_exception_type", cause.GetType().FullName,
                   "base_message", cause.Message, "base_hresult", cause.HResult);
    }
    static object Call(object item, string name)
    {
        return item.GetType().InvokeMember(name, BindingFlags.InvokeMethod, null, item, new object[0]);
    }
    static void Release(object value)
    {
        if (value != null && Marshal.IsComObject(value)) Marshal.ReleaseComObject(value);
    }
    // COM-valued properties are identities, never recursively inspected or stringified.
    static object Value(object value, Context context, HashSet<Array> arrays)
    {
        if (value == null) return Map("kind", "null");
        string type = value.GetType().FullName;
        if (Marshal.IsComObject(value)) return Map("kind", "com_object_not_expanded", "clr_type", type);
        var array = value as Array;
        if (array != null)
        {
            if (!arrays.Add(array))
            {
                context.Complete = false;
                return Map("kind", "array_cycle_not_expanded", "clr_type", type);
            }
            try
            {
                int rank = array.Rank;
                var lengths = new int[rank]; var lower = new int[rank]; var upper = new int[rank];
                var indices = new int[rank];
                for (int d = 0; d < rank; ++d)
                {
                    lengths[d] = array.GetLength(d); lower[d] = array.GetLowerBound(d);
                    upper[d] = array.GetUpperBound(d); indices[d] = lower[d];
                }
                var entries = new List<object>();
                if (array.LongLength != 0)
                {
                    while (true)
                    {
                        entries.Add(Map("indices", (int[])indices.Clone(),
                                        "value", Value(array.GetValue(indices), context, arrays)));
                        int d = rank - 1;
                        while (d >= 0 && indices[d] == upper[d]) { indices[d] = lower[d]; --d; }
                        if (d < 0) break;
                        ++indices[d];
                    }
                }
                return Map("kind", "array", "clr_type", type, "rank", rank,
                           "lengths", lengths, "lower_bounds", lower, "upper_bounds", upper,
                           "element_count", array.LongLength, "indexed_values", entries);
            }
            finally { arrays.Remove(array); }
        }
        if (value is double)
        {
            double number = (double)value;
            bool finite = !Double.IsNaN(number) && !Double.IsInfinity(number);
            // Hex bits are authoritative even if a JSON consumer rounds the number,
            // discards signed zero, or cannot represent a non-finite value.
            return Map("kind", "scalar", "clr_type", type,
                       "value", finite ? (object)number : null, "finite", finite,
                       "roundtrip_text", number.ToString("R", CultureInfo.InvariantCulture),
                       "binary64_hex", unchecked((ulong)BitConverter.DoubleToInt64Bits(number)).ToString("x16"));
        }
        if (value is float)
        {
            float number = (float)value;
            bool finite = !Single.IsNaN(number) && !Single.IsInfinity(number);
            return Map("kind", "scalar", "clr_type", type,
                       "value", finite ? (object)number : null, "finite", finite,
                       "roundtrip_text", number.ToString("R", CultureInfo.InvariantCulture),
                       "binary32_hex", BitConverter.ToUInt32(BitConverter.GetBytes(number), 0).ToString("x8"));
        }
        if (value is string || value is bool || value is byte || value is sbyte ||
            value is short || value is ushort || value is int || value is uint ||
            value is long || value is ulong || value is decimal || value is char)
            return Map("kind", "scalar", "clr_type", type, "value", value,
                       "invariant_text", Convert.ToString(value, CultureInfo.InvariantCulture));
        if (value is DateTime)
            return Map("kind", "datetime", "clr_type", type,
                       "roundtrip_text", ((DateTime)value).ToString("o", CultureInfo.InvariantCulture));
        if (value is DBNull) return Map("kind", "dbnull", "clr_type", type);
        context.Complete = false;
        return Map("kind", "unsupported_not_expanded", "clr_type", type);
    }
    static object Attempt(Func<object> read, Context context)
    {
        try { return Map("ok", true, "result", Value(read(), context, new HashSet<Array>())); }
        catch (Exception error)
        {
            context.Complete = false;
            return Map("ok", false, "error", Error(error));
        }
    }
    // A failed or non-string observation is unknown, never an assumed match.
    static object EqualCapturedStrings(object before, object after)
    {
        var left = before as Dictionary<string, object>;
        var right = after as Dictionary<string, object>;
        if (left == null || right == null || !left.ContainsKey("ok") || !right.ContainsKey("ok") ||
            !(bool)left["ok"] || !(bool)right["ok"] || !left.ContainsKey("result") || !right.ContainsKey("result"))
            return null;
        var leftValue = left["result"] as Dictionary<string, object>;
        var rightValue = right["result"] as Dictionary<string, object>;
        if (leftValue == null || rightValue == null || !leftValue.ContainsKey("value") || !rightValue.ContainsKey("value") ||
            !(leftValue["value"] is string) || !(rightValue["value"] is string))
            return null;
        return String.Equals((string)leftValue["value"], (string)rightValue["value"], StringComparison.Ordinal);
    }
    static object Read(GENESYS.IItem item, string parentPath, int depth,
                       Context context, HashSet<long> ancestors)
    {
        ++context.Nodes;
        var node = Map("depth_remaining", depth);
        string name = null, methods = null;
        try { name = item.GetName(); node["name"] = name; }
        catch (Exception error) { context.Complete = false; node["name_error"] = Error(error); }
        string path = parentPath + "/" + (name ?? "<unreadable-name>"); node["path"] = path;
        IntPtr identity = Marshal.GetIUnknownForObject(item);
        long key = identity.ToInt64(); Marshal.Release(identity);
        if (!ancestors.Add(key))
        {
            context.Complete = false; node["cycle_not_expanded"] = true; return node;
        }
        try
        {
            try { methods = item.GetMethodList(); node["methods"] = methods; }
            catch (Exception error) { context.Complete = false; node["methods_error"] = Error(error); }
            var variables = new List<object>(); node["variables"] = variables;
            try
            {
                int count = item.GetVarCount(); node["variable_count"] = count;
                for (int i = 0; i < count; ++i)
                {
                    int index = i;
                    variables.Add(Map("index", i,
                        "name", Attempt(() => item.GetVarName(index), context),
                        "xml_name", Attempt(() => item.GetVarXMLName(index), context),
                        "type", Attempt(() => item.GetVarType(index), context),
                        "value", Attempt(() => item.GetVarValue(index), context)));
                }
            }
            catch (Exception error) { context.Complete = false; node["variables_error"] = Error(error); }
            if (methods != null && methods.Contains("GetValue()"))
            {
                context.ParameterEvaluationPerformed = true;
                var reads = new Dictionary<string, object>(); node["parameter_reads"] = reads;
                foreach (string method in new[] { "Get", "GetData", "GetValue" })
                {
                    string selected = method;
                    reads[method] = methods.Contains(method + "()")
                        ? Attempt(() => Call(item, selected), context)
                        : Map("supported", false);
                }
            }
            var children = new List<object>(); node["children"] = children;
            try
            {
                int count = item.GetItemCount(); node["child_count"] = count;
                if (depth == 0 && count != 0)
                {
                    context.Complete = false; node["children_not_read_due_to_depth"] = count;
                }
                else for (int i = 0; i < count; ++i)
                {
                    GENESYS.IItem child = null;
                    try
                    {
                        child = item.GetItemByIndex(i);
                        children.Add(Read(child, path, depth - 1, context, ancestors));
                    }
                    catch (Exception error)
                    {
                        context.Complete = false; children.Add(Map("child_index", i, "read_error", Error(error)));
                    }
                    finally { Release(child); }
                }
            }
            catch (Exception error) { context.Complete = false; node["children_error"] = Error(error); }
            return node;
        }
        finally { ancestors.Remove(key); }
    }
    public static object Capture(string expected)
    {
        string started = DateTime.UtcNow.ToString("o");
        object active = null; GENESYS.Manager manager = null; GENESYS.Workspace workspace = null;
        try
        {
            active = Marshal.GetActiveObject("Genesys.Application"); // Never creates an application.
            manager = ((GENESYS.Application)active).Manager;
            if (manager.GetWorkspaceCount() != 1) throw new InvalidOperationException("Expected exactly one workspace; no capture performed.");
            workspace = manager.GetWorkspaceByIndex(0);
            var root = (GENESYS.IItem)workspace;
            string actual = root.GetName();
            if (!String.Equals(actual, expected, StringComparison.Ordinal))
                throw new InvalidOperationException("Workspace name does not exactly match ExpectedWorkspace; no capture performed.");
            var context = new Context();
            var result = Map("schema_version", 2, "classification", "systemvue_parameter_snapshot_without_explicit_mutation_calls",
                "analysis_requested", false, "explicit_mutation_calls", 0, "workspace_save_calls", 0,
                "started_utc", started, "expected_workspace", expected, "workspace", actual,
                "tree_depth_limit", 12, "binary64_encoding", "binary64_hex is the exact IEEE754 bit pattern; roundtrip_text uses invariant R format");
            result["workspace_methods"] = Attempt(() => root.GetMethodList(), context);
            result["errors_before"] = Attempt(() => manager.GetErrors(), context);
            result["analysis_done_before"] = Attempt(() => Call(workspace, "IsAnalysisDone"), context);
            result["xml_before"] = Attempt(() => root.ToXML(), context);
            result["tree"] = Read(root, "", 12, context, new HashSet<long>());
            result["xml_after"] = Attempt(() => root.ToXML(), context);
            result["analysis_done_after"] = Attempt(() => Call(workspace, "IsAnalysisDone"), context);
            result["errors_after"] = Attempt(() => manager.GetErrors(), context);
            result["xml_before_after_equal"] = EqualCapturedStrings(result["xml_before"], result["xml_after"]);
            result["errors_before_after_equal"] = EqualCapturedStrings(result["errors_before"], result["errors_after"]);
            result["comparison_unknown_value"] = null;
            result["comparison_note"] = "Equality is null (unknown) when either observation failed or is not a string. Equality does not prove absence of other evaluation side effects.";
            result["workspace_count_after"] = Attempt(() => manager.GetWorkspaceCount(), context);
            result["workspace_name_after"] = Attempt(() => root.GetName(), context);
            result["parameter_evaluation_performed"] = context.ParameterEvaluationPerformed;
            result["capture_complete"] = context.Complete;
            result["nodes_read"] = context.Nodes;
            result["completed_utc"] = DateTime.UtcNow.ToString("o");
            result["snapshot_note"] = "Sequential parameter snapshot with no explicit mutation/save/analysis calls. Get/GetData/GetValue may evaluate expressions and change caches or errors. This is neither atomic nor a side-effect-free guarantee. capture_complete is traversal success, not analysis completion or numerical acceptance.";
            return result;
        }
        finally { Release(workspace); Release(manager); Release(active); }
    }
}
'@
$record = [RFModelResStateCapture]::Capture($ExpectedWorkspace)
$envelope = [ordered]@{
    collector_sha256 = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash.ToLowerInvariant()
    interop_path = $interop
    interop_sha256 = (Get-FileHash -LiteralPath $interop -Algorithm SHA256).Hash.ToLowerInvariant()
    powershell_version = $PSVersionTable.PSVersion.ToString()
    installed_executable_version = [Diagnostics.FileVersionInfo]::GetVersionInfo('C:/Program Files/Keysight/SystemVue2023/Bin/SystemVue.exe').FileVersion
    readback = $record
}
$json = $envelope | ConvertTo-Json -Depth 100
# CreateNew closes the preflight race and cannot replace existing evidence.
$stream = [IO.File]::Open($outputPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write)
try {
    $bytes = ([Text.UTF8Encoding]::new($false)).GetBytes($json + [Environment]::NewLine)
    $stream.Write($bytes, 0, $bytes.Length)
} finally { $stream.Dispose() }
[pscustomobject]@{
    output = $outputPath
    workspace = $record.workspace
    nodes_read = $record.nodes_read
    capture_complete = $record.capture_complete
    analysis_requested = $false
} | ConvertTo-Json
