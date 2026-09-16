# Stuttering Watch

A wearable for kids who stutter. The watch listens while the child speaks, sends audio to this backend, and the companion app tracks practice, progress, and fluency over time.

This repository currently contains:

- **Backend** — Django REST API for accounts, therapy game levels, and watch audio uploads
- **Watch firmware** — ESP32 sketch that records speech over I2S and uploads WAV chunks to the API

Hardware, schematics, and enclosure files will be added later.

## How it fits together

```
Kid's watch (ESP32 + mic)
        │  login + audio chunks
        ▼
Django API  ──────────────►  Companion app (accounts, games, stats)
        │
        ▼
Stored speaking sessions (fluency metrics: no stutter, prolongation, repetition, block)
```

1. The watch connects to Wi-Fi, authenticates with the API, and uses a simple voice-activity check.
2. While speech is detected, it records ~3 second WAV chunks and uploads them.
3. Parents or therapists use the app APIs for signup, settings, and pronunciation practice levels.

## Project layout

```
backend/                 Django project settings and WSGI
core/                    API models, views, auth, migrations
StutternigWatchCode/     ESP32 Arduino firmware
media/                   Uploaded audio and images (local)
Dockerfile               Production image (Gunicorn)
requirements.txt
```

## Backend setup

Requires Python 3.10+.

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt

cp .env.example .env
# Edit .env: set DJANGO_SECRET_KEY and optional Gmail SMTP credentials

export DJANGO_SECRET_KEY="$(grep DJANGO_SECRET_KEY .env | cut -d= -f2-)"
export EMAIL_HOST_USER="$(grep EMAIL_HOST_USER .env | cut -d= -f2-)"
export EMAIL_HOST_PASSWORD="$(grep EMAIL_HOST_PASSWORD .env | cut -d= -f2-)"

python manage.py migrate
python manage.py createsuperuser
python manage.py runserver
```

Admin: [http://127.0.0.1:8000/admin/](http://127.0.0.1:8000/admin/)  
API root: `http://127.0.0.1:8000/api/`

Email verification and password reset need `EMAIL_HOST_USER` and `EMAIL_HOST_PASSWORD` (Gmail app password). Without them, those endpoints will fail when sending mail; the rest of the API still runs.

### Docker

```bash
docker build -t stuttering-backend .
docker run -p 8000:8000 \
  -e DJANGO_SECRET_KEY=change-me \
  stuttering-backend
```

## API overview

Auth uses a JWT in the `Authorization` header (`Token <jwt>`). Signup is email-based (`User.USERNAME_FIELD` is email).

| Method | Path | Purpose |
|--------|------|---------|
| POST | `/api/signup/` | Create account and send verification code |
| POST | `/api/verify_signup/` | Confirm email |
| POST | `/api/resend_code/` | Resend verification code |
| POST | `/api/login/` | Get JWT |
| GET | `/api/my_profile/` | Current user |
| POST | `/api/change_password/` | Change password |
| GET/PUT | `/api/settings/` | Language, dark mode, notifications |
| GET | `/api/current_state/` | Therapy game progress |
| POST | `/api/submit_audio/` | Submit practice recording for a level record |
| POST | `/api/audio_chunk_upload/` | Watch: upload a WAV chunk (`audio_chunk`, `created_at`) |
| POST | `/api/reset_password/` | Request reset code |
| POST | `/api/verify_reset_password/` | Set a new password |

Watch login body:

```json
{ "email": "child-or-device@example.com", "password": "..." }
```

Watch upload is `multipart/form-data` with `audio_chunk` (WAV) and `created_at` (ISO-8601). Chunks close in time are appended onto the same `SpeakingAudio` session.

## Watch firmware

Sketch: `StutternigWatchCode/StutternigWatchCode.ino`

- Board: ESP32
- Mic: I2S (default pins SCK 32, WS 25, SD 33)
- Sample rate: 8 kHz, 16-bit mono WAV
- Voice activity threshold, then 3-second recordings

Before flashing, set Wi-Fi SSID/password, API base URL, and device account email/password at the top of the sketch. Do not commit real credentials.

Arduino IDE: install ESP32 board support, open the sketch, select your board, flash.

## Coming later

- Circuit diagrams, BOM, and PCB / wiring for the watch
- Enclosure / 3D models
- Hardware notes for battery, charging, and the microphone
- Any companion mobile app source

## License

Not specified yet. Ask before reusing this for a commercial product.
