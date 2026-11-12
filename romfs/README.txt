Esta carpeta se empaqueta dentro del .nro como romfs:/

Coloca aquí cacert.pem (https://curl.se/ca/cacert.pem) ANTES de compilar para que
las conexiones HTTPS con GameBanana se verifiquen. El workflow de GitHub Actions
lo descarga automáticamente. Sin ese archivo la app funciona, pero avisa de que
los certificados no se verifican.
