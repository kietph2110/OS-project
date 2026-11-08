# Design Proposal – Project 26107: Futexes, semaphores, condition variables

Course: Operating Systems – 20261 · Nhóm: Phạm Nguyễn Anh Kiệt (20233483), Đặng Trung Kiên (20233473), Trần Ngọc Lâm (20223714)

Bản Tuần 3: thiết kế dự kiến và kế hoạch kiểm thử; mục 5 phân biệt spinlock hiện có với các thành phần sẽ triển khai theo lộ trình.

| Mục | Nội dung | Người viết |
| --- | --- | --- |
| 1. Giới thiệu và mục tiêu | Bài toán, giải pháp futex, sản phẩm demo | Kiệt |
| 2. Giao diện system call | futex_wait, futex_wake, shmalloc | Kiệt (shmalloc: Kiên) |
| 3. Cấu trúc dữ liệu kernel | Hàng đợi futex, refcnt[], cờ PTE_SH | Kiệt + Kiên |
| 4. Bộ nhớ dùng chung | shmalloc, uvmcopy, kfree với refcount | Kiên |
| 5. Thư viện tầng user | mutex, semaphore, condition variable, barrier | Lâm |
| 6. Danh sách file bị sửa | File, thay đổi, người phụ trách | Cả nhóm |
| 7. Kế hoạch kiểm thử | projecttests, CPUS=1/4, rò rỉ trang, usertests | Lâm + Kiệt |
| 8. Kế hoạch theo tuần | Tuần 3 → Tuần 9 | Kiệt |

## 1. Giới thiệu và mục tiêu

Dự án bổ sung futex vào xv6-riscv để các tiến trình đồng bộ với nhau mà không tốn CPU khi chờ, rồi xây thư viện mutex, semaphore, condition variable và barrier ở tầng user trên nền đó.

**Bài toán.** Chương trình user trong xv6 muốn chờ một khóa chỉ có thể spin, tốn trọn một CPU, hoặc gọi `pause` rồi thử lại. Các tiến trình cũng chưa có vùng nhớ chung để đặt biến khóa.

**Giải pháp.** Khi không tranh chấp, khóa và mở khóa chỉ dùng lệnh nguyên tử RISC-V ở tầng user. Khi có tranh chấp, tiến trình gọi `futex_wait` để ngủ trong kernel và được đánh thức bằng `futex_wake`. Khóa định danh là địa chỉ vật lý của biến futex, nên hai tiến trình dùng chung trang nhớ sẽ chờ trên cùng một khóa.

**Sản phẩm cuối kỳ.** Ba system call `futex_wait`, `futex_wake`, `shmalloc`; thư viện mutex, semaphore, condition variable, barrier; bộ test `projecttests`; benchmark futex, spinlock và yield lock trên 1–4 CPU; demo dining philosophers.

## 2. Giao diện system call

Nhóm thêm ba system call; `futex_wait` và `futex_wake` (số 23, 24) đã có khung trên nhánh `main` từ Tuần 2, `shmalloc` sẽ cài ở Tuần 4.

| System call | Số | Ý nghĩa | Trả về |
| --- | --- | --- | --- |
| `int futex_wait(int *addr, int val)` | 23 | Nếu `*addr == val` thì ngủ cho tới khi có `futex_wake` trên cùng địa chỉ | 0 khi được đánh thức; -1 nếu `*addr != val` hoặc `addr` không hợp lệ |
| `int futex_wake(int *addr, int n)` | 24 | Đánh thức tối đa `n` tiến trình đang chờ trên `addr` | Số tiến trình đã đánh thức; -1 nếu `n <= 0` hoặc `addr` không hợp lệ |
| `void *shmalloc(int npages)` | 25 | Cấp `npages` trang nhớ dùng chung, tiến trình con sau `fork()` thấy cùng trang (Kiên ghi chi tiết ở mục 4) | Địa chỉ ảo của vùng nhớ; 0 nếu lỗi |

**Quy tắc với `addr`:**

