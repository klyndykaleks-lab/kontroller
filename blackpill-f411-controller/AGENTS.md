# Black Pill F411 Agent Guide

## Роль

STM32F411CEU6 работает как MDB Cashless Device #1 и центральный контроллер. FreeRTOS
задачи общаются очередями; `managerTask` содержит основную FSM. Здесь находятся самые
чувствительные к timing участки проекта.

## Канонический baseline

Импортирован из `/Users/maksim/Desktop/pr_VMC/7 7/Controller` и совпадает с
`VMC without buttons 0.0.1/Controller`. Исходный SHA-256 `Core/Src/ftasks.c`:
`fca545edffc0807fbe72391855ce28062f19da047972996aac80385a972469c2`.

## Интерфейсы

- MDB software 9-bit UART: `PA3` RX, `PA2` TX; полярность учитывает транзисторный каскад платы.
- ESP UART USART1: F411 `PA15` TX -> ESP GPIO18 RX; ESP GPIO17 TX -> F411 `PA10` RX; 115200 8N1.
- Motor Board: I2C v3, `0x0F`, keyed FINISH после события F411 PB15; Button Board: I2C v1, `0x10`.
- Замок: PA0, обратная связь PA1, ручной запрос PA4. PA5 — резерв.
- Схема, уровни и совместимость: `docs/PINOUT_V2.md`.
- LCD 1602 и SD обслуживаются отдельными задачами; вывод не должен задерживать MDB.

## Критические инварианты

1. MDB отвечает только на адрес Cashless #1.
2. На POLL во время ожидания ESP не посылать преждевременный `VEND_DENIED`.
3. `selection_id` запускает локальный мотор только после `PAYMENT_OK`.
4. Если ID привязан к мотору: после оплаты выдать снек и корректно завершить MDB ветку отказом кофе.
5. Если ID не привязан: после оплаты подтвердить выдачу кофемашине, мотор не запускать.
6. `SESSION COMPLETE`, отмена кнопкой Back, timeout и поздний ответ оплаты должны закрывать контекст ровно один раз.
7. Direct delivery работает без подключённого MDB, если F411 и Motor Board исправны.

## Известные особенности

- Payment timeout: 30 s.
- MDB max response time: 30 s в текущем профиле.
- `DELIVERY_LINE_CHECK` и `ORDER_CANCELLED` входят в текущий UART API.
- Direct delivery подтверждается по I2C v3 full key, BUSY/GPIO или быстрому terminal; переполнение manager
  queue проходит через неблокирующий fallback и публикует `CONTROLLER_DIAGNOSTIC`.
- Неудачные UART TX строки удерживаются в bounded RAM-outbox; финальный
  `DELIVERY_RESULT` имеет отдельный резервный слот.
- `osDelay(3000)` при старте исторически необходим для правильной инициализации.
- `.ioc` не содержит актуальную ручную USART1-конфигурацию. Cube regeneration может удалить PA10/PA15 setup.
- Неразобранный симптом: редкий `un_fineshed` на кофемашине после ряда операций.

## Работа с MDB

Перед изменением прочитать `protocol/mdb-profile.md`. Не добавлять `snprintf`, LCD update,
SD access или длительные critical sections в RX/TX timing path. Диагностику передавать
асинхронно и ограничивать частоту.

## Сборка

```bash
./scripts/build-controller.sh
```

Ожидаются `Debug/Controller.elf`, `.bin`, `.hex`. После MDB/FSM изменения обязательны
сценарии C01-C10 из `docs/TEST_PLAN.md`.

## Кандидат стабильности

USART1 RX использует DMA2 stream 2/channel 4, 4096-byte ring. Private UART payment/final
содержит boot/tx; direct содержит op и read-only DELIVERY_QUERY. Терминальный JSON/API
менять запрещено. Boot generation резервируется до RTOS в flash sectors 6/7; code limit
256 KiB. Flash programming в runtime F411/MDB не добавлять. I2C v3 форматы START/FINISH 20 bytes, snapshot 28 bytes,
CRC и сохраняемый ключ операции. Новые MDB правила проверяются синтетическим 9-bit replay,
который не заменяет физический MDB trace и измерение timing.
