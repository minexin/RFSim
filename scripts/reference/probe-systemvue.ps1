# Creates the registered COM server; does not open or save any workspaces.
# Activation can block inside COM: run with an external process timeout when unattended.
$ErrorActionPreference = 'Stop'
$prior = @(Get-Process SystemVue -ErrorAction SilentlyContinue)
$app = $null
try {
    $app = New-Object -ComObject Genesys.Application
    $count = $app.Manager.GetWorkspaceCount()
    [pscustomobject]@{
        com_created = $true
        workspace_count = $count
        prior_process_count = $prior.Count
    } | ConvertTo-Json
    if ($prior.Count -eq 0 -and $count -eq 0) { $app.Quit() }
} finally {
    if ($null -ne $app) {
        [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($app)
    }
}
