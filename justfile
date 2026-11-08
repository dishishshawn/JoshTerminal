default: build

build:
    pio run

flash:
    pio run -t upload

monitor:
    pio device monitor

# flash then open the serial monitor
run: flash monitor

# regenerate fonts and images in src/fonts and src/img (needs the URW Nimbus fonts: fonts-urw-base35)
assets:
    #!/usr/bin/env bash
    set -euo pipefail
    py="uv run -q --with pillow python"
    N=/usr/share/fonts/opentype/urw-base35/NimbusSansNarrow
    mkdir -p src/fonts src/img
    $py scripts/ttf2lvgl.py $N-Bold.otf font_price_84 84 '0123456789.-' > src/fonts/font_price_84.c
    $py scripts/ttf2lvgl.py $N-Bold.otf font_bold_26 26 'ascii−' > src/fonts/font_bold_26.c
    $py scripts/ttf2lvgl.py $N-Regular.otf font_text_17 17 'ascii−' > src/fonts/font_text_17.c
    $py scripts/ttf2lvgl.py $N-Bold.otf font_bold_17 17 '135ADMYl' > src/fonts/font_bold_17.c
    $py scripts/ttf2lvgl.py $N-Bold.otf font_bold_36 36 '0123456789.+-−%$BTM' > src/fonts/font_bold_36.c
    $py scripts/img2lvgl.py "reddit logo.webp" badge_30 30 knockout > src/img/badge_30.c
    $py scripts/img2lvgl.py "reddit logo.webp" badge_64 64 knockout > src/img/badge_64.c
    $py scripts/make_arrow.py
    $py scripts/img2lvgl.py assets/arrow_up.png arrow_up 40x46 > src/img/arrow_up.c

clean:
    pio run -t clean