- Phải căn lề 4 byte và nằm trong không gian user đã ánh xạ (`PTE_V` và `PTE_U`).
- Kernel kiểm tra bằng `futex_key()` (Lâm viết, `kernel/vm.c`): trả về địa chỉ vật lý làm khóa, hoặc 0 nếu địa chỉ sai.
- Hai tiến trình trỏ vào cùng một biến trong trang dùng chung luôn ra cùng một khóa, dù địa chỉ ảo khác nhau.

**Vì sao `futex_wait` cần tham số `val`.** Giữa lúc user thấy khóa bận và lúc vào kernel, tiến trình khác có thể đã nhả khóa và gọi `futex_wake`. Kernel đọc lại `*addr` trong lúc giữ khóa của hàng đợi; nếu giá trị đã khác `val` thì trả về ngay thay vì ngủ, tránh mất tín hiệu đánh thức (lost wake-up).

**Trạng thái hiện tại (Tuần 3).** `sys_futex_wait` đã so sánh `*addr` với `val` nhưng chưa ngủ (làm ở Tuần 4); `sys_futex_wake` đã gọi `wakeup_n`. Chương trình `futexstub` kiểm tra 7 trường hợp tham số đúng và sai.

**Câu hỏi mở:**

- [ ] Benchmark "yield lock" cần system call `yield()` mà xv6 chưa có; dự kiến Kiệt thêm cùng chương trình benchmark ở Tuần 6.

## 3. Cấu trúc dữ liệu kernel (Kiệt + Kiên, viết ở Tuần 3)

- **Hàng đợi futex (Kiệt).** Không thêm cấu trúc mới: tiến trình chờ được đánh dấu bằng `p->chan = key`, trong đó `key` là địa chỉ vật lý do `futex_key()` trả về. Một spinlock `futex_lock` trong `kernel/proc.c` bảo vệ bước kiểm tra giá trị. Trang user không bao giờ trùng với đối tượng kernel, nên `key` không đụng các kênh chờ khác.
  - `wakeup_n(chan, n)` duyệt bảng `proc`, đánh thức tối đa `n` tiến trình có `p->chan == chan` và trả về số đã đánh thức. Nhóm chọn cách này thay cho bảng băm vì `NPROC = 64`, quét toàn bảng đủ nhanh và dùng lại được `sleep_prepare()`/`sleep()`. Hạn chế: một lock chung và không đánh thức theo FIFO.
  - Chống mất tín hiệu: `futex_wait` giữ `futex_lock`, so sánh `*addr` với `val`, gọi `sleep_prepare(key)`, nhả lock rồi gọi `sleep()`. Nếu `futex_wake` chen vào giữa, `p->chan` đã bị xóa nên `sleep()` trả về ngay.
- **Bộ đếm tham chiếu `refcnt[]` (Kiên).** Trong `kernel/kalloc.c`:

  ```c
  #define PA2IDX(pa) (((uint64)(pa) - KERNBASE) / PGSIZE)
  #define NPHYSPAGES ((PHYSTOP - KERNBASE) / PGSIZE) // 32 768 trang

  struct {
    struct spinlock lock;
    int refcnt[NPHYSPAGES];
  } kref;
  ```

  - Mỗi trang vật lý 4 KB có một ô `int`, tổng cộng 128 KB. Mảng nằm trong BSS nên được xóa về 0 khi khởi động.
  - Bất biến: trang trong freelist có `refcnt = 0`; trang đang dùng có `refcnt` bằng số bảng trang đang ánh xạ tới nó.
  - Khóa riêng `kref.lock`, tách khỏi `kmem.lock`; `kalloc()` và `kfree()` không bao giờ giữ cả hai khóa cùng lúc.
  - `kalloc()` đặt 0 → 1; `kfree()` giảm 1, chỉ trả trang về freelist khi về 0 và `panic` nếu bộ đếm đã ≤ 0 (double-free); `kref_inc()` tăng 1 cho trang đang dùng; `kref_get()` đọc bộ đếm để kiểm thử.
  - `freerange()` đặt `refcnt = 1` trước khi gọi `kfree()` cho từng trang lúc khởi động.
