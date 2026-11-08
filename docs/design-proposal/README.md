# Design Proposal – Project 26107: Futexes, semaphores, condition variables

Course: Operating Systems – 20261 · Nhóm: Phạm Nguyễn Anh Kiệt (20233483), Đặng Trung Kiên (20233473), Trần Ngọc Lâm (20223714)

## 1. Giới thiệu và mục tiêu

Dự án bổ sung futex vào xv6-riscv để các tiến trình đồng bộ với nhau mà không tốn CPU khi chờ, rồi xây thư viện mutex, semaphore, condition variable và barrier ở tầng user trên nền đó. Hiện chương trình user muốn chờ khóa chỉ có thể spin (tốn trọn một CPU) hoặc gọi `pause` rồi thử lại, và các tiến trình chưa có vùng nhớ chung để đặt biến khóa.

Khi không tranh chấp, khóa và mở khóa chỉ dùng lệnh nguyên tử RISC-V ở tầng user. Khi có tranh chấp, tiến trình gọi `futex_wait` để ngủ trong kernel và được đánh thức bằng `futex_wake`. Khóa định danh là địa chỉ vật lý của biến futex, nên hai tiến trình dùng chung một trang nhớ sẽ chờ trên cùng một khóa.

**Sản phẩm cuối kỳ:** ba system call `futex_wait`, `futex_wake`, `shmalloc`; thư viện mutex, semaphore, condition variable, barrier; bộ test `projecttests`; benchmark futex, spinlock và yield lock trên 1–4 CPU; demo dining philosophers.

## 2. System call mới

| System call | Số | Ý nghĩa | Trả về |
| --- | --- | --- | --- |
| `int futex_wait(int *addr, int val)` | 23 | Nếu `*addr == val` thì ngủ đến khi có `futex_wake` trên cùng địa chỉ | 0 khi được đánh thức; -1 nếu `*addr != val`, `addr` sai, hoặc bị kill khi chờ |
| `int futex_wake(int *addr, int n)` | 24 | Đánh thức tối đa `n` tiến trình đang chờ trên `addr` | Số tiến trình đã đánh thức; -1 nếu `n <= 0` hoặc `addr` sai |
| `void *shmalloc(int npages)` | 25 | Cấp `1 ≤ npages ≤ 16` trang dùng chung; con sau `fork()` thấy cùng trang | Địa chỉ ảo; 0 nếu lỗi |
| `int freemem(void)` | 26 | Số trang vật lý còn trống, dùng cho test rò rỉ | Số trang |

`addr` phải căn lề 4 byte và nằm trong vùng user đã ánh xạ (`PTE_V`, `PTE_U`); `futex_key()` trong `kernel/vm.c` trả về địa chỉ vật lý làm khóa, hoặc 0 nếu sai. Hai tiến trình trỏ vào cùng một biến trong trang chung ra cùng một khóa dù địa chỉ ảo khác nhau. Tham số `val` tránh mất tín hiệu đánh thức: nếu giữa lúc user thấy khóa bận và lúc vào kernel có tiến trình khác nhả khóa, kernel đọc lại `*addr` trong lúc giữ khóa hàng đợi, thấy khác `val` và trả về ngay thay vì ngủ.

**Hiện trạng (Tuần 3):** `futex_wait` đã so sánh `*addr` với `val` nhưng chưa ngủ; `futex_wake` đã gọi `wakeup_n`; `futexstub` kiểm tra 7 trường hợp PASS. `shmalloc` và `freemem` cài ở Tuần 4 và Tuần 6.

## 3. Cấu trúc dữ liệu kernel

**Hàng đợi futex.** Không thêm cấu trúc mới: tiến trình chờ đặt `p->chan = key` (địa chỉ vật lý từ `futex_key()`), một spinlock `futex_lock` trong `kernel/proc.c` bảo vệ bước kiểm tra giá trị. `wakeup_n(chan, n)` duyệt bảng `proc`, đánh thức tối đa `n` tiến trình có `p->chan == chan` và trả về số đã đánh thức. Nhóm chọn cách này thay cho bảng băm vì `NPROC = 64` nên quét toàn bảng đủ nhanh và dùng lại được `sleep_prepare()`/`sleep()`. Hạn chế: một lock chung, không đánh thức theo FIFO; đánh giá lại bằng benchmark ở Tuần 6.

