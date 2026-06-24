# Generiert eine komplexe C++ Datei fuer den Benchmark
$filePath = "$PSScriptRoot\benchmark_large.cpp"

$sb = [System.Text.StringBuilder]::new()
$sb.AppendLine("#include <iostream>") | Out-Null
$sb.AppendLine("#include <cmath>") | Out-Null
$sb.AppendLine("") | Out-Null

# 3000 mathematisch komplexe Funktionen generieren, um die Optimierung des Compilers auszulasten
for ($i = 0; $i -lt 3000; $i++) {
    $sb.AppendLine("double complexFunc_$i(double x) {") | Out-Null
    $sb.AppendLine("    double y = x;") | Out-Null
    for ($j = 0; $j -lt 50; $j++) {
        $sb.AppendLine("    y = std::sin(y) * $($j).5 + std::cos(y) * $($i).2;") | Out-Null
        $sb.AppendLine("    y = std::sqrt(std::abs(y)) + std::pow(y, 1.2);") | Out-Null
    }
    $sb.AppendLine("    return y;") | Out-Null
    $sb.AppendLine("}") | Out-Null
    $sb.AppendLine("") | Out-Null
}

# Main-Funktion hinzufügen, die alle aufruft
$sb.AppendLine("int main() {") | Out-Null
$sb.AppendLine("    double sum = 0;") | Out-Null
for ($i = 0; $i -lt 3000; $i++) {
    $sb.AppendLine("    sum += complexFunc_$i(1.5);") | Out-Null
}
$sb.AppendLine('    std::cout << "Result: " << sum << std::endl;') | Out-Null
$sb.AppendLine("    return 0;") | Out-Null
$sb.AppendLine("}") | Out-Null

[System.IO.File]::WriteAllText($filePath, $sb.ToString())
Write-Host "benchmark_large.cpp mit 200 komplexen Funktionen erfolgreich generiert!" -ForegroundColor Green
