# CS2 Memory & Offset Tool

Một công cụ C++ hỗ trợ tự động đọc và cập nhật các offset từ tệp cấu hình (`client_dll.hpp` và `offsets.hpp`) đặt cùng thư mục với tệp thực thi (`.exe`).

## 📌 Tính năng chính

- **Đọc offset động:** Tự động tìm và nạp giá trị offset từ các tệp header C++ (`.hpp`) cạnh tệp thực thi, không cần biên dịch lại khi thay đổi offset trong tệp cấu hình.
- **Tự động xác định thư mục thực thi:** Tìm đường dẫn tệp `.exe` để truy cập các tệp cấu hình đi kèm.
- **Tương thích x64:** Kiểm tra kiến trúc build bằng `static_assert`.
- **Thao tác bộ nhớ:** Hỗ trợ đọc/ghi bộ nhớ của tiến trình thông qua Windows API, tùy thuộc quyền truy cập.

## 📁 Cấu trúc thư mục

Đặt các tệp cấu hình cùng thư mục với chương trình sau khi biên dịch:

```text
Build/
├── YourProgram.exe      # Tệp thực thi chính
├── client_dll.hpp       # Offset client (hoặc client.dll.hpp)
└── offsets.hpp          # Offset chính (hoặc offset.hpp)
```

## 🛠️ Yêu cầu hệ thống và môi trường

- **Hệ điều hành:** Windows 10/11 (64-bit).
- **Trình biên dịch:** Visual Studio 2019/2022 hoặc mới hơn.
- **Cấu hình build:** `x64` (`Release` hoặc `Debug`).

> ⚠️ **Lưu ý:** Dự án yêu cầu build ở chế độ **x64**. Nếu build với **x86**, kiểm tra `static_assert` sẽ khiến quá trình biên dịch thất bại.

## 🚀 Hướng dẫn biên dịch và sử dụng

### 1. Biên dịch dự án

1. Mở dự án bằng **Visual Studio**.
2. Chuyển nền tảng từ `x86` sang **`x64`**.
3. Chọn cấu hình `Release` hoặc `Debug`.
4. Nhấn `Ctrl + Shift + B` để build.

### 2. Chuẩn bị trước khi chạy

1. Mở thư mục chứa tệp `.exe` đã build (ví dụ: `x64/Release/`).
2. Sao chép `client_dll.hpp` và `offsets.hpp` vào **cùng thư mục** với `.exe`.

### 3. Khởi chạy

1. Mở ứng dụng/tiến trình mục tiêu mà bạn có quyền kiểm tra.
2. Chạy chương trình. Nếu thao tác với tiến trình cần quyền nâng cao, hãy dùng **Run as administrator** theo yêu cầu quyền truy cập của Windows.

## ⚙️ Cách thức hoạt động

Chương trình sử dụng `Offsets::ReadFileContent` và Windows API `GetModuleFileNameW` để xác định nơi chứa tệp thực thi và nạp cấu hình:

1. Xác định thư mục hiện tại của tệp `.exe`.
2. Tìm và mở `offsets.hpp` hoặc `offset.hpp`, cùng với `client_dll.hpp` hoặc `client.dll.hpp`.
3. Dùng **Regular Expressions (Regex)** để trích xuất giá trị hexadecimal (`0x...`) theo tên biến.
4. Nạp các offset vào cấu trúc dữ liệu sử dụng tại runtime.

> **Ghi chú:** Việc đọc tệp `.hpp` ở runtime là đọc nội dung văn bản cấu hình; chương trình không tự biên dịch lại các header.

## 📄 Giấy phép và mục đích sử dụng

Dự án phục vụ **học tập và nghiên cứu lập trình C++ trên Windows**, bao gồm cấu trúc bộ nhớ, Windows API và cơ chế nạp cấu hình. Chỉ thao tác với các tiến trình, dữ liệu và môi trường mà bạn có quyền truy cập. Chưa có tệp giấy phép phần mềm riêng được chỉ định.