- Chống mất tín hiệu: `futex_wait` giữ `futex_lock`, so sánh `*addr` với `val`, gọi `sleep_prepare(key)`, nhả lock rồi gọi `sleep()`. Nếu `futex_wake` chen vào giữa thì `p->chan` đã bị xóa và `sleep()` trả về ngay. Thứ tự khóa luôn là `futex_lock` rồi `p->lock`.
- Tiến trình bị `kill` khi đang chờ: sau khi tỉnh, `futex_wait` kiểm tra `killed(p)` và trả về -1 (Tuần 4).

**Bộ đếm tham chiếu** trong `kernel/kalloc.c`: mỗi trang vật lý có một ô `int`, bảo vệ bằng `kref.lock` riêng, tách khỏi `kmem.lock`.

```c
#define PA2IDX(pa) (((uint64)(pa) - KERNBASE) / PGSIZE)
#define NPHYSPAGES ((PHYSTOP - KERNBASE) / PGSIZE) // 32 768 trang, 128 KB
struct { struct spinlock lock; int refcnt[NPHYSPAGES]; } kref;
```

Bất biến: trang trong freelist có `refcnt = 0`; trang đang dùng có `refcnt` bằng số bảng trang ánh xạ tới nó. `kalloc()` đặt 0 → 1; `kfree()` giảm 1, chỉ trả trang về freelist khi về 0 và `panic` nếu đã ≤ 0 (double-free); `kref_inc()` tăng 1; `freerange()` đặt 1 trước khi gọi `kfree()` lúc khởi động.

**Cờ PTE_SH** = `1L << 8` trong `kernel/riscv.h`: bit RSW mà phần cứng bỏ qua, `PTE_FLAGS()` giữ nguyên khi chép sang bảng trang con. Chỉ trang do `shmalloc()` cấp có cờ này.

**Trạng thái Tuần 3:** `refcnt[]` đã chạy trong bộ cấp phát (mọi trang `refcnt = 1`, `usertests` PASS); `PTE_SH` và `shmalloc()` cài ở Tuần 4.

## 4. Bộ nhớ dùng chung

xv6 gốc không có bộ nhớ dùng chung: `fork()` gọi `uvmcopy()` sao chép mọi trang. `shmalloc(npages)` cấp trang bằng `kalloc()`, xóa về 0, ánh xạ liên tiếp từ `PGROUNDUP(p->sz)` với cờ `PTE_R|PTE_W|PTE_U|PTE_SH`, cập nhật `p->sz`, trả về địa chỉ đầu. Trang được cấp ngay, không đi qua cấp phát lười, để PTE đã tồn tại trước `fork()`. Vì vùng chung nằm trong `p->sz`, `exit()`, `exec()` và `sbrk` âm tự thu hồi qua `uvmunmap()`. Lỗi giữa chừng: gỡ các trang đã cấp và trả về 0.

| Vị trí | Thay đổi |
| --- | --- |
| `uvmcopy()` | PTE có `PTE_SH`: `mappages()` cùng PA sang bảng trang con và `kref_inc(pa)`, thay vì `kalloc()` + `memmove()` |
| `kfree()` | Giảm `refcnt`, chỉ giải phóng khi về 0; `uvmunmap()` không cần sửa |

Ví dụ: cha gọi `shmalloc(1)` rồi `fork()` thì trang chung có `refcnt = 2` và `futex_key()` của cha và con cùng một địa chỉ vật lý; con thoát thì `refcnt` về 1, cha thoát sau thì về 0 và trang trở lại freelist.

![Luồng xử lý trang chung khi fork() và exit()](../memory-design/so_do/3_luong_fork_exit.svg)

## 5. Thư viện tầng user

`user/usync.h` và `user/usync.c`; khi dùng giữa nhiều tiến trình, đối tượng đồng bộ và dữ liệu được bảo vệ phải nằm trong trang chung.

