%ifndef ESP_START_LBA
%define ESP_START_LBA 69632
%endif
%ifndef ESP_SECTOR_COUNT
%define ESP_SECTOR_COUNT 1024
%endif

times 0x1A0 db 0

db 'LEAN_OS1'

times 0x1BE - ($ - $$) db 0

db 0x00
db 0xFE, 0xFF, 0xFF
db 0xEF
db 0xFE, 0xFF, 0xFF
dd ESP_START_LBA
dd ESP_SECTOR_COUNT

times 510 - ($ - $$) db 0
dw 0xAA55
