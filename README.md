# SoM Lite DEMO

Applicazione dimostrativa per SoM Lite. Il binario prodotto è `DEMO_UI`, gira su display Linux framebuffer (`/dev/fb0`) con touchscreen evdev (`/dev/input/event1`), risoluzione 1024×600.

---

## Struttura del progetto

| Percorso | Descrizione |
|---|---|
| `main.c` | Entry point dell'applicazione |
| `Makefile` | Makefile principale per la compilazione cross |
| `lv_conf.h` | File di configurazione della libreria LVGL |
| `lvgl/` | Sorgente della libreria LVGL 9.1 |
| `ui/` | Codice UI generato da SquareLine Studio 1.5.4 |
| `logic/` | Logica applicativa, suddivisa per area funzionale |
| `third_party/` | Librerie precompilate per il target aarch64 |

---

### `main.c`

Entry point dell'applicazione. Si occupa di:
- Inizializzare LVGL e configurare il display framebuffer (`/dev/fb0`) in modalità double-buffer full render.
- Registrare il dispositivo touch tramite evdev (`/dev/input/event1`) con calibrazione automatica del range RAW.
- Avviare il thread GLib per il loop DBus (necessario per il Bluetooth tramite BlueZ).
- Chiamare `ui_init()` per caricare la prima schermata.
- Eseguire il loop principale LVGL (`lv_timer_handler()` ogni 5 ms).

---

### `Makefile`

Makefile principale per la compilazione cross. Punti chiave:
- Applica automaticamente i flag `-march=armv8-a -mcpu=cortex-a55` solo se il compilatore è `aarch64-*`.
- Compila ricorsivamente tutti i `.c` presenti in `ui/` e `logic/`.
- Linka le librerie precompilate in `third_party/` (libyuv, GLib/GIO).
- Il binario prodotto è `DEMO_UI` nella root del progetto.

---

### `lv_conf.h`

File di configurazione di LVGL. Definisce quali moduli, widget, driver e font sono abilitati. Modificare questo file per attivare/disattivare funzionalità LVGL (es. monitor prestazioni, dimensione memoria, driver display).

---

### `lvgl/`

Sorgente della libreria **LVGL 9.1** inclusa direttamente nel repository. Non va modificata. Il Makefile la compila tramite il suo makefile ufficiale (`lvgl/lvgl.mk`).

---

### `ui/`

Codice generato da **SquareLine Studio 1.5.4**. Contiene la definizione grafica di tutti gli schermi e i componenti UI. **Non modificare manualmente** i file generati automaticamente; le modifiche vanno fatte da SquareLine Studio e riesportate.

I due file rilevanti per lo sviluppo sono:

| Percorso | Descrizione |
|---|---|
| `ui.h` | Header principale da includere nei moduli di logica per accedere agli oggetti degli schermi |
| `ui_events.h/.c` | Unico file da modificare manualmente: contiene le callback degli eventi UI in cui aggiungere le chiamate alle funzioni della logica applicativa |

---

### `logic/`

Tutta la logica applicativa del progetto, suddivisa per area funzionale. Ogni modulo espone funzioni `init` / `deinit` richiamate dagli eventi UI in `ui_events.c`.

**Header globale**

| Percorso | Descrizione |
|---|---|
| `logic.h` | Include comuni (stdio, lvgl, ui.h) e macro `DEBUG` / `INFO` / `ERROR` |

**`logic/media/` — Gestione multimedia**

| Percorso | Descrizione |
|---|---|
| `mediaLogic.h` | Header aggregatore del dominio media |
| `audio/audioLogic.h/.c` | Registrazione microfono (ALSA), riproduzione file WAV, file picker audio |
| `camera/cameraLogic.h/.c` | Acquisizione video da V4L2, decodifica YUYV→XRGB (libyuv), file picker RAW |
| `buzzer/buzzerLogic.h/.c` | Controllo buzzer tramite PWM, gestione stato ON/OFF da UI |
| `buzzer/buzzerButton.h/.c` | Thread per lettura pulsanti fisici da `/dev/input/event0` (gpio-keys-polled) |
| `buzzer/buzzerPwm.h/.c` | Wrapping low-level per attivazione/disattivazione PWM buzzer |

**`logic/connectivity/` — Connettività wireless e NFC**

