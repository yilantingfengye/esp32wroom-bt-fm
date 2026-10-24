# Activate ESP-IDF 6.1 for this project in PowerShell.
#
# ESP-IDF 6.1 installed by EIM:
$env:IDF_PATH = "D:\esp\.espressif\v6.1\esp-idf"
$env:IDF_PYTHON_ENV_PATH = "C:\Users\chen\.espressif\python_env\idf6.1_py3.11_env"

# Avoid "environment is not configured to handle Unicode characters" warnings:
$env:PYTHONUTF8 = "1"

& "$env:IDF_PATH\export.ps1"
