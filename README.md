# Flash Guard

_Flash Guard is an adaptive dimming utility to dynamically darken bright screen contents_


### why?

As a chronic migraine sufferer, the sudden contrast difference from dark
applications to bright white pages is a big trigger.

### jUsT uSe DaRk ReAdEr

Sure, I used to use Dark Reader, but there are some issues with it:
1. It doesn't always render images, diagrams, etc. properly
2. Some pages don't work
3. Only works in your browser. What about applications that don't support dark
   mode?

### installation

>[!NOTE]
> Only X11 is supported

Clone the repo and build the project:
```
git clone https://github.com/buxxket/flashguard
cd flashguard
make install
```
This builds the binary, installs a systemd service to handle starting the
program, and creates a basic config file at `~/.config/flashguard/flashguard.config`.

### customisation

There are a few knobs you can turn to adjust the dimming speed and amount. The
config file lives in `~/.config/flashguard/flashguard.config`.

If you want to start from the shipped defaults, copy the file from the repo
root or let `make install` create one for you. A complete example config is
shown below:

```ini
# Flash Guard
#
# All values are optional. Remove a line to fall back to the built-in default.

fps=144 # update frequency in frames per second
min_brightness=0.6 # minimum screen brightness
max_brightness=1.0 # maximum screen brightness
curve_start_avg=0.10 # average luminance where dimming begins
curve_end_avg=0.95 # average luminance where dimming ends
curve_gamma=0.5 # curve steepness
curve_shoulder=0.35 # curve shoulder
avg_input_gamma=1.0 # input gamma applied to sampled brightness
avg_bias=0.0 # bias applied to sampled brightness
brightness_smoothing=0.0 # smoothing factor from 0 to 1
grid_x=40 # horizontal sample grid size
grid_y=24 # vertical sample grid size
```

The most useful settings are:

* `fps` for how often the screen is sampled.
* `min_brightness` and `max_brightness` for the allowed brightness range.
* `curve_start_avg` and `curve_end_avg` for where dimming starts and ends.
* `grid_x` and `grid_y` for the sampling grid size.

### starting

You can start the Flash Guard with `systemd`:

```sh
systemd --user start flashguard.service
```

To enable Flash Guard to start automatically when you login to your system, you
can enable the `systemd` service:

```sh
systemd --user enable --now flashguard.service
```

### stopping

You can stop Flash Guard with:

```sh
systemd --user stop flashguard.service
```

To disable auto-start, disable the `systemd` service:

```sh
systemd --user disable flashguard.service
```

### logs

Like any `systemd` service, you can view the logs via `journalctl`:

```sh
journalctl --user -fu flashguard.service
```
