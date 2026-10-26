# Afterflow

by prsevend

a predictive input cheat that fixes your early clicks and makes them accurate

built for **geometry dash 2.2** (gd 2.2081 geode 5.11.2 windows). no menus and no in-game overlay: **f6** cycles stages in-level and **f9** toggles a terminal with a live log (kitty on linux/proton or a powershell log viewer on native windows). both keys are geode keybind settings so you can rebind them in mod settings or the keybinds popup

## what it does

geometry dash runs physics at a fixed **240 ticks per second**. afterflow keeps a hidden ghost player and for every press or release it tests the input at its current tick and at delays of 1..n ticks. if pressing now would kill you but a delayed version survives the input is held back and delivered on the surviving tick. the same check runs for releases so early clicks and early let-gos can both be corrected

the prediction understands cube, ball, ufo, spider, swing, wave, robot and ship movement plus orbs, pads, gravity portals, size portals, speed portals and click between frames' mid tick clicks. it never changes the real level state since ghosts are simulated in a detached layer and every object they touch is restored afterwards

## keybinds

afterflow uses geode keybind settings. open **mod settings → afterflow** (or geode's **keybinds** popup) to change them

| Setting | Default | Where |
| ------- | ------- | ----- |
| Cycle Stage | F6 | in-level |
| Terminal | F9 | everywhere |

the first launch shows a one time note with the current bindings and a shortcut to mod settings. a saved flag records that it was seen so it does not come back

## stages
press **f6** (or your bound cycle stage key) in level to cycle. the stage is remembered across sessions. the key also works while the terminal is focused

| Stage | All modes | Wave |
| ----- | --------- | ---- |
| 1 | 2 ticks | 2 ticks |
| 2 | 4 ticks | 4 ticks |
| 3 | 8 ticks | 8 ticks |
| 4 | 16 ticks | 8 ticks |
| 5 | 24 ticks | 12 ticks |
| 6 | 32 ticks | 16 ticks |
| 7 | 40 ticks | 20 ticks |
| 8 | off - 100% raw input | off |

at 60 fps one frame is 4 ticks so 4 ticks is `1.0f @ 60fps` and 16 ticks is `4.0f @ 60fps`
## terminal (f9)

the game runs as a windows process under proton so it cannot start a linux program directly. afterflow writes a live log to `/dev/shm/afterflow-<user>/terminal.log` (where `<user>` is your linux username) and drops a trigger file at `/dev/shm/afterflow-<user>/open-kitty`. `/dev/shm` is memory-backed, so the log and runtime files disappear on reboot; if `/dev/shm` is unavailable afterflow falls back to `/tmp/afterflow-<user>`. a small systemd user service watches the trigger and toggles the real **kitty** window on the host while tailing that log. the terminal key (default **f9**) opens the terminal when it is closed and closes it when it is open. it also works while the terminal is focused. the live messages are not duplicated to the geode console; only warnings and errors are logged there

the service is included as `afterflow-terminal-watch.sh` + `afterflow-terminal.service`:

```sh
install -Dm755 afterflow-terminal-watch.sh ~/.local/bin/afterflow-terminal-watch.sh
mkdir -p ~/.config/systemd/user
cp afterflow-terminal.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now afterflow-terminal.service
```

requires `kitty` on linux/proton. native windows uses powershell and needs no service. on hyprland the service also applies a floating centered window rule and raises the window above the game. on other compositors the log window still opens but placement is left to the window manager

## capture exclusion

capture exclusion is off by default and can be turned on with the **hide terminal from captures** setting. afterflow has two paths that keep the terminal visible on your monitor while removing it from captures:

- **hyprland (linux/proton):** the `hyprland-plugin/` directory builds a small compositor plugin. it hooks the render and screencopy path, skips `no_screen_share` windows in the mirror that `grim`, `wf-recorder`, OBS and Discord read, then draws those windows back into the local output. the terminal stays readable locally and the capture gets the wallpaper or desktop behind it, with no black box.
- **native windows:** the powershell terminal calls `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)` on its own window. OBS, Discord, game bar and other DWM/windows graphics capture based tools omit it while it stays visible locally.

when the setting is off the hyprland terminal uses the `afterflow-nocapture` window class and no `no_screen_share` rule is applied, and the windows viewer skips display affinity. macos has no capture exclusion path. on hyprland the watcher rewrites the named rule to match both classes each time it opens the terminal

### hyprland plugin

hyprland's built-in `no_screen_share` window rule alone draws an opaque black rectangle over the window in captures. the plugin is what replaces that rectangle with real exclusion. install both.

requirements: hyprland development headers for the running hyprland version (`hyprland` pkg-config package) and a c++ compiler. the plugin is abi-specific, so rebuild it after every hyprland upgrade. a header mismatch is rejected with a notification instead of loading a broken plugin.

```sh
make -C hyprland-plugin
make -C hyprland-plugin install   # ~/.local/lib/afterflow/afterflow-capture-exclusion.so
```

lua config (`~/.config/hypr/hyprland.lua`):

```lua
pcall(hl.plugin.load, os.getenv("HOME") .. "/.local/lib/afterflow/afterflow-capture-exclusion.so")
hl.window_rule({
    name = "afterflow-terminal",
    match = { class = "^(afterflow)$" },
    float = true,
    size = { "1000", "700" },
    no_screen_share = true,
})
```

legacy hyprlang config (`~/.config/hypr/hyprland.conf`). use the absolute path because `plugin =` does not expand `~`:

```
plugin = /home/youruser/.local/lib/afterflow/afterflow-capture-exclusion.so

windowrule {
    name = afterflow-terminal
    match:class = ^(afterflow)$
    float = 1
    size = 1000 700
    no_screen_share = 1
}
```

the watcher also applies the same named rule each time it opens the terminal, matching both `afterflow` and `afterflow-nocapture` and setting `no_screen_share` from the current capture setting, so it works in an existing session even if the rule is not in your config yet. the rule only marks the window; the loaded plugin is what removes the black box and keeps the terminal on the local output. verify with `hyprctl plugin list`.

while a capture is active the plugin redraws the terminal's raw surface after the mirror copy, so window decorations, shadows, blur and rounding may be missing from the local view. text and input still work. window sharing through the portal picker is still rejected for the terminal, and only monitor/region capture is affected.

## build

you need geode sdk 5.11.2 plus cmake 3.29+ plus clang plus the 2.2081 bindings

```sh
# linux cross compiling a windows mod
export GEODE_SDK=/path/to/geode-sdk
export GEODE_BINDINGS_REPO_PATH=/path/to/geode-bindings   # only needed for the 2.2081 binding fixes
geode build -p windows

# windows
set GEODE_SDK=C:\path\to\geode-sdk
geode build

# macos (native, builds arm64 and x86_64 by default)
export GEODE_SDK=/path/to/geode-sdk
geode build
```

the packaged mod ends up in `build-win/` when cross compiling from linux (`build/` on windows) and it also gets copied into the game's `geode/mods` folder automatically

## installation

copy the resulting .geode file into geometry dash's geode/mods folder and restart the game. enable **afterflow** in the geode mod list. the defaults work out of the box so you can rebind the two keys in mod settings if you want different ones

## credits

dynamic mod
whitechocolate
click before frames

lucide.dev for icon

## warnings

this is a cheat. it changes input timing so your runs are not legit. dont use it on rated levels or leaderboard submissions or anything you want to claim as legit. some levels and lists have input buffer or macro detectors so you can get flagged.
