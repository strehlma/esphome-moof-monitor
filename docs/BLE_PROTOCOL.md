# VanMoof S3/SX3 BLE Protocol Documentation

This document describes the Bluetooth Low Energy (BLE) protocol used by VanMoof S3 and SX3 bikes.

**Source**: Extracted from [pymoof](https://github.com/quantsini/pymoof) and [VanMoofKit](https://github.com/SvenTiigi/VanMoofKit). UUIDs and formats marked *confirmed* were verified on two S3 bikes (firmware 1.09.03) in November 2026.

## UUID Pattern

All VanMoof BLE UUIDs follow this pattern:
```
6acc55XX-e631-4069-944d-b8ca7598ad50
```

Where `XX` identifies the specific service or characteristic.

## Services

| Service | UUID | Description |
|---------|------|-------------|
| Security | `6acc5500-e631-4069-944d-b8ca7598ad50` | Authentication and encryption |
| Defense | `6acc5520-e631-4069-944d-b8ca7598ad50` | Lock, alarm, and security features |
| Movement | `6acc5530-e631-4069-944d-b8ca7598ad50` | Speed, distance, power level |
| BikeInfo | `6acc5540-e631-4069-944d-b8ca7598ad50` | Battery, firmware, frame number |
| BikeState | `6acc5560-e631-4069-944d-b8ca7598ad50` | Module state, errors, configuration |
| Sound | `6acc5570-e631-4069-944d-b8ca7598ad50` | Bell, sounds, volume |
| Light | `6acc5580-e631-4069-944d-b8ca7598ad50` | Light mode and sensor |

## Characteristics

### Security Service (6acc55XX, XX = 01-05)

| Characteristic | UUID | Access | Encrypted | Description |
|----------------|------|--------|-----------|-------------|
| Challenge | `6acc5501-e631-4069-944d-b8ca7598ad50` | Read | No | Returns nonce for authentication |
| Key Index | `6acc5502-e631-4069-944d-b8ca7598ad50` | Write | Yes | Write encrypted authentication payload |
| Backup Code | `6acc5503-e631-4069-944d-b8ca7598ad50` | Read/Write | Yes | Backup unlock code |

### Defense Service (6acc55XX, XX = 21-24)

| Characteristic | UUID | Access | Encrypted | Description |
|----------------|------|--------|-----------|-------------|
| Lock State | `6acc5521-e631-4069-944d-b8ca7598ad50` | Read | Yes | Current lock state (locked/unlocked) |
| Unlock Request | `6acc5522-e631-4069-944d-b8ca7598ad50` | Write | Yes | Request to unlock bike |
| Alarm State | `6acc5523-e631-4069-944d-b8ca7598ad50` | Read | Yes | Alarm setting: 0 off, 1 manual, 2 automatic |
| Alarm Mode | `6acc5524-e631-4069-944d-b8ca7598ad50` | Read/Write | Yes | 0 disarmed, 1 armed, 2-4 alarm stage 1-3 |

### Movement Service (6acc55XX, XX = 31-38)

| Characteristic | UUID | Access | Encrypted | Description |
|----------------|------|--------|-----------|-------------|
| Distance | `6acc5531-e631-4069-944d-b8ca7598ad50` | Read | Yes | Total distance in meters (uint32) |
| Speed | `6acc5532-e631-4069-944d-b8ca7598ad50` | Read | Yes | Current speed |
| Unit System | `6acc5533-e631-4069-944d-b8ca7598ad50` | Read/Write | Yes | Metric/Imperial |
| Power Level | `6acc5534-e631-4069-944d-b8ca7598ad50` | Read/Write | Yes | Motor assist level (0-4) |
| Speed Limit | `6acc5535-e631-4069-944d-b8ca7598ad50` | Read/Write | Yes | Maximum speed limit |
| E-Shifter Gear | `6acc5536-e631-4069-944d-b8ca7598ad50` | Read | Yes | Current gear (for e-shifter models) |
| E-Shifting Points | `6acc5537-e631-4069-944d-b8ca7598ad50` | Read/Write | Yes | Gear shift points configuration |
| E-Shifter Mode | `6acc5538-e631-4069-944d-b8ca7598ad50` | Read/Write | Yes | E-shifter operating mode |

### BikeInfo Service (6acc55XX, XX = 41-52)

Firmware versions are UTF-8 text in the decrypted, zero-padded block (`6acc554a` on an S3: `1.09.03`, *confirmed*). `6acc5545` exists on the S3, but reading it fails with status 14 (0x0E, unlikely error).

| Characteristic | UUID | Access | Encrypted | Description |
|----------------|------|--------|-----------|-------------|
| Motor Battery Level | `6acc5541-e631-4069-944d-b8ca7598ad50` | Read | Yes | Motor battery percentage (0-100) |
| Motor Battery State | `6acc5542-e631-4069-944d-b8ca7598ad50` | Read | Yes | 0 not charging, 1 charging (*confirmed*) |
| Module Battery Level | `6acc5543-e631-4069-944d-b8ca7598ad50` | Read | Yes | Module battery percentage (*confirmed*) |
| Module Battery State | `6acc5544-e631-4069-944d-b8ca7598ad50` | Read | Yes | 0 not charging, 1 charging (*confirmed*) |
| Firmware Bike | `6acc554a-e631-4069-944d-b8ca7598ad50` | Read | Yes | Bike firmware version (*confirmed*) |
| Firmware BLE Chip | `6acc554b-e631-4069-944d-b8ca7598ad50` | Read | Yes | BLE chip firmware |
| Firmware Controller | `6acc554c-e631-4069-944d-b8ca7598ad50` | Read | Yes | Motor controller firmware |
| Firmware PCBA | `6acc554d-e631-4069-944d-b8ca7598ad50` | Read | Yes | PCBA hardware version |
| Firmware GSM | `6acc554e-e631-4069-944d-b8ca7598ad50` | Read | Yes | GSM module firmware |
| Firmware E-Shifter | `6acc554f-e631-4069-944d-b8ca7598ad50` | Read | Yes | E-shifter firmware |
| Firmware Battery | `6acc5550-e631-4069-944d-b8ca7598ad50` | Read | Yes | Battery firmware |
| Frame Number | `6acc5552-e631-4069-944d-b8ca7598ad50` | Read | **No** | Bike frame number (serial) |

### BikeState Service (6acc55XX, XX = 61-67)

| Characteristic | UUID | Access | Encrypted | Description |
|----------------|------|--------|-----------|-------------|
| Module Mode | `6acc5561-e631-4069-944d-b8ca7598ad50` | Read/Write | Yes | Module operating mode |
| Module State | `6acc5562-e631-4069-944d-b8ca7598ad50` | Read/Write | Yes | Byte 0: 0 on, 1 off, 2 shipping, 3 standby, 4-6 alarm 1-3, 7 sleeping, 8 tracking (0/1 *confirmed*); byte 1 unknown (3 or 7 seen) |
| Errors | `6acc5563-e631-4069-944d-b8ca7598ad50` | Read | Yes | One error code in byte 0, 0 = none (*confirmed*) |
| Wheel Size | `6acc5564-e631-4069-944d-b8ca7598ad50` | Read/Write | Yes | Wheel size (an S3 reports 1) |
| Clock | `6acc5567-e631-4069-944d-b8ca7598ad50` | Read/Write | Yes | Bike clock/time |

### Sound Service (6acc55XX, XX = 71-74)

| Characteristic | UUID | Access | Encrypted | Description |
|----------------|------|--------|-----------|-------------|
| Play Sound | `6acc5571-e631-4069-944d-b8ca7598ad50` | Write | Yes | Trigger sound playback |
| Sound Volume | `6acc5572-e631-4069-944d-b8ca7598ad50` | Read/Write | Yes | Volume level 0-100 |
| Bell Sound | `6acc5574-e631-4069-944d-b8ca7598ad50` | Read/Write | Yes | Bell sound selection |

### Light Service (6acc55XX, XX = 81-84)

| Characteristic | UUID | Access | Encrypted | Description |
|----------------|------|--------|-----------|-------------|
| Light Mode | `6acc5581-e631-4069-944d-b8ca7598ad50` | Read/Write | Yes | Light mode (always on, auto, off) |
| Light Sensor | `6acc5584-e631-4069-944d-b8ca7598ad50` | Read | Yes | Ambient light sensor value |

## Authentication Process

VanMoof bikes use AES-128 encryption for secure communication. Authentication follows this process:

1. **Read Challenge**: Read the `Security.CHALLENGE` characteristic to get a random nonce
2. **Build Payload**: Create authentication payload:
   ```
   payload = nonce + key_id
   encrypted_payload = AES_ECB_Encrypt(payload, encryption_key)
   ```
3. **Write Authentication**: Write `encrypted_payload` to `Security.KEY_INDEX` characteristic
4. **Authenticated**: After successful authentication, you can read/write encrypted characteristics

## Encryption Details

- **Algorithm**: AES-128-ECB
- **Key Size**: 128 bits (16 bytes)
- **Block Size**: 16 bytes
- **Padding**: PKCS#7 padding for data not aligned to 16 bytes

### Encryption Key Sources

- **User Key (UKEY)**: User-specific encryption key obtained from VanMoof servers
- **Master Key (MKEY)**: Workshop/service key for advanced access
- **Key ID**: Identifies which key slot to use (typically 1 for user keys)

## Data Formats

### Battery Level
- **Type**: uint8
- **Range**: 0-100
- **Unit**: Percentage

### Distance
- **Type**: uint32 (little-endian)
- **Unit**: Meters
- **Example**: `[0x10, 0x27, 0x00, 0x00]` = 10000 meters = 10 km

### Speed
- **Type**: uint16 (little-endian)
- **Unit**: cm/s
- **Example**: `[0x88, 0x13]` = 5000 cm/s = 50 km/h

### One-byte values

Most encrypted characteristics hold one byte at offset 0 of the decrypted 16-byte block; the remaining bytes are zero (*confirmed* for 5523, 5524, 5542, 5543, 5544, 5563, 5564). Module state (5562) also sets byte 1.

### Lock State
- **Type**: uint8
- **Values**:
  - `0x00`: Unlocked
  - `0x01`: Locked

### Frame Number
- **Type**: ASCII string
- **Format**: Typically 8-10 characters
- **Example**: `"BK123456"`

## Implementation Notes

1. **Frame Number** is the only characteristic that can be read without authentication
2. All other read/write operations require prior authentication
3. Encrypted data must be decrypted after reading and encrypted before writing
4. The nonce (challenge) changes on each connection
5. Some characteristics support notifications for real-time updates

## References

- [pymoof Library](https://github.com/quantsini/pymoof) - Python implementation
- [VanMoofKit](https://github.com/SvenTiigi/VanMoofKit) - Swift implementation
- [vanmoof-tools](https://github.com/chwdt/vanmoof-tools) - Reverse engineering documentation

## Disclaimer

This documentation is based on reverse engineering efforts by the community. It is not official VanMoof documentation and may be incomplete or inaccurate. Use at your own risk.