- **Cờ `PTE_SH` (Kiên).** `#define PTE_SH (1L << 8)` trong `kernel/riscv.h`: bit RSW mà phần cứng bỏ qua, được `PTE_FLAGS()` giữ nguyên khi chép sang bảng trang con. Chỉ trang do `shmalloc()` cấp mới có cờ này; `uvmcopy()` dựa vào nó để chọn chia sẻ hay sao chép trang (xem mục 4).
- Trạng thái Tuần 3: `refcnt[]` đã cài trong bộ cấp phát, mọi trang vẫn có `refcnt = 1` vì chưa chia sẻ; `PTE_SH` và `shmalloc()` cài ở Tuần 4.

## 4. Bộ nhớ dùng chung (Kiên)

Futex chỉ có tác dụng giữa các tiến trình khi chúng cùng nhìn thấy một biến. xv6 gốc không có bộ nhớ dùng chung: `fork()` gọi `uvmcopy()` sao chép mọi trang của cha. Nhóm thêm trang nhớ ẩn danh dùng chung và bộ đếm tham chiếu để trang chỉ được giải phóng khi không còn tiến trình nào dùng.

**`shmalloc(npages)`.**

- Cấp `npages` trang mới bằng `kalloc()`, xóa về 0, ánh xạ liên tiếp từ `va = PGROUNDUP(p->sz)` với cờ `PTE_R | PTE_W | PTE_U | PTE_SH`, sau đó cập nhật `p->sz = va + npages * PGSIZE` và trả về `va`.
- Vùng chung nằm ngay trên heap và nằm trong `p->sz`, nên `exit()`, `exec()` và `sbrk` âm tự thu hồi nó qua `uvmunmap()` như bộ nhớ thường, không cần sửa thêm.
- Trang được cấp ngay, không đi qua cấp phát lười `vmfault()`, để PTE đã tồn tại trước khi `fork()`; nếu không, `uvmcopy()` sẽ bỏ qua trang và con không thấy vùng chung.
- Lỗi (`npages <= 0`, `npages > 16` hoặc hết bộ nhớ): gỡ các trang đã cấp bằng `uvmunmap()` và trả về 0.

**Cờ `PTE_SH` = bit 8.** PTE của Sv39 dành bit 8–9 (RSW) cho phần mềm, phần cứng bỏ qua nên việc đánh dấu không ảnh hưởng dịch địa chỉ. Bit 0–4 (V, R, W, X, U) đã dùng, bit 5–7 (G, A, D) có ý nghĩa với phần cứng. `PTE_FLAGS()` lấy 10 bit thấp (`0x3FF`) nên `PTE_SH` được giữ nguyên khi chép cờ sang bảng trang con.

**Những gì phải sửa.**

| Vị trí | Thay đổi |
| --- | --- |
| `kernel/kalloc.c` | Thêm `int refcnt[(PHYSTOP - KERNBASE) / PGSIZE]` (32 768 ô, 128 KB) với khóa riêng `kref.lock`; chỉ số của trang là `(pa - KERNBASE) / PGSIZE` |
| `kalloc()` | Trang lấy từ freelist phải có `refcnt = 0`, đặt thành 1 |
| `kfree()` | Giảm `refcnt`; chỉ đưa trang về freelist khi về 0. Nếu `refcnt <= 0` trước khi giảm thì `panic("kfree: double free")` |
| `freerange()` | Đặt `refcnt = 1` trước khi gọi `kfree()` cho từng trang lúc khởi động; nếu không, `kfree()` thấy 0 và panic ngay khi boot |
| `uvmcopy()` | PTE có `PTE_SH`: `mappages()` cùng PA sang bảng trang con và gọi `kref_inc(pa)`, thay vì `kalloc()` + `memmove()` |
| `uvmunmap()` | Không sửa: vẫn gọi `kfree()`, bộ đếm quyết định trang có bị giải phóng hay không |

Thêm `kref_inc()` và `kref_get()` vào `kernel/defs.h`. `kalloc()` và `kfree()` không bao giờ giữ `kref.lock` và `kmem.lock` cùng lúc nên không có deadlock.

