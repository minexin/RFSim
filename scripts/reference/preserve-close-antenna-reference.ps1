# Preserve the dedicated antenna reference before switching to another vendor case.
param([Parameter(Mandatory=$true)][string]$BackupPath)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../build-reference')) + '\'
$backup = [IO.Path]::GetFullPath($BackupPath)
if (-not $backup.StartsWith($root, [StringComparison]::OrdinalIgnoreCase) -or
    [IO.Path]::GetFileName($backup) -notmatch '^RFModel_AntennaBackup-[a-zA-Z0-9-]+\.wsv$' -or
    (Test-Path -LiteralPath $backup)) {
    throw 'Backup must be a new RFModel_AntennaBackup-*.wsv inside build-reference.'
}
$interop = 'C:/Program Files/Keysight/SystemVue2023/Examples/RF Architecture Design/RF Design Kit/Scripting/Excel_to_RF/Interop.GENESYS.dll'
Add-Type -Path $interop
Add-Type -ReferencedAssemblies $interop -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class PreserveAntennaReference
{
    public static void Run(string backup)
    {
        object active = Marshal.GetActiveObject("Genesys.Application");
        try
        {
            var app = (GENESYS.Application)active;
            var manager = app.Manager;
            try
            {
                if (manager.GetWorkspaceCount() != 1)
                    throw new InvalidOperationException("Expected sole antenna reference workspace");
                var workspace = manager.GetWorkspaceByIndex(0);
                try
                {
                    if (((GENESYS.IItem)workspace).GetName() != "RFModel_AntennaNoise")
                        throw new InvalidOperationException("Unexpected active workspace; refusing to close");
                    app.RunScript("wsdoc=Application.Manager.GetWorkspaceByIndex(0)\r\n" +
                        "wsdoc.SaveAs(\"" + backup.Replace("\"", "\"\"") + "\")\r\n",
                        GENESYS.ScriptLanguage.genLangVBScript);
                    if (!System.IO.File.Exists(backup) || new System.IO.FileInfo(backup).Length == 0)
                        throw new InvalidOperationException("Backup was not created; workspace remains open");
                    app.RunScript("wsdoc=Application.Manager.GetWorkspaceByIndex(0)\r\n" +
                        "wsdoc.CloseWorkspace()\r\n", GENESYS.ScriptLanguage.genLangVBScript);
                    if (manager.GetWorkspaceCount() != 0)
                        throw new InvalidOperationException("Workspace close did not complete");
                }
                finally { Marshal.ReleaseComObject(workspace); }
            }
            finally { Marshal.ReleaseComObject(manager); }
        }
        finally { Marshal.ReleaseComObject(active); }
    }
}
'@
[PreserveAntennaReference]::Run($backup)
Get-FileHash -LiteralPath $backup
