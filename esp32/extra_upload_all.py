Import("env")

def after_firmware_upload(source, target, env):
    print("\n=== Firmware uploaded. Uploading LittleFS (data/) ===\n")
    # To wgrywa obraz systemu plików z katalogu ./data
    env.Execute("pio run -e {} -t uploadfs".format(env["PIOENV"]))

# "upload" to standardowy target PlatformIO (wgrywa firmware).
# Po nim odpalamy uploadfs.
env.AddPostAction("upload", after_firmware_upload)
