# Hướng dẫn cài và nạp firmware ESP32-S3

Tài liệu này áp dụng cho firmware trong `src/` và cấu hình PlatformIO trong `platformio.ini` của dự án Robot UAV Xpert.

## Phần cứng và cấu hình hiện tại

- Board khai báo: ESP32-S3 DevKitC-1, flash 4 MB; profile dùng để biên dịch là `esp32-s3-super-mini`.
- Framework: Arduino qua PlatformIO.
- Driver động cơ: DRV8833; firmware dùng GPIO 3, 4, 5, 6.
- Servo: GPIO 7. NeoPixel/debug LED: GPIO 48.
- BLE name đang chọn: `ESP32-MOTOR-XANH-02`, mã xe `CAR-02`.
- Mật khẩu BLE mặc định: `123456`.

Kiểm tra board thực tế và dây nối trước khi cấp nguồn. Nếu dùng profile xe khác hoặc chân khác, sửa `ACTIVE_DEVICE_INDEX` và cấu hình tương ứng trong `src/device_config.h` và `src/device_config.cpp` trước khi build.

## Cài môi trường

1. Cài Visual Studio Code và extension **PlatformIO IDE**.
2. Mở thư mục gốc của repo trong VS Code (thư mục có `platformio.ini`). Chờ PlatformIO cài platform Espressif32 và thư viện NeoPixel.
3. Kết nối ESP32-S3 bằng cáp USB có truyền dữ liệu. Cài driver USB-UART nếu board của bạn dùng chip chuyển đổi USB-UART; cổng USB native của S3 thường không cần driver riêng.
4. Đóng Serial Monitor hoặc ứng dụng khác đang chiếm cổng COM.

## Nạp firmware từ mã nguồn

Trong terminal tại thư mục gốc dự án, chạy:

```powershell
pio run -e esp32-s3-super-mini
pio device list
pio run -e esp32-s3-super-mini -t upload --upload-port COM5
```

Thay `COM5` bằng cổng thiết bị thực tế. Có thể bỏ `--upload-port COM5` nếu PlatformIO tự nhận đúng cổng. Trong VS Code, thao tác tương đương là **PlatformIO: Build** rồi **PlatformIO: Upload**.

Nếu không vào chế độ nạp tự động, giữ nút **BOOT**, nhấn rồi thả **RESET/EN**, sau đó thả **BOOT** và chạy lệnh upload lại. Sau khi nạp xong nhấn RESET/EN một lần.

## Kiểm tra khởi động

Mở Serial Monitor ở **115200 baud**. Khi khởi động bình thường, log sẽ có thông tin firmware, trạng thái khởi tạo PWM và dòng `BLE san sang` cùng tên/mã thiết bị. Các ngõ điều khiển động cơ được kéo LOW và tốc độ khởi đầu bằng 0.

Trên Chrome hoặc Edge, mở giao diện web qua HTTPS hoặc `localhost`, chọn **Kết nối thiết bị**, chọn thiết bị có tên tương ứng, nhập mật khẩu `123456` nếu giao diện yêu cầu, rồi thử servo và động cơ ở tốc độ thấp trước. Web Bluetooth không hoạt động ổn định khi mở trang trực tiếp bằng `file://`; dùng bản Vercel theo `HUONG_DAN_DEPLOY_VERCEL.md` hoặc máy chủ localhost.

## BLE của firmware

- Service UUID: `6E400001-B5A3-F393-E0A9-E50E24DCCA9E`
- RX/write UUID: `6E400002-B5A3-F393-E0A9-E50E24DCCA9E`
- TX/notify UUID: `6E400003-B5A3-F393-E0A9-E50E24DCCA9E`
- Xác thực: ghi `AUTH:<mật khẩu>` vào RX.
- Lệnh điều khiển: `f<PWM>`, `b<PWM>`, `l<PWM>`, `r<PWM>`, `s`; PWM từ 0 đến 255.
- Servo: `v<góc>`; góc từ 0 đến 180.
- Khi BLE ngắt, firmware dừng động cơ.

## Firmware nhị phân có sẵn

Repo có `release/v1.0.0/firmware.bin` và `firmware.factory.bin`. Đây là các bản build đã đóng gói từ trước; nếu cần chắc chắn khớp mã nguồn hiện tại, hãy build và nạp từ mã nguồn bằng PlatformIO như các bước trên. `firmware.bin` là app image; `firmware.factory.bin` thường là image ghép để nạp từ offset 0. Không nạp file app image tại offset 0. Với board 4 MB và partition `default.csv` hiện tại, cách ít nhầm nhất là nạp qua PlatformIO; chỉ nạp file phát hành theo offset/hướng dẫn đi kèm chính release đó.

## Lỗi thường gặp

- **Không thấy cổng COM:** thử cáp USB dữ liệu khác, cổng USB khác, kiểm tra Device Manager và driver phù hợp với board.
- **Upload timeout / Failed to connect:** đóng ứng dụng chiếm cổng; thử quy trình BOOT/RESET thủ công; kiểm tra lại cổng COM.
- **Build lỗi do board/profile:** mở đúng thư mục gốc dự án để PlatformIO đọc `platformio.ini`; không chọn profile board khác tùy ý.
- **Web không tìm thấy thiết bị:** bật Bluetooth, cấp quyền Bluetooth cho trình duyệt, dùng Chrome/Edge hỗ trợ Web Bluetooth và truy cập bằng HTTPS/localhost. Kiểm tra tên BLE trong `device_config.cpp`.
- **Xác thực thất bại:** mật khẩu cấu hình trong `device_config.cpp` phải trùng với mật khẩu nhập trên web.
- **Động cơ/servo không chạy đúng:** kiểm tra nguồn riêng phù hợp cho driver/servo, nối chung GND với ESP32, dây GPIO và loại board. Không cấp dòng motor từ chân 3V3 của ESP32.

## Ghi chú rà soát cấu hình

Tất cả sáu profile trong `src/device_config.cpp` đang đặt cùng mật khẩu `123456`; hãy đổi trước khi triển khai thực tế nếu cần giới hạn truy cập. Firmware dùng mật khẩu BLE đơn giản ở tầng ứng dụng, không nên xem đây là cơ chế bảo mật mạnh. Ngoài ra, đổi `ACTIVE_DEVICE_INDEX` không tự tạo khác biệt chân/mật khẩu nếu profile được chọn vẫn giữ cùng giá trị.
