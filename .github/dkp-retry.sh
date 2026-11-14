# Reintenta un comando de dkp-pacman cuando pkg.devkitpro.org falla (403, cortes, "operation too slow"...).
# Uso:  . ./.github/dkp-retry.sh && dkp_retry dkp-pacman -S --needed --noconfirm switch-dev
dkp_use_curl() {
  conf=/opt/devkitpro/pacman/etc/pacman.conf
  [ -f "$conf" ] || return 0
  grep -q '^XferCommand' "$conf" && return 0
  sed -i '/^\[options\]/a XferCommand = /usr/bin/curl -fL --retry 3 -A "Mozilla/5.0 (X11; Linux x86_64; rv:128.0) Gecko/20100101 Firefox/128.0" -o %o %u' "$conf"
  echo "pacman: se descargará con curl y cabecera de navegador"
}

dkp_retry() {
  n=0
  until "$@"; do
    n=$((n + 1))
    if [ "$n" -ge 6 ]; then
      echo "Falló $n veces: $*"
      return 1
    fi
    [ "$n" -ge 2 ] && dkp_use_curl
    wait=$((n * 30))
    echo "Intento $n falló; reintento en ${wait}s..."
    sleep "$wait"
  done
}