**Ví dụ.** Cha gọi `shmalloc(1)` rồi `fork()`: trang chung có `refcnt = 2`, `futex_key()` của cha và con trả về cùng địa chỉ vật lý. Con thoát trước: `refcnt` về 1, cha vẫn đọc được giá trị con ghi. Cha thoát sau: `refcnt` về 0, trang mới trở lại freelist.

![Luồng xử lý trang chung khi fork() và exit()](../memory-design/so_do/3_luong_fork_exit.svg)

**Lộ trình.** Tuần 3: `refcnt[]` trong `kalloc.c`, `usertests` vẫn PASS. Tuần 4: `PTE_SH`, `sys_shmalloc()`, sửa `uvmcopy()` và chương trình test cha–con dùng chung trang.

## 5. Thư viện tầng user (Lâm)

Thư viện gồm `user/usync.h` khai báo kiểu dữ liệu/giao diện và `user/usync.c` cài đặt các công cụ đồng bộ. Khi dùng giữa nhiều tiến trình, đối tượng đồng bộ và dữ liệu được bảo vệ phải nằm trong trang nhớ chung, khởi tạo trước khi các tiến trình sử dụng.

**Tuần 3 — spinlock.** Phần code đã có trên nhánh `lam` gồm `uspinlock_t` và ba hàm `uspin_init/lock/unlock`. Biến `locked` có hai trạng thái 0/1. Lấy khóa dùng atomic exchange với acquire, lặp đến khi giá trị cũ bằng 0; nhả khóa dùng atomic store với release. Các thao tác này được compiler chuyển thành lệnh RISC-V phù hợp. Spinlock chờ bận, chưa sử dụng futex; test đơn tiến trình nằm trong `user/usynctest.c`.

**Tuần 4–5 — mutex trên futex và demo.** Mutex dự kiến có ba trạng thái: 0 tự do, 1 đang khóa chưa ghi nhận tranh chấp, 2 đang khóa và có thể có người chờ. Đường nhanh lấy khóa bằng CAS 0→1; đường chậm đánh dấu 2 và chờ bằng futex. Khi nhả từ 2→0, đánh thức một người chờ; người được đánh thức phải thử lấy khóa lại. Tuần 5 demo mutex giữa nhiều tiến trình.

![Sơ đồ trạng thái mutex trên futex](images/mutex_state_machine.png)

*Hình 1. Thiết kế logic mutex cho tuần 4, không phải sơ đồ spinlock đã cài tuần 3. Chuyển trạng thái phải dùng thao tác nguyên tử; nếu đường chậm gặp trạng thái 0 thì lấy khóa bằng 0→2 thay vì ngủ.*

**Tuần 6 — semaphore và condition variable.** Semaphore quản lý số quyền sử dụng tài nguyên; condition variable cho phép chờ điều kiện với mutex đi kèm và kiểm tra lại điều kiện sau khi thức dậy.

![Sơ đồ trạng thái semaphore với bộ đếm có dấu](images/semaphore_state_machine.png)

*Hình 2. Mô hình semaphore trừu tượng đã đề xuất ở tuần 2: P là lấy quyền, V là trả quyền; s > 0 là số quyền còn lại, s < 0 biểu diễn -s yêu cầu chưa được cấp quyền. s = 0 không có yêu cầu chưa được cấp quyền, dù tiến trình vừa được cấp quyền có thể chưa được CPU chạy lại. Mô hình giả định cập nhật bộ đếm, đăng ký chờ và cấp quyền/đánh thức được phối hợp an toàn; đây chưa phải thuật toán futex hoàn chỉnh. Không thể chỉ ghép s--/s++ với wait/wake trên s để bảo đảm đúng.*

**Tuần 7 — barrier và ứng dụng.** Bổ sung barrier tái sử dụng để các tiến trình đợi đủ thành viên trong mỗi lượt, cùng bounded buffer và dining philosophers. Chi tiết cài đặt semaphore, condition variable và barrier sẽ được chốt khi triển khai.

## 6. Danh sách file bị sửa (cả nhóm)

