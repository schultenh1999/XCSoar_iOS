# 0. One-time: build-tool deps (cmake, ninja, automake, lua, …)
./ide/provisioning/install-darwin-packages.sh BASE IOS

# 1. Build the UNSIGNED ipa  (first run is slow: it builds SDL2, Boost,
#    FreeType … from source for iOS — ~30–60 min, cached afterwards)
gmake TARGET=IOS64 ipa
#    → output/IOS64/xcsoar.ipa

# 2. Sign it — prompts you to pick a profile + signing identity,
#    then offers to save them to darwin/.env
./darwin/sign.sh
#    → output/IOS64/xcsoar-signed.ipa

# 3. Install on a connected device — prompts you to pick the device,
#    installs, and streams the app log
./darwin/install.sh