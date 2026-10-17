# TeamSpeak Plugin for Conan Exiles

Hello everyone,

I'm happy to share this TeamSpeak plugin for Conan Exiles, designed to bring immersive proximity-based voice communication into the game.

The project combines a TeamSpeak client plugin with a Conan Exiles mod to enable positional voice chat between players.

While some originally planned features have not been implemented, the core functionality is working and can be used on servers once the required components are properly installed and configured.

**Important Notice:**
The TeamSpeak plugin is currently not digitally signed. You may receive an unsigned-plugin warning during installation. Digital signing may be added in a future release.

Make sure to specify that BattlEye must be disabled.

The source code is available for further development, customization, and improvements.

Feedback, bug reports, suggestions, and contributions are always welcome. The goal is to continue improving the plugin and provide a reliable voice communication experience for the Conan Exiles community.

Thank you for your interest and support!

## Verify your download (SHA-256)

As a security measure, compare the SHA-256 hash of the file you downloaded with the value below **before** installing it. If the hashes differ, do not install the file and download it again from this repository's releases page.

| Version | File | SHA-256 |
|---|---|---|
| 8.0.7 | `conan_exiles-8.0.7-win64.ts3_plugin` (installer package) | `4345ab9d6b243ef280f5bab0afaa4372fe432d2f48da36d931dd2ac8a5ce6969` |
| 8.0.7 | `conan_exiles.dll` (inside the package, installed to `%APPDATA%\TS3Client\plugins`) | `9debfd6974ff80b15cad0e0ae1a97593b11b5c18c35410e22a781545f89a0124` |
| 8.0.6 | `conan_exiles-8.0.6-win64.ts3_plugin` (installer package) | `5241183845dcda022b105141cf84036c5092455f3105ecc2efed22afa290c138` |
| 8.0.6 | `conan_exiles.dll` (inside the package, installed to `%APPDATA%\TS3Client\plugins`) | `71c853e493abf3f7dc60510bc246812a010cc9fb14ef0bfdf283df28e1b36d60` |

Windows PowerShell:

```powershell
Get-FileHash .\conan_exiles-8.0.7-win64.ts3_plugin -Algorithm SHA256
```

Windows Command Prompt:

```bat
certutil -hashfile conan_exiles-8.0.7-win64.ts3_plugin SHA256
```

Linux / macOS / Git Bash:

```bash
sha256sum conan_exiles-8.0.7-win64.ts3_plugin
```

Since 8.0.7 every release also contains a `SHA256SUMS` file. Put it next to the downloaded package and run `sha256sum -c --ignore-missing SHA256SUMS` (the `conan_exiles.dll` line only applies after extracting the package).

The comparison is case-insensitive (PowerShell prints upper case). A hash only proves that the file is identical to the one published here; it does not replace trusting the source, so only use files from this repository's releases.

# I invite you to carefully read the [LICENSE](LICENSE) before any use.

https://discord.gg/tFBbQzmDaZ