| File | Thay đổi | Người phụ trách |
| --- | --- | --- |
| `kernel/syscall.h`, `kernel/syscall.c` | Số hiệu và bảng syscall | Kiệt |
| `kernel/sysproc.c` | `sys_futex_wait`, `sys_futex_wake`, `sys_yield` | Kiệt |
| `kernel/proc.c` | Hàng đợi futex, `wakeup_n` | Kiệt |
| `kernel/vm.c` | `futex_key()` | Lâm |
| `kernel/vm.c` | `uvmcopy()` với `PTE_SH` | Kiên |
| `kernel/kalloc.c` | `refcnt[]`, `kalloc`, `kfree` | Kiên |
| `kernel/riscv.h` | `PTE_SH` | Kiên |
| `user/usync.c`, `user/usync.h` | Thư viện đồng bộ | Lâm |
| `user/futexbench.c` | Benchmark futex/spin/yield | Kiệt |
| `user/projecttests.c` | Khung chạy test PASS/FAIL | Kiệt |

## 7. Kế hoạch kiểm thử (Lâm + Kiệt, viết ở Tuần 3)

- **Chức năng (Lâm).** Tuần 3: `usynctest` kiểm tra khởi tạo, khóa, mở khóa, lấy lại khóa và 100.000 vòng lock/unlock với bộ đếm đúng. Tuần 4–5: P tiến trình tăng bộ đếm chung K lần dưới mutex, kết quả phải bằng P × K và mọi tiến trình kết thúc; phối hợp Kiệt kiểm tra tham số futex và tình huống wait/wake sát nhau. Tuần 6–7: semaphore không cấp vượt số tài nguyên; condition variable luôn kiểm tra lại điều kiện dưới mutex; barrier không cho qua trước khi đủ thành viên; bounded buffer không mất/lặp dữ liệu và dining philosophers không deadlock.
- **Stress (Tuần 7).** Phối hợp Kiệt chạy các ca tranh chấp với `CPUS=1` và `CPUS=4`, lặp ít nhất 10 lần, có timeout để phát hiện treo. Ghi cấu hình, commit và kết quả; chỉ đánh dấu PASS khi thỏa điều kiện kiểm tra. Test đơn tiến trình tuần 3 chưa chứng minh tính đúng khi tranh chấp.
- **Rò rỉ trang (Tuần 6).** Lâm đề xuất tiêu chí kiểm thử; Kiên triển khai đo số trang trống trước/sau chu kỳ cấp trang chung, fork, exit/exec và thu hồi tiến trình con bằng wait. Sau khi bỏ mọi ánh xạ và thu hồi tài nguyên, số trang trống phải trở về mốc trong cùng điều kiện đo, không giảm dần qua các lượt; không giải phóng sớm hoặc double-free.
- **Hồi quy (cả nhóm).** Tuần 8 chạy `usertests` và `grind`, xử lý lỗi trước code freeze; có thể chạy sớm sau mỗi lần tích hợp để phát hiện lỗi.
- **Benchmark (Kiệt).** `user/futexbench.c` cho nhiều tiến trình cùng tranh một khóa, so sánh futex, spinlock và yield lock trên 1–4 CPU. Mỗi điểm dữ liệu chạy ít nhất 5 lần, báo cáo trung bình và độ lệch.
- **CI (Kiệt).** GitHub Actions kiểm tra định dạng, build và chạy `usertests` trên mỗi lần push; từ Tuần 5 chạy thêm `projecttests`.

## 8. Kế hoạch theo tuần (Kiệt)

Mỗi tuần mỗi người một bước nhỏ, các bước ghép lại để đạt bốn mốc của môn: Proposal (Tuần 3), Midterm (Tuần 5), Code freeze (Tuần 8), Demo cuối (Tuần 9).

![Kế hoạch Tuần 3–9](ke_hoach_tuan.png)

Tuần 5 cần mutex trên futex chạy giữa nhiều tiến trình, nên ba việc của Tuần 4 (futex thật, shmalloc, mutex) phải xong cùng lúc.

