# Regole e Istruzioni Operative - Supra-iDrive6-Android-Auto (Fork)

Questo repository è il fork personale di sviluppo gestito da Giorgio Pais.

- **`origin`**: `git@github.com:atlas-ark/Supra-iDrive6-Android-Auto.git` (Fork personale)
- **`upstream`**: `https://github.com/Sq00ky/Supra-iDrive6-Android-Auto.git` (Repository padre)

## 📌 Istruzioni di Allineamento e Commit per l'Assistente AI
1. **Identità Git**:
   Ogni commit deve usare rigorosamente:
   - `user.name`: `Giorgio Pais`
   - `user.email`: `lambda.py@gmail.com`

2. **Allineamento preventivo con Upstream**:
   Prima di ogni operazione di commit/push, sincronizzare e allineare lo stato locale con `upstream`:
   ```bash
   git fetch upstream
   git rebase upstream/main
   ```
3. **Verifica Test e Standard QNX**:
   - Assicurarsi che `make -C milestone-7 check` passi senza errori.
   - Preservare la compatibilità con QNX SDP 6.5.0 GCC 4.4.2 (`-std=gnu++0x` / C++03).
