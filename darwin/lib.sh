# shellcheck shell=bash
# Shared helpers for the interactive Darwin (iOS) build / sign / install scripts.
#
# This file is meant to be *sourced*, not executed.  All interactive prompts
# are gated behind is_interactive(), so the same scripts keep working
# unattended in CI and in Xcode build phases (where there is no terminal).
#
# Selection helpers print their menus/prompts to stderr and the chosen value
# to stdout, so callers can capture the result with: VAR="$(select_xxx)".

# Absolute path of the darwin/ directory (where .env lives).
DARWIN_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Log to stderr so it never pollutes a captured $(...) result.
log() { printf '%s\n' "$*" >&2; }

# True only when we have a usable terminal and are not running in CI.
is_interactive() {
  [[ -z "${CI:-}" ]] && [[ -r /dev/tty ]] && { [[ -t 0 ]] || [[ -t 1 ]]; }
}

# ask_yes_no "Question" -> returns 0 for yes, 1 for no (default no).
ask_yes_no() {
  local prompt="$1" reply
  printf '%s [y/N]: ' "$prompt" >&2
  read -r reply < /dev/tty || return 1
  [[ "$reply" =~ ^[Yy]([Ee][Ss])?$ ]]
}

# Persist KEY=VALUE into darwin/.env (replacing any existing entry for KEY).
# The value is shell-quoted so it round-trips through `source`.
save_env_var() {
  local key="$1" value="$2" file="$DARWIN_DIR/.env" tmp
  touch "$file"
  tmp="$(mktemp)"
  grep -v -E "^[[:space:]]*${key}=" "$file" > "$tmp" || true
  printf '%s=%q\n' "$key" "$value" >> "$tmp"
  mv "$tmp" "$file"
  log "💾 Saved $key to $file"
}

# Decode a provisioning profile and print one of its properties.
# Usage: _profile_property <file> <PlistBuddy-key>
_profile_property() {
  local file="$1" key="$2" tmp value=""
  tmp="$(mktemp)"
  if security cms -D -i "$file" -o "$tmp" 2>/dev/null; then
    value="$(/usr/libexec/PlistBuddy -c "Print $key" "$tmp" 2>/dev/null || true)"
  fi
  rm -f "$tmp"
  printf '%s' "$value"
}

# Interactively choose a provisioning profile from the standard locations.
# Prints the selected file path on stdout.
select_provisioning_profile() {
  local search_dirs=(
    "$HOME/Library/MobileDevice/Provisioning Profiles"
    "$HOME/Library/Developer/Xcode/UserData/Provisioning Profiles"
    "$DARWIN_DIR"
  )

  local files=() dir f
  for dir in "${search_dirs[@]}"; do
    [[ -d "$dir" ]] || continue
    while IFS= read -r -d '' f; do
      files+=("$f")
    done < <(find "$dir" -maxdepth 1 -type f \
      \( -name '*.mobileprovision' -o -name '*.provisionprofile' \) -print0 2>/dev/null)
  done

  if [[ ${#files[@]} -eq 0 ]]; then
    log "❌ No provisioning profiles found in the standard locations:"
    for dir in "${search_dirs[@]}"; do log "     $dir"; done
    log "   Download one from the Apple Developer portal (or let Xcode install it),"
    log "   then re-run, or set IOS_PROFILE_PATH manually."
    return 1
  fi

  log ""
  log "Available provisioning profiles:"
  local i=1 name appid
  for f in "${files[@]}"; do
    name="$(_profile_property "$f" ":Name")"
    appid="$(_profile_property "$f" ":Entitlements:application-identifier")"
    log "  $i) ${name:-<unnamed>}   [${appid:-?}]"
    log "       ${f}"
    i=$((i + 1))
  done

  local choice
  while true; do
    printf 'Select a provisioning profile [1-%d]: ' "${#files[@]}" >&2
    read -r choice < /dev/tty || return 1
    if [[ "$choice" =~ ^[0-9]+$ ]] && (( choice >= 1 && choice <= ${#files[@]} )); then
      break
    fi
    log "Invalid selection."
  done

  printf '%s\n' "${files[$((choice - 1))]}"
}

# Interactively choose a code-signing identity from the keychain.
# Prints the selected identity name on stdout.
select_signing_identity() {
  local output line names=()
  output="$(security find-identity -v -p codesigning 2>/dev/null || true)"
  while IFS= read -r line; do
    if [[ "$line" =~ \"([^\"]+)\" ]]; then
      names+=("${BASH_REMATCH[1]}")
    fi
  done <<< "$output"

  if [[ ${#names[@]} -eq 0 ]]; then
    log "❌ No code-signing identities found in your keychain."
    log "   Import your Apple Distribution certificate first, e.g.:"
    log "     security import certificate.p12 -k ~/Library/Keychains/login.keychain-db"
    return 1
  fi

  log ""
  log "Available signing identities:"
  local i=1 n
  for n in "${names[@]}"; do
    log "  $i) $n"
    i=$((i + 1))
  done

  local choice
  while true; do
    printf 'Select a signing identity [1-%d]: ' "${#names[@]}" >&2
    read -r choice < /dev/tty || return 1
    if [[ "$choice" =~ ^[0-9]+$ ]] && (( choice >= 1 && choice <= ${#names[@]} )); then
      break
    fi
    log "Invalid selection."
  done

  printf '%s\n' "${names[$((choice - 1))]}"
}

# Interactively choose a connected device.  Prints the device name on stdout.
select_device() {
  log ""
  log "Connected devices:"
  xcrun devicectl list devices >&2 2>/dev/null \
    || log "   (could not list devices; make sure the device is connected and trusted)"

  local name
  printf 'Enter the device name (or UDID) to install to: ' >&2
  read -r name < /dev/tty || return 1
  printf '%s\n' "$name"
}
