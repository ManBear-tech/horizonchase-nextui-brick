#!/bin/sh
# Horizon Chase (Unity 2022.3.33f1 IL2CPP) — launcher universal.
# O binário negocia fbdev/Mali, KMSDRM, Wayland e GLES em runtime.
set -u
GAMEDIR=${HC_GAMEDIR:-/storage/roms/ports/horizonchase}
export HC_GAMEDIR=$GAMEDIR
cd "$GAMEDIR" || { echo "sem $GAMEDIR"; exit 1; }

# A trava acompanha o processo pelo descritor 9 (inclusive após exec). Um segundo
# launcher aborta antes de tocar na instância que já possui Mali/KMS/fbdev.
exec 9>"$GAMEDIR/.horizonchase.lock"
flock -n 9 || { echo "ABORTO: outro launcher do Horizon já está ativo"; exit 1; }

# Nunca lançar sobre instância viva. Confere executable, cwd/comm e também o
# executable marcado "(deleted)" após uma eventual substituição de arquivo.
hc_pids() {
  for p in /proc/[0-9]*; do
    e=$(readlink "$p/exe" 2>/dev/null || true)
    case "$e" in
      "$GAMEDIR/horizonchase"*) echo "${p##*/}"; continue ;;
    esac
    c=$(cat "$p/comm" 2>/dev/null || true)
    # Apply the stderr redirection before opening cmdline: /proc entries may
    # disappear between enumeration and read on short-lived system processes.
    a=$(tr '\0' ' ' 2>/dev/null < "$p/cmdline" || true)
    d=
    case "$c:$a" in
      UnityMain:*|horizonchase:*|*:"./horizonchase "*|*:"$GAMEDIR/horizonchase "*)
        d=$(readlink "$p/cwd" 2>/dev/null || true)
        ;;
    esac
    case "$d:$c" in
      "$GAMEDIR:UnityMain"|"$GAMEDIR:horizonchase")
        echo "${p##*/}"; continue ;;
    esac
    case "$d:$a" in
      "$GAMEDIR:./horizonchase "*|"$GAMEDIR:$GAMEDIR/horizonchase "*)
        echo "${p##*/}" ;;
    esac
  done
}
old_pids=$(hc_pids)
if [ -n "$old_pids" ]; then
  for pid in $old_pids; do
    echo "[run] encerrando instância anterior pid=$pid"
    kill "$pid" 2>/dev/null || true
  done
  i=0
  while [ "$i" -lt 20 ]; do
    alive=
    for pid in $old_pids; do [ -d "/proc/$pid" ] && alive="$alive $pid"; done
    [ -z "$alive" ] && break
    sleep 0.5
    i=$((i+1))
  done
  for pid in $alive; do
    echo "[run] forçando encerramento da instância anterior pid=$pid"
    kill -9 "$pid" 2>/dev/null || true
  done
  remaining=$(hc_pids)
  [ -z "$remaining" ] ||
    { echo "ABORTO: instância viva ($remaining)"; exit 1; }
fi

if [ -n "${LD_LIBRARY_PATH:-}" ]; then
  export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:$GAMEDIR
else
  export LD_LIBRARY_PATH=/usr/lib:$GAMEDIR
fi
# Unity/FMOD create many short-lived worker threads. Two malloc arenas avoid
# retaining one fragmented glibc heap per thread on the 1 GB Mali-450 target.
export MALLOC_ARENA_MAX=${MALLOC_ARENA_MAX:-2}
# UnityPlayer is driven by Android Choreographer at 30 Hz in this title. Keep
# that native cadence on every backend unless an explicit engineering override
# is supplied; an unlimited host loop wastes CPU/GPU and inflates driver queues.
export HC_FRAME_LIMIT=${HC_FRAME_LIMIT:-30}

# Resolução real, sem assumir o painel do NextOS. O EGL usa o drawable como
# autoridade final; estas variáveis alimentam o JNI antes da janela existir.
_m=
[ -r /sys/class/graphics/fb0/mode ]  && read -r _m < /sys/class/graphics/fb0/mode  || true
[ -z "$_m" ] && [ -r /sys/class/graphics/fb0/modes ] && read -r _m < /sys/class/graphics/fb0/modes || true
if [ -n "$_m" ]; then
  _p=${_m#*:}; _p=${_p%%[!0-9x]*}; _w=${_p%x*}; _h=${_p#*x}
  case "$_w" in ''|*[!0-9]*) _w= ;; esac
  case "$_h" in ''|*[!0-9]*) _h= ;; esac
