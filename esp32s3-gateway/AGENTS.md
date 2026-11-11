# ESP32-S3 Gateway Agent Guide

## Роль

ESP32-S3 поднимает локальный HTTP API `http://<ip>/{payment,delivery}`, принимает JSON от
терминала и преобразует его в строковый UART протокол F411. Исходящие business-сообщения
передаются HTTPS worker на per-board Kaspay endpoint. Исполнительные решения
принимает F411; ESP не управляет мотором напрямую.

## Канонический baseline

Импортирован из `pr_VMC/7 7/Controller/esp_192_168_0_test_package/...`.
Исходный SHA-256 старого sketch:
`dd9943c014e87636fcb057cf3d8c8fff7645d1499e29230db436ba1910ef6ac1`.
Переименование `.ino` выполнено только для корректной структуры Arduino sketch.

## Текущая конфигурация

- Wi-Fi SSID: `4G-CPE-360F` (password remains in ignored `wifi_secrets.h`).
- Static IP: `192.168.0.10`, gateway/DNS `192.168.0.1`, subnet `/24`.
- Local API: port 80, два POST route из `protocol/http-api.md`.
- HTTPS test default: `https://device-api-test.kas-pay.by/controllers/01m1f161e1zzxq8985yh9xbchy/commands`;
  production требует отдельно provisioned board ID `[a-z0-9]+`.
- ESP UART: RX GPIO18, TX GPIO17, 115200 8N1.
- Payment timeout: 30 s.
- Delivery result timeout: 60 s.
- Line check timeout: 1.5 s.
- HTTPS transport работает в отдельной FreeRTOS-задаче и не владеет FSM/UART/NVS.
- WebServer работает в отдельной low-priority задаче core 0; только main loop вызывает
  business admission и остаётся единственным владельцем UART/FSM/NVS.
- Inbound HTTP: body <=512 bytes, receive <=2 s, processing <=2.5 s,
  response write <=1 s; bridge capacity 1.
- Outbox identity — только внутренний `slot+generation`; не добавлять его в public JSON.
- Direct-delivery `ACTIVE/RESULT` хранится в компактном NVS-журнале; reboot не повторяет
  физическую команду и восстанавливает ожидание/отправку результата.
- Пока direct `RESULT` не получил exact backend ACK и owner-checked NVS clear, новая
  direct-операция на любой линии запрещена; Confirmed повторяет только local clear.

## Известные риски

- Новый WS client заменяет старый; событие замены должно быть видно в serial log.
- Для direct delivery `requestStatus=accepted` означает подтверждённую приёмку F103 (включая быстрый terminal), но не
  физическое вращение или падение товара.
- Падение WS одновременно с потерей ping и LCD указывает на общий reset/power, а не на WS библиотеку.
- В исходнике находятся Wi-Fi параметры; не публиковать репозиторий до их выноса.

## Правила изменения

- Не выполнять TLS/HTTP POST в loop или HTTP callback; worker возвращает только transport state.
- Не вызывать `admitTerminalJson` из HTTP-задачи и не обращаться из неё к UART/NVS/FSM.
- Не заменять project-local Network/NetworkClientSecure/WebServer/HTTPClient системными
  копиями без переноса bounded parsing/I/O patches и host regression tests.
- На reconnect не терять уже полученный UART result, пока не истёк явный timeout.
- Дубликат запроса обрабатывать идемпотентно; не запускать вторую физическую выдачу.
- Не менять JSON spelling без синхронной правки `protocol/http-api.md` и tester.
- controllerId 0 и 1 должны приниматься согласно текущему API.

## Сборка

```bash
./scripts/build-esp.sh
```

Используется ESP32 core 3.3.7, `ArduinoJson` 7.4.3 и project-local pinned forks
Network/NetworkClientSecure/WebServer/HTTPClient 3.3.7. Системный CA bundle и hostname
verification сохранены.
После изменения выполнить E01-E09 и интеграционные сценарии из `docs/TEST_PLAN.md`.

## Кандидат стабильности

Старый WebSocket API сохранён как архивный baseline, но не запускается runtime. Тест
по-прежнему фиксирует JSON builders и API documents. Private UART содержит boot/tx или direct op. NVS payment/vend/direct
не очищать для обхода busy: paid требует durable decision, final ACK требует durable result.
Direct operationId резервируется монотонно в NVS; после неоднозначного UART write/reboot
отправляется только DELIVERY_QUERY. Успешный enqueue не является HTTPS ACK; durable
result очищается только после точного `200 {"status":"received"}`.
