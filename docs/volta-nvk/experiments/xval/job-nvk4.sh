#!/bin/bash
# NVK on the RTX 4060: hold the render node open so nouveau does not runtime-suspend
# the GPU between processes (resume left every job timing out)
exec 9</dev/dri/by-path/pci-0000:01:00.0-render
export VK_ICD_FILENAMES=$HOME/nvk-check/main-fixed/nouveau_icd.json
cd ~/xval; A=head B=new4c DEV=0 ./xval-pp.sh
echo ALL-DONE