- **Tuần 3 — spinlock (đã cài).** `uspinlock_t` với `uspin_init/lock/unlock`: lấy khóa bằng atomic exchange (acquire) lặp đến khi giá trị cũ bằng 0, nhả khóa bằng atomic store (release). Chưa dùng futex; test đơn tiến trình trong `usynctest.c`.
- **Tuần 4–5 — mutex.** Ba trạng thái: 0 tự do, 1 đang khóa, 2 đang khóa và có thể có người chờ. Đường nhanh CAS 0→1; đường chậm đánh dấu 2 rồi `futex_wait`; nhả 2→0 thì `futex_wake` một người, người đó phải thử lấy khóa lại (nếu gặp 0 thì lấy bằng 0→2).
- **Tuần 6 — semaphore, condition variable.** Semaphore dùng bộ đếm có dấu (`s > 0` là số quyền còn lại, `s < 0` là `-s` yêu cầu đang chờ); condition variable chờ với mutex đi kèm và kiểm tra lại điều kiện sau khi thức dậy. Cập nhật bộ đếm và đăng ký chờ phải nguyên tử, không thể chỉ ghép `s--`/`s++` với wait/wake.
- **Tuần 7 — barrier, ứng dụng.** Barrier tái sử dụng, bounded buffer, dining philosophers.

![Sơ đồ trạng thái mutex trên futex (thiết kế Tuần 4)](images/mutex_state_machine.png)

## 6. Danh sách file bị sửa

| File | Thay đổi |
| --- | --- |
| `kernel/syscall.h`, `kernel/syscall.c`, `user/usys.pl`, `user/user.h` | Số hiệu, bảng syscall, khai báo phía user |
| `kernel/sysproc.c` | `sys_futex_wait`, `sys_futex_wake`, `sys_shmalloc`, `sys_freemem`, `sys_yield` |
| `kernel/proc.c`, `kernel/defs.h` | `futex_lock`, `wakeup_n` |
| `kernel/vm.c` | `futex_key()`, `uvmcopy()` với `PTE_SH` |
| `kernel/kalloc.c`, `kernel/riscv.h` | `refcnt[]`, `kref_inc`, đếm trang trống, `PTE_SH` |
| `user/usync.c`, `user/usync.h` | Thư viện đồng bộ |
| `user/futexstub.c`, `user/usynctest.c`, `user/projecttests.c` | Test tham số futex, test spinlock, khung PASS/FAIL |
| `user/futexbench.c` | Benchmark futex, spinlock, yield lock |
| `Makefile`, `.github/workflows/test.yml` | Chương trình user mới, bước CI |

## 7. Kế hoạch kiểm thử

- **Chức năng.** Tuần 3: `usynctest` (khóa, mở khóa, lấy lại khóa, 100.000 vòng lock/unlock). Tuần 4–5: P tiến trình tăng bộ đếm chung K lần dưới mutex, kết quả phải bằng P × K và mọi tiến trình kết thúc; test tham số futex, wait/wake sát nhau, tiến trình bị kill khi chờ. Tuần 6–7: semaphore không cấp vượt số tài nguyên; condition variable kiểm tra lại điều kiện; barrier không cho qua trước khi đủ thành viên; bounded buffer không mất hay lặp dữ liệu; dining philosophers không deadlock.
- **Stress (Tuần 7).** Các ca tranh chấp với `CPUS=1` và `CPUS=4`, lặp ít nhất 10 lần, có timeout để phát hiện treo; ghi cấu hình, commit và kết quả. Test đơn tiến trình của Tuần 3 chưa chứng minh tính đúng khi tranh chấp.
- **Rò rỉ trang (Tuần 6).** Đo `freemem()` trước và sau chu kỳ cấp trang chung, fork, exit/exec, wait; số trang trống phải về đúng mốc ban đầu và không giảm dần qua các lượt, không giải phóng sớm hay double-free.
- **Hồi quy.** Tuần 8 chạy `usertests` và `grind` trước code freeze; có thể chạy sớm sau mỗi lần tích hợp.
- **Benchmark.** `futexbench` cho nhiều tiến trình tranh một khóa, so sánh futex, spinlock, yield lock trên 1–4 CPU; yield lock cần syscall `yield()` (thêm cùng benchmark ở Tuần 6); mỗi điểm chạy ít nhất 5 lần, báo cáo trung bình và độ lệch.
- **CI.** GitHub Actions kiểm tra định dạng, build và chạy `usertests` mỗi lần push; từ Tuần 5 chạy thêm `projecttests`.

## 8. Kế hoạch theo tuần

Mỗi tuần mỗi người một bước nhỏ, các bước ghép lại để đạt bốn mốc: Proposal (Tuần 3), Midterm (Tuần 5), Code freeze (Tuần 8), Demo cuối (Tuần 9). Tuần 5 cần mutex trên futex chạy giữa nhiều tiến trình nên ba việc của Tuần 4 (futex thật, `shmalloc`, mutex) phải xong cùng lúc.

![Kế hoạch Tuần 3–9](ke_hoach_tuan.png)
