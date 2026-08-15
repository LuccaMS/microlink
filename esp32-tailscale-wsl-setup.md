# ESP32-S3 + MicroLink (Tailscale) — Setup via WSL2 + ESP-IDF

Guia de referência para colocar o ESP32-S3-N16R8 no tailnet usando o
projeto [MicroLink](https://github.com/CamM2325/microlink), com o ESP-IDF
rodando dentro do WSL2 (Ubuntu) no Windows.

## Hardware / ambiente

- Placa: **ESP32-S3-N16R8** (16MB flash, 8MB PSRAM octal)
- Porta USB: nativa (USB-Serial/JTAG do próprio chip) — aparece como
  `/dev/ttyACM0` no Linux, não `/dev/ttyUSBx`
- Toolchain: **ESP-IDF v5.3.5**, instalado manualmente (sem a extensão do
  VS Code)
- SO: WSL2 (Ubuntu) rodando no Windows

---

## 1. Dar acesso USB do Windows pro WSL2

O WSL2 roda numa VM separada e não enxerga portas COM/USB do Windows
automaticamente. É preciso compartilhar o dispositivo com o **usbipd-win**.

### 1.1. Instalar o usbipd-win (uma vez só)

No PowerShell **como Administrador**:

```powershell
winget install usbipd
```

Reinicie o terminal depois de instalar.

### 1.2. Instalar as ferramentas usbip dentro do WSL (uma vez só)

```bash
sudo apt install linux-tools-generic hwdata
sudo update-alternatives --install /usr/local/bin/usbip usbip /usr/lib/linux-tools/*/usbip 20
```

### 1.3. Listar os dispositivos USB (toda vez que quiser conferir)

No PowerShell (Admin):

```powershell
usbipd list
```

Saída esperada (o ESP32-S3 com porta USB nativa aparece como "USB JTAG/serial
debug unit"):

```
BUSID  VID:PID    DEVICE                                                        STATE
4-10   303a:1001  Dispositivo Serial USB (COM3), USB JTAG/serial debug unit     Shared
```

Anote o **BUSID** (no nosso caso, `4-10`).

### 1.4. Bind (uma vez por porta USB física)

```powershell
usbipd bind --busid 4-10
```

Isso só precisa ser feito uma vez por porta física do PC. Se você plugar o
ESP em outra porta USB, vai precisar dar bind de novo nela.

### 1.5. Attach (toda vez que reconectar o cabo ou reiniciar o PC/WSL)

```powershell
usbipd attach --wsl --busid 4-10
```

⚠️ **O attach não é permanente.** Ele se perde quando:
- você desconecta e reconecta o cabo USB
- o ESP32 reseta (inclusive reset feito pelo próprio `idf.py flash`)
- você roda `wsl --shutdown`
- você reinicia o PC

Sempre que a porta sumir do WSL, repita esse comando.

### 1.6. Confirmar que apareceu no WSL

```bash
lsusb | grep -i 303a      # 303a é o VID da Espressif
ls /dev/ttyACM*           # deve mostrar /dev/ttyACM0
```

---

## 2. Permissão de acesso à porta serial (grupo `dialout`)

O device `/dev/ttyACM0` pertence ao grupo `dialout`:

```bash
$ ls -l /dev/ttyACM0
crw-rw---- 1 root dialout 166, 0 Aug 15 00:18 /dev/ttyACM0
```

Se seu usuário não estiver nesse grupo, o `idf.py` dá o erro:

```
Error: Invalid value for '-p' / '--port': Path '/dev/ttyACM0' is not readable.
```

### 2.1. Adicionar seu usuário ao grupo (uma vez só)

```bash
sudo usermod -aG dialout $USER
```

### 2.2. Aplicar a mudança

Mudança de grupo só vale a partir de uma **sessão nova** — não basta digitar
o comando e continuar no mesmo terminal. É preciso:

1. Fechar **todas** as janelas/terminais WSL abertas (inclusive terminal
   integrado do VS Code)
2. No PowerShell: `wsl --shutdown`
3. Esperar uns 10 segundos
4. Abrir o WSL de novo

Confirmar com:

```bash
groups
# deve listar "dialout" entre os grupos
```

### 2.3. Saída rápida (sem mexer em grupo)

Se estiver com pressa e não quiser reiniciar o WSL:

```bash
sudo chmod 666 /dev/ttyACM0
```

Dá acesso de leitura/escrita pra qualquer usuário nesse device específico.
**Não é permanente** — some assim que o dispositivo é desconectado/
reconectado, então é só um atalho pontual, não substitui o passo 2.1/2.2.

---

## 3. Instalar o ESP-IDF via linha de comando (sem extensão do VS Code)

### 3.1. Pré-requisitos do sistema

```bash
sudo apt update
sudo apt install -y git wget flex bison gperf python3 python3-venv \
    python3-pip cmake ninja-build ccache libffi-dev libssl-dev dfu-util
```

### 3.2. Clonar o ESP-IDF

```bash
mkdir -p ~/esp && cd ~/esp
git clone -b v5.3.5 --recursive https://github.com/espressif/esp-idf.git
```

⚠️ Cuidado ao copiar o comando — não deixe pontuação sobrando no final da
linha (ex: `esp-idf.git.`), isso quebra o clone.

### 3.3. Instalar o toolchain

```bash
cd ~/esp/esp-idf
./install.sh esp32s3
```

### 3.4. Ativar o ambiente (toda vez que abrir um terminal novo)

```bash
source ~/esp/esp-idf/export.sh
idf.py --version   # confirma que funcionou
```

Dica — criar um alias pra não digitar o caminho inteiro sempre:

```bash
echo "alias get_idf='. \$HOME/esp/esp-idf/export.sh'" >> ~/.bashrc
source ~/.bashrc
# depois disso, em qualquer terminal novo:
get_idf
```

---

## 4. Configurar o projeto MicroLink (PSRAM + credenciais)

### 4.1. Clonar o projeto

```bash
git clone https://github.com/CamM2325/microlink.git
cd microlink/examples/basic_connect
```

### 4.2. Configurar PSRAM / partição (`sdkconfig.defaults`)

Criar (ou editar) o arquivo `sdkconfig.defaults` na raiz do projeto com o
conteúdo abaixo — esses valores já são os corretos para o **N16R8**
(16MB flash / 8MB PSRAM **octal**):

```ini
# PSRAM Configuration (required for ESP32-S3 with PSRAM)
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_TYPE_AUTO=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768

# Partition table (app needs ~1MB+)
CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE=y

# TLS/HTTPS (required for DERP and control plane)
CONFIG_ESP_TLS_USING_MBEDTLS=y
CONFIG_MBEDTLS_SSL_PROTO_TLS1_2=y
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_CMN=y

# Networking
CONFIG_LWIP_IPV4=y
CONFIG_LWIP_IP4_FRAG=y
CONFIG_LWIP_IP4_REASSEMBLY=y

# Stack size
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
```

### 4.3. Configurar credenciais (WiFi + Tailscale auth key)

O projeto traz um exemplo:

```bash
cp sdkconfig.credentials.example sdkconfig.credentials
```

Editar `sdkconfig.credentials`:

```ini
CONFIG_ML_WIFI_SSID="SeuWiFi"
CONFIG_ML_WIFI_PASSWORD="SuaSenha"
CONFIG_ML_TAILSCALE_AUTH_KEY="tskey-auth-..."
```

Gere a auth key em: https://login.tailscale.com/admin/settings/keys
(marque como reusável, facilita nos testes).

⚠️ **Ponto importante:** só criar o arquivo `sdkconfig.credentials` **não
é suficiente**. Existem duas pegadinhas:

1. Ele só é lido se você disser explicitamente pro build usar esse arquivo
   junto com o `sdkconfig.defaults`, via flag `-D SDKCONFIG_DEFAULTS=...`.
2. Os arquivos `*.defaults` só são aplicados **na primeira geração** do
   `sdkconfig`. Se você já rodou `idf.py build` antes (gerando um
   `sdkconfig` com os campos de credencial vazios), esse `sdkconfig` já
   existente passa a ser a fonte da verdade — o `sdkconfig.credentials`
   é ignorado, mesmo que você acabou de criá-lo.

Por isso, se você já buildou antes sem credenciais, é preciso apagar o
`sdkconfig` e a pasta `build` e gerar tudo de novo:

```bash
rm -rf build sdkconfig
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.credentials" build
```

Alternativa mais simples (evita depender do flag toda vez): pular o
arquivo `sdkconfig.credentials` e preencher direto via `menuconfig`, que
escreve direto no `sdkconfig`:

```bash
idf.py menuconfig
# → MicroLink V2 Configuration → Credentials
# preencher: WiFi SSID / WiFi Password / Tailscale Auth Key
# apertar "S" para salvar, depois sair
```

### 4.4. Conferir que as credenciais pegaram

```bash
idf.py menuconfig
# → MicroLink V2 Configuration → Credentials
# os campos devem aparecer preenchidos
```

---

## 5. Build, flash e monitor

```bash
cd ~/microlink/examples/basic_connect
idf.py set-target esp32s3     # só na primeira vez
idf.py -p /dev/ttyACM0 flash monitor
```

Para sair do monitor serial: `Ctrl+]`

### 5.1. Se travar em "waiting for download" depois do flash

Às vezes o reset via RTS pin não funciona direito no modo USB-Serial/JTOG
nativo do S3. Solução: apertar o botão físico de **RESET/EN** na placa,
ou desconectar/reconectar o cabo USB — e depois reabrir o monitor:

```bash
idf.py -p /dev/ttyACM0 monitor
```

⚠️ Qualquer desconecta/reconecta de cabo derruba o `attach` do usbipd —
repita o passo 1.5 (`usbipd attach --wsl --busid 4-10`) antes de tentar
de novo.

### 5.2. Log de sucesso esperado

Depois do WiFi conectar, o dispositivo deve se registrar no Tailscale e
receber um IP `100.x.x.x`. Para confirmar de outra máquina no tailnet:

```bash
tailscale status | grep esp32
tailscale ping <IP_DO_ESP32>
```

---

## Checklist rápido (depois que tudo já está configurado)

Sempre que for testar de novo, nessa ordem:

1. `usbipd attach --wsl --busid 4-10` (PowerShell, se a porta sumiu)
2. `source ~/esp/esp-idf/export.sh` (ou `get_idf` se criou o alias)
3. `cd ~/microlink/examples/basic_connect`
4. `idf.py -p /dev/ttyACM0 flash monitor`
