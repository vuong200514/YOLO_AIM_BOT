# CS2 Memory & Offset Tool

Một công cụ C++ hỗ trợ tự động đọc và cập nhật các offset từ tệp cấu hình (`client_dll.hpp` và `offsets.hpp`) đặt cùng thư mục với tệp thực thi (`.exe`).

## 📌 Tính năng chính

- **Đọc offset động:** Tự động tìm và nạp giá trị offset từ các tệp header C++ (`.hpp`) cạnh tệp thực thi, không cần biên dịch lại khi thay đổi offset trong tệp cấu hình.
- **Tự động xác định thư mục thực thi:** Tìm đường dẫn tệp `.exe` để truy cập các tệp cấu hình đi kèm.
- **Tương thích x64:** Kiểm tra kiến trúc build bằng `static_assert`.
- **Thao tác bộ nhớ:** Hỗ trợ đọc/ghi bộ nhớ của tiến trình thông qua Windows API, tùy thuộc quyền truy cập.
- **Tối ưu YOLO trên GPU cấu hình hạn chế:** Sử dụng NVIDIA TensorRT, chuyển mô hình sang ONNX rồi xây dựng TensorRT Engine với độ chính xác FP16 để tăng tốc suy luận.

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

## ⚡ Tối ưu YOLO với NVIDIA TensorRT (GPU GTX 1650 Ti)

Ngoài module C++ xử lý offset, dự án có phần thử nghiệm tối ưu pipeline **YOLO Object Detection** nhằm duy trì hiệu năng thời gian thực trên GPU laptop cấu hình hạn chế.

### Quy trình tối ưu

1. **Export YOLO sang ONNX:** Chuyển checkpoint YOLO đã huấn luyện sang định dạng ONNX để làm đầu vào cho hệ thống tối ưu suy luận.
2. **Tạo TensorRT Engine FP16:** Sử dụng **NVIDIA TensorRT** biên dịch và tối ưu đồ thị ONNX thành engine chạy trên GPU, bật **FP16 (half precision)** để giảm chi phí tính toán và bộ nhớ.
3. **Đánh giá sau chuyển đổi:** So sánh kết quả detection và độ trễ suy luận với phiên bản gốc, kiểm tra chất lượng dự đoán vẫn được duy trì tốt trong các tình huống thử nghiệm.
4. **Tích hợp vào xử lý thời gian thực:** Sử dụng engine TensorRT trong pipeline nhận diện hình ảnh khi ứng dụng game đang hoạt động.

### Kết quả thử nghiệm

- **GPU:** NVIDIA GeForce GTX 1650 Ti.
- **Hiệu năng quan sát được:** Khoảng **200 FPS** trong quá trình chạy pipeline detection, **kể cả khi đang chơi game**.
- **Độ chính xác:** Theo đánh giá thực nghiệm, chất lượng nhận diện vẫn được duy trì tốt sau khi chuyển sang FP16.
- **Ý nghĩa:** Cho thấy khả năng tối ưu tốc độ suy luận YOLO trên phần cứng có tài nguyên GPU hạn chế.

> **Lưu ý benchmark:** 200 FPS là kết quả thử nghiệm được ghi nhận cho pipeline nhận diện; không đồng nghĩa game chạy 200 FPS. Giá trị có thể thay đổi theo độ phân giải đầu vào, phiên bản model, mức tải GPU và phương pháp đo. Chưa có bảng benchmark định lượng mAP/precision/recall trước và sau chuyển đổi.

## 📄 Giấy phép và mục đích sử dụng

Dự án phục vụ **học tập và nghiên cứu lập trình C++ trên Windows**, bao gồm cấu trúc bộ nhớ, Windows API và cơ chế nạp cấu hình. Chỉ thao tác với các tiến trình, dữ liệu và môi trường mà bạn có quyền truy cập. Chưa có tệp giấy phép phần mềm riêng được chỉ định.
