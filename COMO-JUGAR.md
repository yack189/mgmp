# Mewgenics co-op — cómo jugar

Mod no oficial para jugar Mewgenics entre dos personas. El host crea la
partida, el otro se une, y en combate cada uno mueve sus gatos.

## Lo que necesitan los dos

- **Windows**, 64 bits.
- **Mewgenics comprado en Steam**, la **misma versión** en las dos PCs
  (Steam lo actualiza solo; si uno tiene una versión distinta, el mod no deja
  conectar y lo dice).
- **Radmin VPN** (gratis), con los dos dentro de la **misma red** de Radmin.
  Así no hace falta abrir puertos en el router.

## Instalar

1. Descarga `mgmp-coop.zip` de la página de *Releases* del repositorio
   (la versión llamada **ultima-version**).
2. Descomprímelo en una carpeta cualquiera, por ejemplo `Documentos\MewgenicsCoop`.
3. Abre `mgmp.json` con el Bloc de notas y revisa la línea `"game"`: tiene que
   apuntar a tu `Mewgenics.exe`. Si instalaste Steam en otro disco, cámbiala.
   Las barras van dobles: `"D:\\SteamLibrary\\steamapps\\common\\Mewgenics\\Mewgenics.exe"`.
   (También puedes arrastrar `Mewgenics.exe` encima de `mgmp_loader.exe`.)
4. **Haz una copia de tus partidas guardadas** antes de la primera vez, por si acaso.

## Jugar

1. Abre **`mgmp_loader.exe`** (no el juego desde Steam). Abre el juego con el mod.
2. En el menú principal verás el botón **MULTIJUGADOR** arriba a la derecha.
3. Escribe tu nombre.
4. **El host** pulsa **CREAR PARTIDA**. Le aparece su IP; la que dice
   **Radmin** (empieza por `26.`) es la que tiene que pasarle a su amigo
   (botón **Copiar**).
5. **El amigo** pega esa IP y pulsa **UNIRSE**. Cuando conecte, en la ventana
   del host aparece su nombre en el segundo hueco.
6. El host pulsa **COMENZAR** y elige la partida guardada. El amigo entra solo,
   no tiene que tocar nada.

En combate, cuando le toca a un gato del otro jugador sale arriba
**"Es el turno de ..."**, y tú no puedes mover ese gato.

Abajo a la izquierda siempre ves con quién estás conectado.

**Para jugar solo**, abre el juego normal desde Steam (o pulsa **Jugar** en vez
de Multijugador).

## Probar en una sola PC

Puedes abrir `mgmp_loader.exe` **dos veces** en la misma PC: en una ventana
pulsa *Crear partida*, y en la otra escribe `127.0.0.1` y pulsa *Unirse*.

## Si algo falla

- **F1** abre el panel técnico (en inglés) con el estado de la conexión.
- Junto a `mgmp_loader.exe` se guarda un registro por partida,
  `mgmp_trace_FECHA-HORA.log`. Si algo sale mal, hace falta **el de las dos
  PCs** (el más reciente de cada una).
- El loader guarda lo que hizo en `mgmp_loader.log`, junto a él. Si algo falla,
  su ventana se queda abierta con el error hasta que pulses Enter.
- La primera vez, el loader crea un archivo `steam_appid.txt` junto a
  `Mewgenics.exe`. Sirve para que Steam no reinicie el juego sin el mod; no
  cambia nada del juego y Steam tiene que estar abierto igual.
