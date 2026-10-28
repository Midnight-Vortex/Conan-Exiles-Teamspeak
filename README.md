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
| 8.0.15 | `conan_exiles-8.0.15-win64.ts3_plugin` (installer package) | `c710831f4dacf4228e5b463c0900d368e4940108e45600c6b6f524ba2de97886` |
| 8.0.15 | `conan_exiles.dll` (inside the package, installed to `%APPDATA%\TS3Client\plugins`) | `7b2fdb24820f5f15c4d866ea41f9a8a7f79d0628febc6373ed9bf964720bd632` |
| 8.0.14 | `conan_exiles-8.0.14-win64.ts3_plugin` (installer package) | `b83b23656b2f1a6ff251ec1556c40d85e75770d960ba0dca97118d0e4164dd2c` |
| 8.0.14 | `conan_exiles.dll` (inside the package, installed to `%APPDATA%\TS3Client\plugins`) | `d3839f487a960da139ec32fbb47627a81ef9311b1eb6d634a7c5e8ce41b1d2fc` |
| 8.0.13 | `conan_exiles-8.0.13-win64.ts3_plugin` (installer package) | `fabdd28ab5009a0ac3e68a4291b029ee72f6fe6386d01f52d9ba759b9aab8368` |
| 8.0.13 | `conan_exiles.dll` (inside the package, installed to `%APPDATA%\TS3Client\plugins`) | `6cc2430ee4875b51e8e958276d1488ff0c0de4d0f454478c63338e8c0c7cfc84` |
| 8.0.12 | `conan_exiles-8.0.12-win64.ts3_plugin` (installer package) | `7b008c26a416ae7d9f0d94780ad3389b40cca5e605764273e904f1baf9159806` |
| 8.0.12 | `conan_exiles.dll` (inside the package, installed to `%APPDATA%\TS3Client\plugins`) | `9bcc36e63cc18f4b4370c10fc047f6818562094ce35fb713e47c691fd7e67dff` |
| 8.0.11 | `conan_exiles-8.0.11-win64.ts3_plugin` (installer package) | `1af09c3a707d040a86130052f3052892f1a7659a40d541069e570d9d910b31e6` |
| 8.0.11 | `conan_exiles.dll` (inside the package, installed to `%APPDATA%\TS3Client\plugins`) | `5dcde1ce3762dfeaf976949cfff254e4accfcb8b303eba0db72a31c0a1f770a7` |
| 8.0.10 | `conan_exiles-8.0.10-win64.ts3_plugin` (installer package) | `c9fcc2e56c4eb47c594075830e8de4d1057226fab0c648e22022f784c782818d` |
| 8.0.10 | `conan_exiles.dll` (inside the package, installed to `%APPDATA%\TS3Client\plugins`) | `f243b9765080132f5bd8df25b7d06e6ff9fa4bf9070b40b0d43ec1268c79bb4b` |
| 8.0.9 | `conan_exiles-8.0.9-win64.ts3_plugin` (installer package) | `08628397ee61b71d1ee7ed9763f00fc460b3d2f49095c9a00f623ac8bbfa6859` |
| 8.0.9 | `conan_exiles.dll` (inside the package, installed to `%APPDATA%\TS3Client\plugins`) | `cc246496ed7649c47583478fb91f9f5fe00d8d8efbb41aff20b5bb75d6f18e23` |
| 8.0.8 | `conan_exiles-8.0.8-win64.ts3_plugin` (installer package) | `fa9b076b2aa35231a1f1e9787c7a8fdc3b490899cc1058cd05120fc50180deec` |
| 8.0.8 | `conan_exiles.dll` (inside the package, installed to `%APPDATA%\TS3Client\plugins`) | `77e72008f15703032e9b5659197c0dae3bf9078b0be99ae9139b52fda1d43b68` |
| 8.0.7 | `conan_exiles-8.0.7-win64.ts3_plugin` (installer package) | `4345ab9d6b243ef280f5bab0afaa4372fe432d2f48da36d931dd2ac8a5ce6969` |
| 8.0.7 | `conan_exiles.dll` (inside the package, installed to `%APPDATA%\TS3Client\plugins`) | `9debfd6974ff80b15cad0e0ae1a97593b11b5c18c35410e22a781545f89a0124` |
| 8.0.6 | `conan_exiles-8.0.6-win64.ts3_plugin` (installer package) | `5241183845dcda022b105141cf84036c5092455f3105ecc2efed22afa290c138` |
| 8.0.6 | `conan_exiles.dll` (inside the package, installed to `%APPDATA%\TS3Client\plugins`) | `71c853e493abf3f7dc60510bc246812a010cc9fb14ef0bfdf283df28e1b36d60` |

Windows PowerShell:

```powershell
Get-FileHash .\conan_exiles-8.0.15-win64.ts3_plugin -Algorithm SHA256
```

Windows Command Prompt:

```bat
certutil -hashfile conan_exiles-8.0.15-win64.ts3_plugin SHA256
```

Linux / macOS / Git Bash:

```bash
sha256sum conan_exiles-8.0.15-win64.ts3_plugin
```

Since 8.0.7 every release also contains a `SHA256SUMS` file. Put it next to the downloaded package and run `sha256sum -c --ignore-missing SHA256SUMS` (the `conan_exiles.dll` line only applies after extracting the package).

The comparison is case-insensitive (PowerShell prints upper case). A hash only proves that the file is identical to the one published here; it does not replace trusting the source, so only use files from this repository's releases.

# I invite you to carefully read the [LICENSE](LICENSE) before any use.

https://discord.gg/tFBbQzmDaZ
