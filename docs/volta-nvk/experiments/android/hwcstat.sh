# root on the board: print drm_hwcomposer stats since the previous call
dumpsys android.hardware.graphics.composer3.IComposer/default | sed -n '/since last dumpsys/,/Backends/p' | grep -vE "^$|Backends"
