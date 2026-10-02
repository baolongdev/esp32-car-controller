# Hướng dẫn từ đầu: cài Node.js, đẩy code lên GitHub và deploy Vercel bằng CLI

Hướng dẫn này dành cho Windows và PowerShell. Repo là web tĩnh, trang chính là `ble_motor_control.html`; file `vercel.json` đã cấu hình `/` trỏ đến trang đó. Không cần framework hay lệnh build.

## 1. Cài Node.js

1. Mở trang tải chính thức: <https://nodejs.org/>.
2. Tải bản **LTS** cho Windows, chạy file `.msi` vừa tải.
3. Trong bộ cài, giữ các lựa chọn mặc định và đảm bảo mục cài **npm package manager** được bật. Hoàn tất cài đặt.
4. Đóng rồi mở lại PowerShell để cập nhật PATH.
5. Kiểm tra:

```powershell
node --version
npm --version
```

## 2. Cài Git và cấu hình tên

1. Tải Git for Windows tại <https://git-scm.com/download/win> và cài với lựa chọn mặc định.
2. Mở PowerShell mới, kiểm tra bằng `git --version`.
3. Đặt tên và email commit của bạn:

```powershell
git config --global user.name "Ten cua ban"
git config --global user.email "email-cua-ban@example.com"
```

Email nên là email đã đăng ký GitHub hoặc địa chỉ noreply GitHub của bạn.

## 3. Tạo repository GitHub

1. Đăng nhập tại <https://github.com/> (tạo tài khoản nếu chưa có).
2. Mở <https://github.com/new>, đặt tên repo, ví dụ `robot-uav-xpert`, rồi chọn Public hoặc Private.
3. Không chọn **Add a README**, `.gitignore` hoặc license. Repo trống giúp push lần đầu không xung đột.
4. Nhấn **Create repository** và lấy URL HTTPS của repo, dạng `https://github.com/USERNAME/REPOSITORY.git`.

## 4. Push thư mục dự án lên GitHub

Mở PowerShell tại thư mục `E:\Robot - UAV Xpert`. Có thể mở thư mục đó trong File Explorer, bấm thanh địa chỉ, gõ `powershell` rồi Enter. Xác nhận đúng thư mục (cần thấy `ble_motor_control.html`, `vercel.json` và `platformio.ini`):

```powershell
Get-Location
Get-ChildItem
git status
```

Nếu `git status` báo không phải repository Git, khởi tạo repo:

```powershell
git init
git branch -M main
```

Sau đó thêm file, commit và push. Thay URL mẫu bằng URL repo của bạn:

```powershell
git add .
git commit -m "Initial project upload"
git remote add origin https://github.com/USERNAME/REPOSITORY.git
git push -u origin main
```

Nếu Git báo `remote origin already exists`, xem URL hiện tại bằng `git remote -v`. Để sửa URL rồi push:

```powershell
git remote set-url origin https://github.com/USERNAME/REPOSITORY.git
git push -u origin main
```

Khi GitHub yêu cầu đăng nhập, làm theo cửa sổ trình duyệt hoặc Git Credential Manager; thường không dùng mật khẩu tài khoản GitHub trực tiếp trong prompt Git. Kiểm tra trang repo GitHub có file HTML và `vercel.json` ở thư mục gốc.

## 5. Cài Vercel CLI và đăng nhập

Mở PowerShell tại thư mục dự án và chạy:

```powershell
npm install --global vercel
vercel --version
vercel login
```

Hoàn thành xác thực theo hướng dẫn CLI rồi quay lại PowerShell.

## 6. Deploy lần đầu bằng CLI

Đảm bảo đang ở thư mục có `vercel.json`, sau đó chạy:

```powershell
vercel
```

Trả lời các câu hỏi CLI:

- **Set up and deploy?** → `Y`
- **Which scope?** → chọn tài khoản hoặc team sở hữu project
- **Link to existing project?** → `N` để tạo project mới; chọn `Y` nếu project Vercel đã tồn tại
- **Project name** → ví dụ `robot-uav-xpert`
- **In which directory is your code located?** → `./`
- **Framework preset** → `Other` hoặc `No framework`
- **Build command** → để trống
- **Output directory** → để trống hoặc chấp nhận giá trị mặc định
- **Development command** → để trống

CLI sẽ deploy preview và in URL. Mở URL để kiểm tra. Liên kết project được lưu cục bộ trong `.vercel/`; thư mục này đã bị Git ignore.

## 7. Deploy production và cập nhật

Xuất bản production bằng:

```powershell
vercel --prod
```

CLI sẽ in URL production. Sau khi sửa code, lưu thay đổi lên GitHub rồi deploy bản mới:

```powershell
git add .
git commit -m "Update web interface"
git push
vercel --prod
```

## 8. Kiểm tra web và BLE

Mở URL HTTPS do Vercel cấp. Đường dẫn `/` hiển thị giao diện theo rewrite trong `vercel.json`. Dùng Chrome hoặc Edge có hỗ trợ Web Bluetooth, bật Bluetooth, nhấn **Kết nối thiết bị**, chọn ESP32 và nhập mật khẩu firmware (hiện là `123456` trong `src/device_config.cpp`).

Web Bluetooth không hỗ trợ trên mọi trình duyệt, gồm Safari/iOS, và không hoạt động khi mở trang bằng `file://`. ESP32 cần bật và ở gần máy tính/điện thoại.

## Lỗi thường gặp

- **`node` hoặc `npm` không nhận diện:** mở PowerShell mới sau khi cài Node.js; nếu cần khởi động lại Windows.
- **`git` không nhận diện:** cài Git for Windows rồi mở PowerShell mới.
- **GitHub báo `rejected`:** repo có thể không trống. Hướng dẫn này giả định repo mới tạo không có README/commit khởi tạo.
- **`src refspec main does not match any`:** chưa tạo commit. Chạy `git add .`, `git commit -m "Initial project upload"`, rồi push lại.
- **Deploy nhầm thư mục:** kiểm tra `Get-Location`; cần đứng tại thư mục chứa `vercel.json`.
- **Không thấy ESP32:** thử Chrome/Edge, bật Bluetooth, cấp quyền chọn thiết bị và kiểm tra tên/mật khẩu BLE trong firmware.
