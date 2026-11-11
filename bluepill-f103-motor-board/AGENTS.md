# Blue Pill F103 Motor Board Agent Guide

## Роль

STM32F103C8T6 является I2C slave и единственной прошивкой, которая непосредственно
управляет только моторами и сохраняет приёмку/итог операции. F411 владеет датчиком и замком.

## Канонический baseline

Импортирован из `/Users/maksim/Desktop/pr_VMC/7 7/Motor_board new `.
Исходный SHA-256 `Core/Src/main.c`:
`85d52492553d549f7f45fac4a2cbecf987bfe623fce0f286cdd67a0e7b3a82c`.

## Линии моторов

В pinout v2 `deliveryLine=N` включает только M`N`. Карта PA5, PA6, PA3,
PA4, PA1, PA2, PC15, PA0, PC13, PC14 по порядку M1…M10 находится в
`docs/PINOUT_V2.md`. Проверять все десять выходов до подключения моторов.

## Инварианты

- Никогда не держать мотор включённым без конечного timeout.
- I2C callback должен оставаться коротким; физическую операцию выполнять в main loop/state machine.
- Результат `success` возможен только после keyed FINISH от F411 для текущей BUSY операции.
- Датчик PB15 и замок PA0 принадлежат F411; плата моторов к ним не подключается.
- I2C v3 snapshot 28 bytes возвращает сохранённый статус операции и ключ.

## Сборка и тест

```bash
./scripts/build-motor-board.sh
```

После изменения выполнить M01-M06 из `docs/TEST_PLAN.md`, включая проверку GPIO без
подключённых моторов и проверку просадки питания под нагрузкой.

## Кандидат стабильности

- START v3: 20 bytes с boot/op/direct ID и CRC; старые двухбайтные команды несовместимы.
- Последние две 1-KiB flash pages зарезервированы journal, linker code limit 62 KiB.
- Приёмка записывается до GPIO; terminal публикуется после commit. I2C ISR читает
  атомарную копию записи; программирование flash выполняется только в main.
- После reset BUSY становится FAILED без автозапуска. Duplicate/old key не меняет GPIO.
- Не стирать journal одной платы независимо от paired F411/ESP; см. protocol/i2c-motor-board.md.
