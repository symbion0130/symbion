param(
    [string]$SourceDocx = (Join-Path (Resolve-Path (Join-Path $PSScriptRoot "..")).Path "docs\source\MasterDocument.docx"),
    [string]$OutFile = (Join-Path (Resolve-Path (Join-Path $PSScriptRoot "..")).Path "docs\source\MasterDocument.txt")
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $SourceDocx)) {
    throw "Master document not found: $SourceDocx"
}

Add-Type -AssemblyName System.IO.Compression.FileSystem

$zip = [System.IO.Compression.ZipFile]::OpenRead($SourceDocx)
try {
    $entry = $zip.GetEntry("word/document.xml")
    if ($null -eq $entry) {
        throw "word/document.xml not found inside $SourceDocx"
    }

    $reader = New-Object System.IO.StreamReader($entry.Open())
    $xmlText = $reader.ReadToEnd()
    $reader.Close()

    $xml = [xml]$xmlText
    $ns = New-Object System.Xml.XmlNamespaceManager($xml.NameTable)
    $ns.AddNamespace("w", "http://schemas.openxmlformats.org/wordprocessingml/2006/main")

    $paragraphs = New-Object System.Collections.Generic.List[string]
    foreach ($p in $xml.SelectNodes("//w:p", $ns)) {
        $parts = @()
        foreach ($node in $p.SelectNodes(".//w:t", $ns)) {
            $parts += $node.InnerText
        }
        $line = (($parts -join "") -replace "\s+", " ").Trim()
        if ($line.Length -gt 0) {
            $paragraphs.Add($line)
        }
    }

    $dir = Split-Path -Parent $OutFile
    if (-not (Test-Path $dir)) {
        New-Item -ItemType Directory -Path $dir | Out-Null
    }
    [System.IO.File]::WriteAllLines($OutFile, $paragraphs, [System.Text.UTF8Encoding]::new($false))
    Write-Host "Extracted $($paragraphs.Count) paragraphs to $OutFile"
}
finally {
    $zip.Dispose()
}