fi
if [ -z "${_w:-}" ] || [ -z "${_h:-}" ]; then
  for _status in /sys/class/drm/card*-*/status; do
    [ -r "$_status" ] || continue
    read -r _connected < "$_status" || true
    [ "$_connected" = connected ] || continue
    _modes=${_status%/status}/modes
    [ -r "$_modes" ] || continue
    read -r _mode < "$_modes" || true
    _w=${_mode%x*}; _h=${_mode#*x}
    case "$_w" in ''|*[!0-9]*) _w= ;; esac
    case "$_h" in ''|*[!0-9]*) _h= ;; esac
    [ -n "${_w:-}" ] && [ -n "${_h:-}" ] && break
  done
fi
if { [ -z "${_w:-}" ] || [ -z "${_h:-}" ]; } && [ -r /sys/class/graphics/fb0/virtual_size ]; then
  IFS=, read -r _w _vh < /sys/class/graphics/fb0/virtual_size || true
  case "${_w:-}" in ''|*[!0-9]*) _w= ;; esac
  case "${_vh:-}" in ''|*[!0-9]*) _vh= ;; esac
  if [ -n "${_w:-}" ] && [ -n "${_vh:-}" ]; then
    _h=$_vh
    # 1280x1440 e 640x960 são double-buffer; 640x480 não é.
    if [ "$_vh" -gt "$_w" ] && [ $((_vh % 2)) -eq 0 ]; then
      _half=$((_vh / 2))
      if [ "$_half" -le "$_w" ] && [ $((_half * 2)) -ge "$_w" ]; then
        _h=$_half
      fi
    fi
  fi
fi
[ -n "${_w:-}" ] && [ -n "${_h:-}" ] && export TER_SCREEN_W="$_w" TER_SCREEN_H="$_h"
echo "[run] fb real = ${TER_SCREEN_W:-?}x${TER_SCREEN_H:-?}"

# log em arquivo pelo SHELL (o dup2 interno trava a init — lição do terraria)
export TER_NOSTORAGEPATCH=1   # 🚫 patch de offset do Terraria NUNCA nesta libunity
export CUP_NOLOGFILE=1
export CUP_FRAMES=${CUP_FRAMES:-0}
# Em aparelho KMS com menos de ~1,25 GB físicos (perfil R36S de 1 GB), atlases
# crus/Alpha8 usam teto 512. O Mali-450 de pouca RAM mantém o perfil validado
# 768. KMS com >=1,7 GB deixa texturas na resolução original; ASTC/ETC2 seguem
# comprimidos em qualquer perfil que a GPU suporte.
if [ -z "${CUP_TEXHALF:-}" ]; then
  _mem_kb=$(awk '/^MemTotal:/ {print $2; exit}' /proc/meminfo 2>/dev/null || true)
  case "${_mem_kb:-}" in ''|*[!0-9]*) _mem_kb=0 ;; esac
  _has_drm=0
  for _card in /dev/dri/card[0-9]*; do
    [ -e "$_card" ] && { _has_drm=1; break; }
  done
  if [ "$_has_drm" -eq 1 ] && [ "$_mem_kb" -gt 0 ] &&
     [ "$_mem_kb" -lt 1250000 ]; then
    CUP_TEXHALF=512
  elif [ "$_has_drm" -eq 1 ] && [ "$_mem_kb" -ge 1700000 ]; then
    unset CUP_TEXHALF
  else
    CUP_TEXHALF=768
  fi
fi
[ -n "${CUP_TEXHALF:-}" ] && export CUP_TEXHALF || unset CUP_TEXHALF
# Horizon's GLES2 sprite variant asks for a split external-alpha sampler even
# after the unsupported ASTC atlases have been decoded to ordinary RGBA. On
# this build that sampler is a zero-filled dummy, making affected sprites
# transparent. Use the alpha already present in the decoded main texture.
export CUP_ALPHAFIX=${CUP_ALPHAFIX:-1}
# FMOD's Android streaming worker cannot run without ART/JVM. The native
# createSound call is retried as a resident sample, preserving the original
# music and SFX while the AudioTrack shim feeds SDL/Pulse.
export HC_STREAM_FALLBACK=${HC_STREAM_FALLBACK:-1}

for hc_pulse_socket in /var/run/pulse/native /run/pulse/native; do
  if [ -S "$hc_pulse_socket" ]; then
    export PULSE_SERVER="unix:$hc_pulse_socket"
    break
  fi
done
# O Android/Unity enumera uma identidade única e estável; o estado físico é
# normalizado pelo bridge do Horizon para o layout Xbox/XInput.
export TER_GAMEPAD=1
# GCOFF só quando pedido de verdade (getenv("") também conta como ligado).
# ⚠️ CUP_GCOFF chamava il2cpp_gc_disable por OFFSET do Terraria -> .rodata no HC = SIGSEGV.
# Agora resolve por nome, mas segue OFF por padrão (não precisamos desligar o GC).
[ -n "${CUP_GCOFF:-}" ] && export CUP_GCOFF || unset CUP_GCOFF
exec ./horizonchase
