# Terminal Bluetooth (console série SPP)

Console de secours sans réseau : un `login:` shell accessible par Bluetooth
quand le SSH/WiFi ne répond plus. Repose sur le profil **Serial Port (SPP,
canal RFCOMM 1)** + un `agetty` sur `/dev/rfcomm0`.

> Le Bluetooth est **éteint au boot** (économie de temps de démarrage). Le
> bouton CONSOLE de l'UI et le scan le rallument automatiquement à la demande.

## 1. Activer la console côté Pi

Au choix :

- **UI** : carte **BLUETOOTH** → bouton **CONSOLE** (toggle).
- **SSH** : `sudo /usr/local/sbin/meshui-ctl bt-serial-on`

Ce que ça déclenche :

- démarrage de `bluetooth.service` ;
- Pi **découvrable + appairable** (nom `bq-lora`) ;
- `meshui-btserial.service` publie le record SPP via `sdptool add --channel=1 SP`
  (nécessite `bluetoothd --compat`, forcé par `bluetooth-compat.conf`) puis lance
  `rfcomm watch hci0 1 /sbin/agetty rfcomm0 115200 vt100`.

## 2. Appairer

Depuis le téléphone/PC, scanner et appairer avec **`bq-lora`** pendant que la
console est active.

## 3. Se connecter au port série (SPP)

| Plateforme | Procédure |
|------------|-----------|
| **Android** | App *Serial Bluetooth Terminal* → `bq-lora` → service "Serial Port". 115200 8N1 |
| **Windows** | L'appairage crée un **COM sortant** (Bluetooth → Ports COM). Ouvrir avec PuTTY en *Serial*, 115200 8N1 |
| **Linux**   | `sudo rfcomm connect hci0 <MAC_DU_PI> 1` puis `screen /dev/rfcomm0 115200` |

> `<MAC_DU_PI>` : via `bluetoothctl devices` ou l'écran de scan de l'UI.

## 4. Se logguer

Prompt `bq-lora login:` (c'est `agetty` sur `rfcomm0`, 115200) :

- identifiant : **`bq-lora`**
- mot de passe : celui du compte

→ shell complet, sans réseau ni SSH.

## 5. Couper la console

UI → bouton **CONSOLE** (off), ou `sudo meshui-ctl bt-serial-off`. À faire dès
que terminé : arrête le getty et rend le Pi non découvrable.

## Dépannage

- **Connexion SPP refusée** : presque toujours `bluetoothd` pas en mode `--compat`
  (sans ça `sdptool add SP` ne publie pas le record). Vérifier
  `systemctl cat bluetooth.service | grep compat` et la présence du drop-in
  `/etc/systemd/system/bluetooth.service.d/bluetooth-compat.conf`.
- **Pi non visible au scan** : la console n'est pas active → relancer le toggle
  ou `bt-serial-on`.
- **État du service** : `systemctl status meshui-btserial.service`.
