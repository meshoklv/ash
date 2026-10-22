# Source me: . scripts/env.sh
# PSP toolchain (pspdev release v20261001, GCC 15.2). Override any variable before sourcing.
#   PSPDEV           where pspdev is installed       (default: $HOME/opt/pspdev)
#   ASH_BUILDTOOLS   optional extra cmake/ninja dir  (default: $HOME/opt/buildtools, ignored if missing)
#   PPSSPP_HEADLESS  PPSSPPHeadless binary, only for the emulator test scripts
export PSPDEV="${PSPDEV:-$HOME/opt/pspdev}"
export ASH_BUILDTOOLS="${ASH_BUILDTOOLS:-$HOME/opt/buildtools}"
export PPSSPP_HEADLESS="${PPSSPP_HEADLESS:-$HOME/opt/ppsspp/PPSSPPHeadless}"
case ":$PATH:" in *":$PSPDEV/bin:"*) ;; *) export PATH="$PSPDEV/bin:$PATH" ;; esac
[ -d "$ASH_BUILDTOOLS/bin" ] && case ":$PATH:" in *":$ASH_BUILDTOOLS/bin:"*) ;; *) export PATH="$PATH:$ASH_BUILDTOOLS/bin" ;; esac
true
