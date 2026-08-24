[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Source,

    [Parameter(Mandatory = $true)]
    [string]$Destination
)

$ErrorActionPreference = "Stop"
$sourcePath = (Resolve-Path $Source).Path
$destinationPath = [IO.Path]::GetFullPath($Destination)
Remove-Item -LiteralPath $destinationPath -Recurse -Force -ErrorAction SilentlyContinue
Copy-Item -LiteralPath $sourcePath -Destination $destinationPath -Recurse

$renameItems = Get-ChildItem -LiteralPath $destinationPath -Recurse -Force |
    Sort-Object { $_.FullName.Length } -Descending
foreach ($item in $renameItems) {
    if ($item.Name.Contains("FovControl")) {
        $renamed = $item.Name.Replace("FovControl", "PanoramaFovControl")
        Rename-Item -LiteralPath $item.FullName -NewName $renamed
    }
}

$extensions = @(".cpp", ".h", ".hpp", ".rc", ".reds", ".txt", ".cmake")
Get-ChildItem -LiteralPath $destinationPath -Recurse -File | Where-Object {
    $extensions -contains $_.Extension.ToLowerInvariant() -or $_.Name -eq "CMakeLists.txt"
} | ForEach-Object {
    $content = Get-Content -LiteralPath $_.FullName -Raw
    $content = $content.Replace("FovControl", "PanoramaFovControl")
    Set-Content -LiteralPath $_.FullName -Value $content -Encoding utf8NoBOM
}

Write-Host "Prepared collision-safe FOV Control source at $destinationPath"
