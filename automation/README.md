# WineHua Phase 2 automation

Run the core suite once:

```powershell
powershell -ExecutionPolicy Bypass -File \\wsl.localhost\Ubuntu\home\maple\Work\WineHua-build\automation\Invoke-WineHuaAutomation.ps1
```

Run the Phase 2 entry gate (three reuse-prefix core runs and one isolated
clean-prefix core run):

```powershell
powershell -ExecutionPolicy Bypass -File \\wsl.localhost\Ubuntu\home\maple\Work\WineHua-build\automation\Invoke-WineHuaAutomation.ps1 -Gate
```

The command builds only in `winehua-master-ext4`, validates the HAP payload and
architectures, installs through Windows HDC, starts the App with Want
parameters, validates the deterministic OpenGL fixed frame, and archives all
machine-readable results under `D:\MyProject\winehua-logs\automation`.
