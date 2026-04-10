# PC Client for ESP32 Intercom

Python-клиент для общения с ESP32 интеркомами с ноутбука/ПК (Windows, Linux, macOS).

## Установка

```bash
cd pc_client
pip install -r requirements.txt
```

### Windows — дополнительно

Библиотеке `keyboard` нужны права администратора. Запускайте от администратора:
```cmd
python intercom_client.py --ip 192.168.1.100
```

## Быстрый старт

### Один интерком

```bash
python intercom_client.py --ip 192.168.1.100
```

Пробел = PTT (передача), Q или Esc = выход.

### Своя клавиша PTT

```bash
python intercom_client.py --ip 192.168.1.100 --key f1
```

### Несколько интеркомов

Редактируешь `intercoms.json` — прописываешь IP и клавиши:

```json
{
    "pc_name": "PC",
    "intercoms": [
        { "name": "Garage", "ip": "192.168.1.100", "key": "space" },
        { "name": "Home",   "ip": "192.168.1.101", "key": "enter" }
    ]
}
```

Запуск:

```bash
python intercom_client.py --config intercoms.json
```

Каждый интерком на своей клавише — не broadcast!

## Управление

| Клавиша | Действие |
|---------|----------|
| Space / F1 / ... (из конфига) | PTT — говорить |
| Q / Esc | Выход |

## Протокол

- Управление: JSON по UDP порт 8080
- Аудио: raw PCM по UDP порт 8081
- Формат: 16 kHz, 16-bit signed, mono
- Заголовок аудио: 6 байт `[seq:u16 LE][timestamp:u32 LE]`
