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
    public sealed class WorkspaceSnapshot
    {
        public string name;
        public int item_count;
        public string[] top_level_items;
    }

    public static WorkspaceSnapshot[] Inspect()
    {
        object active = Marshal.GetActiveObject("Genesys.Application");
        try
        {
            var application = (GENESYS.Application)active;
            var manager = application.Manager;
            try
            {
                int count = manager.GetWorkspaceCount();
                var snapshots = new WorkspaceSnapshot[count];
                for (int index = 0; index < count; ++index)
                {
                    var workspace = manager.GetWorkspaceByIndex(index);
                    try
                    {
                        var item = (GENESYS.IItem)workspace;
                        int itemCount = item.GetItemCount();
                        var names = new string[Math.Min(itemCount, 100)];
                        for (int childIndex = 0; childIndex < names.Length; ++childIndex)
                        {
                            var child = item.GetItemByIndex(childIndex);
                            try
                            {
                                names[childIndex] = child.GetName();
                            }
                            finally
                            {
                                Marshal.ReleaseComObject(child);
                            }
                        }
                        snapshots[index] = new WorkspaceSnapshot
                        {
                            name = item.GetName(),
                            item_count = itemCount,
                            top_level_items = names
                        };
                    }
                    finally
                    {
                        Marshal.ReleaseComObject(workspace);
                    }
                }
                return snapshots;
            }
            finally
            {
                Marshal.ReleaseComObject(manager);
            }
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
    $workspaces = @([ActiveSystemVueInspector]::Inspect())
    [pscustomobject]@{
        attached = $true
        workspace_count = $workspaces.Count
        workspaces = $workspaces
        operation = 'read_only_active_instance'
    } | ConvertTo-Json -Depth 5
} catch {
    [pscustomobject]@{
        attached = $false
        error = $_.Exception.ToString()
        operation = 'read_only_active_instance'
    } | ConvertTo-Json
    exit 1
}
