param(
    [string]$InputPath = "relatorio.md",
    [string]$OutputPath = "relatorio.docx"
)

$ErrorActionPreference = "Stop"

if (!(Test-Path $InputPath)) {
    throw "Input markdown not found: $InputPath"
}

$mdPath = (Resolve-Path $InputPath).Path
$outPath = if ([System.IO.Path]::IsPathRooted($OutputPath)) {
    $OutputPath
} else {
    Join-Path (Split-Path $mdPath -Parent) $OutputPath
}

$lines = Get-Content -Path $mdPath -Encoding UTF8
if ($lines.Count -eq 0) {
    throw "Input markdown is empty: $InputPath"
}

$title = $lines[0]
if ($title -match '^#\s+(.*)$') {
    $title = $Matches[1].Trim()
}

$coverLines = @()
$i = 1
while ($i -lt $lines.Count) {
    $ln = $lines[$i]
    if ($ln -match '^\s*---\s*$') {
        $i++
        break
    }
    if ($ln.Trim() -ne "") {
        $coverLines += $ln.Trim()
    }
    $i++
}

$bodyLines = @()
if ($i -lt $lines.Count) {
    $bodyLines = $lines[$i..($lines.Count - 1)]
}

$word = $null
$doc = $null

try {
    $word = New-Object -ComObject Word.Application
    $word.Visible = $false
    $doc = $word.Documents.Add()

    $cm = 28.3464567
    $ps = $doc.PageSetup
    $ps.PaperSize = 7 # wdPaperA4
    $ps.TopMargin = 1.8 * $cm
    $ps.BottomMargin = 1.8 * $cm
    $ps.LeftMargin = 1.8 * $cm
    $ps.RightMargin = 1.8 * $cm

    foreach ($sec in $doc.Sections) {
        foreach ($h in $sec.Headers) { $h.Range.Text = "" }
        foreach ($f in $sec.Footers) { $f.Range.Text = "" }
    }

    $normal = $doc.Styles.Item("Normal")
    $normal.Font.Name = "Calibri"
    $normal.Font.Size = 11
    $normal.ParagraphFormat.Alignment = 3 # justify
    $normal.ParagraphFormat.SpaceBefore = 0
    $normal.ParagraphFormat.SpaceAfter = 6
    $normal.ParagraphFormat.LineSpacingRule = 0
    $normal.ParagraphFormat.LineSpacing = 13.8

    $h1 = $doc.Styles.Item("Heading 1")
    $h1.Font.Name = "Calibri"
    $h1.Font.Size = 16
    $h1.Font.Bold = 1
    $h1.ParagraphFormat.Alignment = 0
    $h1.ParagraphFormat.SpaceBefore = 14
    $h1.ParagraphFormat.SpaceAfter = 6

    $h2 = $doc.Styles.Item("Heading 2")
    $h2.Font.Name = "Calibri"
    $h2.Font.Size = 13
    $h2.Font.Bold = 1
    $h2.ParagraphFormat.Alignment = 0
    $h2.ParagraphFormat.SpaceBefore = 12
    $h2.ParagraphFormat.SpaceAfter = 6

    $h3 = $doc.Styles.Item("Heading 3")
    $h3.Font.Name = "Calibri"
    $h3.Font.Size = 11
    $h3.Font.Bold = 1
    $h3.ParagraphFormat.Alignment = 0
    $h3.ParagraphFormat.SpaceBefore = 10
    $h3.ParagraphFormat.SpaceAfter = 4

    $sel = $word.Selection
    $sel.EndKey(6) | Out-Null

    function Add-Para {
        param(
            [string]$Text,
            [string]$Style = "Normal",
            [int]$Align = -1,
            [switch]$Code,
            [switch]$Bold,
            [double]$FontSize = 0
        )

        $script:sel.EndKey(6) | Out-Null
        $script:sel.Style = $Style

        if ($Align -ge 0) {
            $script:sel.ParagraphFormat.Alignment = $Align
        }

        if ($Code) {
            $script:sel.Font.Name = "Consolas"
            $script:sel.Font.Size = 10
            $script:sel.Font.Bold = 0
            $script:sel.ParagraphFormat.Alignment = 0
            $script:sel.ParagraphFormat.LeftIndent = 18
            $script:sel.ParagraphFormat.SpaceAfter = 3
        } else {
            $script:sel.Font.Name = "Calibri"
            if ($FontSize -gt 0) { $script:sel.Font.Size = $FontSize } else { $script:sel.Font.Size = 11 }
            $script:sel.Font.Bold = [int]$Bold.IsPresent
            $script:sel.ParagraphFormat.LeftIndent = 0
            if ($Style -eq "Normal" -and $Align -lt 0) {
                $script:sel.ParagraphFormat.Alignment = 3
            }
        }

        $script:sel.TypeText($Text)
        $script:sel.TypeParagraph()
    }

    function Is-TableSep {
        param([string]$Line)
        return ($Line -match '^\|\s*[:\-\| ]+\|\s*$')
    }

    function Parse-TableRow {
        param([string]$Line)
        $t = $Line.Trim()
        if ($t.StartsWith("|")) { $t = $t.Substring(1) }
        if ($t.EndsWith("|")) { $t = $t.Substring(0, $t.Length - 1) }
        $parts = $t -split '\|'
        $clean = @()
        foreach ($p in $parts) { $clean += $p.Trim() }
        return ,$clean
    }

    # Cover page
    $sel.Style = "Normal"
    $sel.ParagraphFormat.Alignment = 1
    $sel.Font.Name = "Calibri"
    $sel.Font.Size = 24
    $sel.Font.Bold = 1
    $sel.TypeText($title)
    $sel.TypeParagraph()
    $sel.TypeParagraph()

    $sel.Font.Size = 12
    $sel.Font.Bold = 0
    foreach ($cl in $coverLines) {
        if ($cl -match '^-\\s+(.*)$') {
            $sel.TypeText($Matches[1])
        } else {
            $sel.TypeText($cl)
        }
        $sel.TypeParagraph()
    }

    $sel.TypeParagraph()
    $sel.TypeText("Relatorio Tecnico")
    $sel.TypeParagraph()
    $sel.InsertBreak(7) # wdPageBreak

    $codeMode = $false
    $codeBuffer = @()
    $idx = 0

    while ($idx -lt $bodyLines.Count) {
        $line = $bodyLines[$idx]

        if ($line -match '^```') {
            if (-not $codeMode) {
                $codeMode = $true
                $codeBuffer = @()
            } else {
                foreach ($c in $codeBuffer) { Add-Para -Text $c -Code }
                Add-Para -Text ""
                $codeMode = $false
                $codeBuffer = @()
            }
            $idx++
            continue
        }

        if ($codeMode) {
            $codeBuffer += $line
            $idx++
            continue
        }

        if ($line -match '^\s*$') {
            Add-Para -Text ""
            $idx++
            continue
        }

        if ($line -match '^\s*---\s*$') {
            Add-Para -Text ""
            $idx++
            continue
        }

        if ($line -match '^###\s+(.*)$') {
            Add-Para -Text $Matches[1].Trim() -Style "Heading 3" -Align 0
            $idx++
            continue
        }

        if ($line -match '^##\s+(.*)$') {
            Add-Para -Text $Matches[1].Trim() -Style "Heading 2" -Align 0
            $idx++
            continue
        }

        if ($line -match '^#\s+(.*)$') {
            Add-Para -Text $Matches[1].Trim() -Style "Heading 1" -Align 0
            $idx++
            continue
        }

        if (($line -match '^\|.*\|\s*$') -and (($idx + 1) -lt $bodyLines.Count) -and (Is-TableSep $bodyLines[$idx + 1])) {
            $rows = @()
            $rows += ,(Parse-TableRow $line)
            $idx += 2
            while ($idx -lt $bodyLines.Count -and $bodyLines[$idx] -match '^\|.*\|\s*$') {
                $rows += ,(Parse-TableRow $bodyLines[$idx])
                $idx++
            }

            $colCount = 0
            foreach ($r in $rows) {
                if ($r.Count -gt $colCount) { $colCount = $r.Count }
            }

            if ($rows.Count -gt 0 -and $colCount -gt 0) {
                $range = $doc.Range($doc.Content.End - 1, $doc.Content.End - 1)
                $table = $doc.Tables.Add($range, $rows.Count, $colCount)
                $table.Style = "Table Grid"
                $table.Borders.Enable = 1
                $table.Rows.Alignment = 0

                for ($r = 1; $r -le $rows.Count; $r++) {
                    $vals = $rows[$r - 1]
                    for ($c = 1; $c -le $colCount; $c++) {
                        $txt = ""
                        if (($c - 1) -lt $vals.Count) { $txt = $vals[$c - 1] }
                        $cell = $table.Cell($r, $c)
                        $cell.Range.Text = $txt
                        $cell.Range.Font.Name = "Calibri"
                        $cell.Range.Font.Size = 10.5
                        $cell.Range.ParagraphFormat.Alignment = 0
                        if ($r -eq 1) { $cell.Range.Font.Bold = 1 }
                    }
                }

                $after = $doc.Range($doc.Content.End - 1, $doc.Content.End - 1)
                $after.InsertParagraphAfter() | Out-Null
            }
            continue
        }

        if ($line -match '^\s*-\s+(.*)$') {
            Add-Para -Text ("• " + $Matches[1].Trim()) -Style "Normal" -Align 0
            $idx++
            continue
        }

        if ($line -match '^\s*\d+\.\s+(.*)$') {
            Add-Para -Text $line.Trim() -Style "Normal" -Align 0
            $idx++
            continue
        }

        $paraLines = @($line.Trim())
        $j = $idx + 1
        while ($j -lt $bodyLines.Count) {
            $n = $bodyLines[$j]
            if ($n -match '^\s*$' -or
                $n -match '^\s*---\s*$' -or
                $n -match '^#' -or
                $n -match '^\s*-\s+' -or
                $n -match '^\s*\d+\.\s+' -or
                $n -match '^```' -or
                (($n -match '^\|.*\|\s*$') -and (($j + 1) -lt $bodyLines.Count) -and (Is-TableSep $bodyLines[$j + 1]))) {
                break
            }
            $paraLines += $n.Trim()
            $j++
        }

        $text = ($paraLines -join " ").Trim()
        Add-Para -Text $text -Style "Normal" -Align 3
        $idx = $j
    }

    $wdFormatDocumentDefault = 16
    $doc.SaveAs([ref]$outPath, [ref]$wdFormatDocumentDefault)
}
finally {
    if ($doc -ne $null) {
        $doc.Close()
        [void][System.Runtime.InteropServices.Marshal]::ReleaseComObject($doc)
    }
    if ($word -ne $null) {
        $word.Quit()
        [void][System.Runtime.InteropServices.Marshal]::ReleaseComObject($word)
    }
    [GC]::Collect()
    [GC]::WaitForPendingFinalizers()
    [GC]::Collect()
    [GC]::WaitForPendingFinalizers()
}

Get-Item $outPath | Select-Object FullName,Length,LastWriteTime
