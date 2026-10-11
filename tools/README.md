# tools

Scripts used while working out the protocols and testing the driver. Most
talk to the device directly with pyusb and need the kernel driver out of the
way: for the T6, `t6_run.sh` unbinds it, runs the script as root and binds it
again; for the T2, unbind it by hand (see the script's docstring). Every
script describes itself in its first lines.

Python dependencies: pyusb and numpy (some also pillow or capstone). The
flake's dev shell has them.

**Tests that change what a monitor shows can make it go dark or, with some
monitors, hang their on-screen menu until a power cycle. Read the script's
docstring first; the interactive ones wait for a y/n answer per step.**

## Trigger 6

| Script | What it does |
|---|---|
| `t6_run.sh` | wrapper: unbind the T6 from mct, run a test as root, bind it back |
| `t6dev.py` | small pyusb toolkit: status, EDID, mode table, set mode, raw frames |
| `t6_refresh_test.py` | custom refresh rates from MCT's PLL formula |
| `t6_formats_test.py` | packed surface formats: RGB32, BGR24, YUYV |
| `t6_yuv444_test.py` | YUV 4:4:4 against NV12 |
| `t6_yuv444_rect_test.py` | partial updates in 4:4:4 |
| `t6_rect_test.py`, `t6_partial_test.py` | session 7 rectangle writes and empty flips |
| `t6_partial_motion_test.py` | the driver's partial update scheme from userspace |
| `t6_yuy2_*.py` | YUYV investigation (row limit, byte order, flips) |
| `t6_gamma_test.py` | do gamma table requests work (they stall on feature level 6) |
| `t6_wide_test.py` | large modes (2560x1440, 3440x1440) in all formats |
| `t6_clock_sweep.py` | interactive mode sweeps: clock limits, 4:4:4 scanout limit, 4K30, mode flag |
| `t6_cursor_test.py` | interactive cursor test: negative positions, 96 and 128 px |
| `t6_pacing_test.py` | are frame-done events tied to the refresh (no) |
| `t6_cancel_test.sh` | what the cancel request (0x14) and a cut transfer do |
| `t6_robust_test.sh` | interactive: injected transfer errors, monitor off/on |
| `t6_modes.sh` | switch a niri output through modes, y/n per mode |
| `t6_edid_sim.sh`, `edid_gen.py` | which modes the driver offers typical monitors, via fake EDIDs (`edid_override`); run without a monitor that hangs on signal changes |
| `nv12_eval.py` | offline: NV12 chroma subsampling methods compared |
| `gbm_read_bench.c` | how fast the CPU reads GBM buffers of a display-only device |

## Trigger II (Wacom DTU-1031X)

| Script | What it does |
|---|---|
| `t2dev.py` | small pyusb toolkit for live T2 experiments |
| `t2rle.py` | the T2's planar RLE codec |
| `replay.py`, `show_image.py` | replay the Windows init, then show an image (`replay.py`, `gen_init_blob.py` and `timeline.py` need `captures/redir2.bin`, which is only in the development repository) |
| `gen_init_blob.py`, `gen_init_min.py`, `gen_init_header.py`, `init_trim.py` | extract, trim and convert the init sequence (`wacom-t2d/init.bin`, `kernel/mct_t2_init.h`) |
| `backlight_bisect.py` | find which init step turns the backlight on |
| `cursor_test.py` | hardware cursor experiments |
| `gamma_test.py` | is command 0x0a a gamma table |
| `t2i2c.py` | the T2's I2C masters |
| `bench.py` | throughput for different transfer sizes |
| `t2_recovery_test.py` | the stuck parser after an interrupted frame, and the driver's recovery |
| `probes/` | the first live probes, before any capture (own README) |

## Captures and the vendor driver

| Script | What it does |
|---|---|
| `tee.py` | TCP tee between QEMU's usb-redir and usbredirserver, logging everything |
| `parse_redir.py`, `redir_ops.py`, `redir_hid.py`, `timeline.py` | read those logs: transfers, HID requests, command timeline |
| `qmp.py` | small QMP client for the capture VM |
| `xref.py`, `vft.py`, `locfunc.py` | find functions in T2Indisp.dll (string xrefs, RTTI, .pdata) |

## Other

| Script | What it does |
|---|---|
| `ask.sh` | ask y/n questions one by one, print a summary |
