# Quick Start Guide

## Step 1: Install ESPHome

```bash
pip3 install esphome
```

## Step 2: Generate API Encryption Key

ESPHome needs an encryption key for the API. Generate one:

```bash
openssl rand -base64 32
```

Copy the output and paste it in `sauna-controller.yaml` at line 49:
```yaml
api:
  encryption:
    key: "YOUR_GENERATED_KEY_HERE"
```

## Step 3: First Flash (USB Required)

1. Connect your ESP32 via USB
2. Run:
   ```bash
   cd esphome-sauna
   esphome run sauna-controller.yaml
   ```
3. Select your USB port (usually `/dev/cu.usbserial-*` on Mac)
4. Wait 3-5 minutes for first compile and upload

## Step 4: Check the Logs

After flashing, logs will automatically stream. Look for:
- ✅ WiFi connected + IP address
- ✅ I2C scan results (should show 0x3C for display)
- ✅ Dallas sensor address detected
- ✅ Web server started

## Step 5: Access Web Interface

Open browser to: `http://sauna-controller.local`
- Username: `admin`
- Password: `sauna`

## Step 6: Test It Out

1. **Display**: Should show current temp and setpoint
2. **Encoder button**: Press to enter/exit adjustment mode
3. **Rotate encoder**: (in adjustment mode) Change setpoint
4. **Watch logs**: See heating decisions in real-time

## If Display Shows Scrambled

Add this to the i2c section in `sauna-controller.yaml`:

```yaml
i2c:
  sda: GPIO21
  scl: GPIO22
  scan: true
  frequency: 10kHz  # Ultra-slow for noisy power supplies
```

Then reflash:
```bash
esphome run sauna-controller.yaml
```

## Future Updates (OTA - No USB Needed)

Once the first flash is done:
```bash
esphome run sauna-controller.yaml
# Select "Over The Air" option
```

## Troubleshooting

### "Timed out waiting for acknowledgement"
- Try lower baud rate: add `upload_speed: 115200` under `esphome:` section
- Try different USB cable
- Press and hold BOOT button during upload

### Can't find device on network
- Check logs for IP address
- Use IP instead: `http://192.168.1.XXX`
- Check WiFi credentials in YAML

### Temperature shows "Unknown"
- Check logs for Dallas sensor address
- Copy address from logs to YAML sensor config

### Display still scrambled
- Lower I2C frequency to 10kHz or even 5kHz
- Check power supply to display
- Try external pull-up resistors (4.7kΩ) on SDA/SCL lines