| Percorso | Descrizione |
|---|---|
| `connectivityLogic.h/.c` | State machine Wi-Fi/Bluetooth, lista device, pairing popup |
| `wifi/wifiLogic.h/.c` | Scansione reti, connessione/disconnessione Wi-Fi tramite `wpa_supplicant` (socket Unix) |
| `wifi/wpa_ctrl.h/.c` | Implementazione del protocollo di controllo `wpa_supplicant`: apertura socket, invio comandi, ricezione eventi |
| `bth/bthLogic.h/.c` | Scansione, pairing/unpairing Bluetooth tramite BlueZ (DBus/GLib) |
| `nfc/nfcLogic.h/.c` | Thread di polling NFC, reset controller (PN7120/PN7150/PN7160), aggiornamento UI |
| `nfc/tml.h/.c` | Transport Message Layer: comunicazione I2C con il controller NFC |

**`logic/communications/` — Comunicazioni cablate**

| Percorso | Descrizione |
|---|---|
| `communicationsLogic.h` | Header aggregatore del dominio comunicazioni |
| `mainCommsLogic.h/.c` | State machine ETH/RS-485: init/deinit, log asincrono su textarea UI |
| `eth/ethLogic.h/.c` | Monitor interfaccia eth0, test throughput con iperf3 |
| `rs/rsLogic.h/.c` | Comunicazione RS-485, avvio processo slave, log output |
| `can/canLogic.h/.c` | Init/deinit modulo CAN, avvio thread RX (`can_rx_handler`) e thread loop principale (`can_main_loop`) |
| `can/can.h` | Definizioni ID messaggi, maschere e strutture del protocollo CAN proprietario |
| `can/can_RX.c` | Socket CAN (`can0`), filtri messaggi, parser frame e handler thread RX |
| `can/vehicle_types.h` | Strutture dati veicolo (`vehicle_t`, `my_struct_t`) condivise tra thread RX e loop principale |
| `can/display.h/.c` | Aggiornamento widget LVGL con dati CAN (velocità, icone, batteria...) |

---

### `third_party/`

Librerie precompilate per il target aarch64. **Non vanno ricompilate**: contengono sia file `.a` che `.so` con i relativi header. I `.so` sono presenti per evitare errori nell'IDE; a runtime verranno usati quelli presenti sul target.

**`third_party/libyuv/`**

| Percorso | Descrizione |
|---|---|
| `include/` | Header pubblici di libyuv |
| `lib/` | Libreria statica `libyuv.a` cross-compilata per aarch64 |

**`third_party/sysroot/`**

| Percorso | Descrizione |
|---|---|
| `usr/include/glib-2.0/` | Header GLib 2.62 (`glib.h`, `gio.h`...) |
| `usr/include/gpiod.h` | Header libgpiod (non attualmente linkato, commentato nel Makefile) |
| `usr/lib64/glib-2.0/include/glibconfig.h` | Configurazione GLib generata per aarch64 |
| `usr/lib64/*.so / *.a` | Librerie GLib/GIO/GObject precompilate per aarch64 |

---

## Come compilare

### Prerequisiti

La compilazione richiede la toolchain Yocto per il target **aarch64** (Cortex-A55). Prima di eseguire `make` è necessario attivare l'ambiente della toolchain.

### Step 1 — Attivare la toolchain Yocto

```bash
source /opt/poky/3.1.31/environment-setup-aarch64-poky-linux
```

Dopo il `source`, la variabile `CC` verrà impostata automaticamente al cross-compilatore aarch64 (es. `aarch64-poky-linux-gcc`). Il Makefile rileva il prefisso `aarch64` e applica automaticamente i flag `-march=armv8-a -mcpu=cortex-a55`.

### Step 2 — Compilare

Dalla root del progetto:

```bash
make
```

Il binario `DEMO_UI` verrà prodotto nella root del progetto.

### Step 3 — Pulizia

```bash
make clean
```

Rimuove il binario `DEMO_UI` e tutti i file oggetto `.o` generati.

---

## Note

- Il binario va eseguito sul target con display framebuffer attivo su `/dev/fb0` e touch su `/dev/input/event1`.
- I file audio demo devono trovarsi in `mediaDemo/audioDemo/` (percorso relativo alla directory di esecuzione). **La cartella va creata manualmente sul target prima di eseguire il binario.**
- I file video RAW devono trovarsi in `mediaDemo/videoDemo/`. **La cartella va creata manualmente sul target prima di eseguire il binario.**
- Il modulo NFC richiede il controller collegato via I2C (PN7120 / PN7150 / PN7160).
- Wi-Fi e Bluetooth richiedono rispettivamente `wpa_supplicant` e `BlueZ` attivi sul target.