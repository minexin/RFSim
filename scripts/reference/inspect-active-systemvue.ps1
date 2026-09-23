# Run in Windows PowerShell 5.1: attach only; never create/close an application.
param(
    [string]$InteropPath = 'C:\Program Files\Keysight\SystemVue2023\Examples\RF Architecture Design\RF Design Kit\Scripting\Excel_to_RF\Interop.GENESYS.dll'
)
$ErrorActionPreference = 'Stop'
Add-Type -Path $InteropPath
Add-Type -ReferencedAssemblies $InteropPath -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class ActiveSystemVueInspector
{
    public static int WorkspaceCount()
    {
        object active = Marshal.GetActiveObject("Genesys.Application");
        try
        {
            var application = (GENESYS.Application)active;
            return application.Manager.GetWorkspaceCount();
        }
        finally
        {
            if (Marshal.IsComObject(active))
            {
                Marshal.ReleaseComObject(active);
            }
        }
    }
}
'@
try {
    [pscustomobject]@{
        attached = $true
        workspace_count = [ActiveSystemVueInspector]::WorkspaceCount()
        operation = 'read_only_active_instance'
    } | ConvertTo-Json
} catch {
    [pscustomobject]@{
        attached = $false
        error = $_.Exception.ToString()
        operation = 'read_only_active_instance'
    } | ConvertTo-Json
    exit 1
}
