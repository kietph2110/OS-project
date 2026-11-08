# Design Proposal – Project 26107: Futexes, semaphores, condition variables

Course: Operating Systems – 20261 · Nhóm: Phạm Nguyễn Anh Kiệt (20233483), Đặng Trung Kiên (20233473), Trần Ngọc Lâm (20223714)

| Mục | Nội dung |
| --- | --- |
| 1. Giới thiệu và mục tiêu | Bài toán, giải pháp futex, sản phẩm cuối kỳ |
| 2. Giao diện system call | `futex_wait`, `futex_wake`, `shmalloc` |
| 3. Cấu trúc dữ liệu kernel | Hàng đợi futex, `refcnt[]`, cờ `PTE_SH` |
| 4. Bộ nhớ dùng chung | `shmalloc`, `uvmcopy`, `kfree` với bộ đếm tham chiếu |
| 5. Thư viện tầng user | spinlock, mutex, semaphore, condition variable, barrier |
| 6. Danh sách file bị sửa | File và nội dung thay đổi |
| 7. Kế hoạch kiểm thử | Chức năng, stress, rò rỉ trang, hồi quy, benchmark, CI |
| 8. Kế hoạch theo tuần | Tuần 3 → Tuần 9 |

## 1. Giới thiệu và mục tiêu

Dự án bổ sung futex vào xv6-riscv để các tiến trình đồng bộ với nhau mà không tốn CPU khi chờ, rồi xây thư viện mutex, semaphore, condition variable và barrier ở tầng user trên nền đó.

**Bài toán.** Chương trình user trong xv6 muốn chờ một khóa chỉ có thể spin, tốn trọn một CPU, hoặc gọi `pause` rồi thử lại. Các tiến trình cũng chưa có vùng nhớ chung để đặt biến khóa.

**Giải pháp.** Khi không tranh chấp, khóa và mở khóa chỉ dùng lệnh nguyên tử RISC-V ở tầng user. Khi có tranh chấp, tiến trình gọi `futex_wait` để ngủ trong kernel và được đánh thức bằng `futex_wake`. Khóa định danh là địa chỉ vật lý của biến futex, nên hai tiến trình dùng chung trang nhớ sẽ chờ trên cùng một khóa.

**Sản phẩm cuối kỳ.** Ba system call `futex_wait`, `futex_wake`, `shmalloc`; thư viện mutex, semaphore, condition variable, barrier; bộ test `projecttests`; benchmark futex, spinlock và yield lock trên 1–4 CPU; demo dining philosophers.

## 2. Giao diện system call

| System call | Số | Ý nghĩa | Trả về |
| --- | --- | --- | --- |
| `int futex_wait(int *addr, int val)` | 23 | Nếu `*addr == val` thì ngủ cho tới khi có `futex_wake` trên cùng địa chỉ | 0 khi được đánh thức; -1 nếu `*addr != val` hoặc `addr` không hợp lệ |
| `int futex_wake(int *addr, int n)` | 24 | Đánh thức tối đa `n` tiến trình đang chờ trên `addr` | Số tiến trình đã đánh thức; -1 nếu `n <= 0` hoặc `addr` không hợp lệ |
| `void *shmalloc(int npages)` | 25 | Cấp `1 ≤ npages ≤ 16` trang nhớ dùng chung; tiến trình con sau `fork()` thấy cùng trang | Địa chỉ ảo của vùng nhớ; 0 nếu lỗi |

`futex_wait` và `futex_wake` đã có khung trên `main`; `shmalloc` cài ở Tuần 4.

**Quy tắc với `addr`.** Phải căn lề 4 byte và nằm trong không gian user đã ánh xạ (`PTE_V` và `PTE_U`). Kernel kiểm tra bằng `futex_key()` (`kernel/vm.c`): trả về địa chỉ vật lý làm khóa, hoặc 0 nếu địa chỉ sai. Hai tiến trình trỏ vào cùng một biến trong trang dùng chung luôn ra cùng một khóa, dù địa chỉ ảo khác nhau. Trang cấp phát lười chưa được chạm tới chưa có PTE nên bị từ chối; biến toàn cục, stack và vùng `malloc` đã được ghi nên không bị ảnh hưởng.

**Vì sao `futex_wait` cần tham số `val`.** Giữa lúc user thấy khóa bận và lúc vào kernel, tiến trình khác có thể đã nhả khóa và gọi `futex_wake`. Kernel đọc lại `*addr` trong lúc giữ khóa của hàng đợi; nếu giá trị đã khác `val` thì trả về ngay thay vì ngủ, tránh mất tín hiệu đánh thức (lost wake-up).

**Hiện trạng (Tuần 3).** `sys_futex_wait` đã so sánh `*addr` với `val` nhưng chưa ngủ (làm ở Tuần 4); `sys_futex_wake` đã gọi `wakeup_n`. Chương trình `futexstub` kiểm tra 7 trường hợp tham số đúng và sai.

## 3. Cấu trúc dữ liệu kernel

**Hàng đợi futex.** Không thêm cấu trúc mới: tiến trình chờ được đánh dấu bằng `p->chan = key`, trong đó `key` là địa chỉ vật lý do `futex_key()` trả về. Một spinlock `futex_lock` trong `kernel/proc.c` bảo vệ bước kiểm tra giá trị. Trang user không bao giờ trùng với đối tượng kernel đang có tiến trình chờ, nên `key` không đụng các kênh chờ khác.

- `wakeup_n(chan, n)` duyệt bảng `proc`, đánh thức tối đa `n` tiến trình có `p->chan == chan` và trả về số đã đánh thức. Nhóm chọn cách này thay cho bảng băm vì `NPROC = 64`, quét toàn bảng đủ nhanh và dùng lại được `sleep_prepare()`/`sleep()`. Hạn chế: một lock chung và không đánh thức theo FIFO; sẽ đánh giá lại bằng benchmark ở Tuần 6.
- Chống mất tín hiệu: `futex_wait` giữ `futex_lock`, so sánh `*addr` với `val`, gọi `sleep_prepare(key)`, nhả lock rồi gọi `sleep()`. Nếu `futex_wake` chen vào giữa, `p->chan` đã bị xóa nên `sleep()` trả về ngay. Thứ tự khóa luôn là `futex_lock` trước `p->lock`.
- Tiến trình bị `kill` khi đang chờ: `futex_wait` kiểm tra `killed(p)` sau khi tỉnh và trả về -1 để tiến trình thoát (Tuần 4).

**Bộ đếm tham chiếu `refcnt[]`** trong `kernel/kalloc.c`:

```c
#define PA2IDX(pa) (((uint64)(pa) - KERNBASE) / PGSIZE)
#define NPHYSPAGES ((PHYSTOP - KERNBASE) / PGSIZE) // 32 768 trang

struct {
  struct spinlock lock;
  int refcnt[NPHYSPAGES];
} kref;
```

- Mỗi trang vật lý 4 KB có một ô `int`, tổng cộng 128 KB, nằm trong BSS nên được xóa về 0 khi khởi động.
- Bất biến: trang trong freelist có `refcnt = 0`; trang đang dùng có `refcnt` bằng số bảng trang đang ánh xạ tới nó.
- Khóa riêng `kref.lock`, tách khỏi `kmem.lock`; `kalloc()` và `kfree()` không bao giờ giữ cả hai khóa cùng lúc.
- `kalloc()` đặt 0 → 1; `kfree()` giảm 1, chỉ trả trang về freelist khi về 0 và `panic` nếu bộ đếm đã ≤ 0 (double-free); `kref_inc()` tăng 1 cho trang đang dùng; `kref_get()` đọc bộ đếm để kiểm thử.
- `freerange()` đặt `refcnt = 1` trước khi gọi `kfree()` cho từng trang lúc khởi động.

**Cờ `PTE_SH`.** `#define PTE_SH (1L << 8)` trong `kernel/riscv.h`: bit RSW mà phần cứng bỏ qua, được `PTE_FLAGS()` giữ nguyên khi chép sang bảng trang con. Chỉ trang do `shmalloc()` cấp mới có cờ này; `uvmcopy()` dựa vào nó để chọn chia sẻ hay sao chép trang (mục 4).

**Trạng thái Tuần 3.** `refcnt[]` đã cài trong bộ cấp phát, mọi trang vẫn có `refcnt = 1` vì chưa chia sẻ; `PTE_SH` và `shmalloc()` cài ở Tuần 4.

## 4. Bộ nhớ dùng chung

Futex chỉ có tác dụng giữa các tiến trình khi chúng cùng nhìn thấy một biến. xv6 gốc không có bộ nhớ dùng chung: `fork()` gọi `uvmcopy()` sao chép mọi trang của cha. Nhóm thêm trang nhớ ẩn danh dùng chung và bộ đếm tham chiếu để trang chỉ được giải phóng khi không còn tiến trình nào dùng.

![Bố cục trang nhớ: trang ẩn danh dùng chung sau fork()](../memory-design/so_do/1_bo_cuc_trang_nho.svg)

**`shmalloc(npages)`.** Cấp `npages` trang mới bằng `kalloc()`, xóa về 0, ánh xạ liên tiếp từ `va = PGROUNDUP(p->sz)` với cờ `PTE_R | PTE_W | PTE_U | PTE_SH`, rồi cập nhật `p->sz = va + npages * PGSIZE` và trả về `va`.

- Vùng chung nằm ngay trên heap và trong `p->sz`, nên `exit()`, `exec()` và `sbrk` âm tự thu hồi nó qua `uvmunmap()` như bộ nhớ thường, không cần sửa thêm.
- Trang được cấp ngay, không đi qua cấp phát lười `vmfault()`, để PTE đã tồn tại trước khi `fork()`; nếu không, `uvmcopy()` sẽ bỏ qua trang và con không thấy vùng chung.
- Lỗi (`npages <= 0`, `npages > 16` hoặc hết bộ nhớ): gỡ các trang đã cấp bằng `uvmunmap()` và trả về 0.

**Cờ `PTE_SH` = bit 8.** PTE của Sv39 dành bit 8–9 (RSW) cho phần mềm nên việc đánh dấu không ảnh hưởng dịch địa chỉ. Bit 0–4 (V, R, W, X, U) đã dùng, bit 5–7 (G, A, D) có ý nghĩa với phần cứng. `PTE_FLAGS()` lấy 10 bit thấp (`0x3FF`) nên `PTE_SH` được giữ nguyên khi chép cờ sang bảng trang con.

**Những gì phải sửa.**

| Vị trí | Thay đổi |
| --- | --- |
| `kernel/kalloc.c` | Thêm `refcnt[]` với khóa riêng `kref.lock` (mục 3) |
| `kalloc()` | Trang lấy từ freelist phải có `refcnt = 0`, đặt thành 1 |
| `kfree()` | Giảm `refcnt`; chỉ đưa trang về freelist khi về 0; `refcnt <= 0` trước khi giảm thì `panic("kfree: double free")` |
| `freerange()` | Đặt `refcnt = 1` trước khi gọi `kfree()` lúc khởi động; nếu không, `kfree()` thấy 0 và panic ngay khi boot |
| `uvmcopy()` | PTE có `PTE_SH`: `mappages()` cùng PA sang bảng trang con và gọi `kref_inc(pa)`, thay vì `kalloc()` + `memmove()` |
| `uvmunmap()` | Không sửa: vẫn gọi `kfree()`, bộ đếm quyết định trang có bị giải phóng hay không |

**Ví dụ.** Cha gọi `shmalloc(1)` rồi `fork()`: trang chung có `refcnt = 2`, `futex_key()` của cha và con trả về cùng địa chỉ vật lý. Con thoát trước: `refcnt` về 1, cha vẫn đọc được giá trị con ghi. Cha thoát sau: `refcnt` về 0, trang trở lại freelist.

![Bảng refcnt và vòng đời một trang](../memory-design/so_do/2_bang_refcount.svg)

![Luồng xử lý trang chung khi fork() và exit()](../memory-design/so_do/3_luong_fork_exit.svg)

**Lộ trình.** Tuần 3: `refcnt[]` trong `kalloc.c`, `usertests` vẫn PASS. Tuần 4: `PTE_SH`, `sys_shmalloc()`, sửa `uvmcopy()` và chương trình test cha–con dùng chung trang.

## 5. Thư viện tầng user

Thư viện gồm `user/usync.h` khai báo kiểu dữ liệu và giao diện, `user/usync.c` cài đặt các công cụ đồng bộ. Khi dùng giữa nhiều tiến trình, đối tượng đồng bộ và dữ liệu được bảo vệ phải nằm trong trang nhớ chung, khởi tạo trước khi các tiến trình sử dụng.

**Tuần 3 — spinlock.** Đã có `uspinlock_t` và ba hàm `uspin_init/lock/unlock`. Biến `locked` có hai trạng thái 0/1. Lấy khóa dùng atomic exchange với acquire, lặp đến khi giá trị cũ bằng 0; nhả khóa dùng atomic store với release. Spinlock chờ bận, chưa dùng futex; test đơn tiến trình nằm trong `user/usynctest.c`.

**Tuần 4–5 — mutex trên futex.** Mutex có ba trạng thái: 0 tự do, 1 đang khóa chưa ghi nhận tranh chấp, 2 đang khóa và có thể có người chờ. Đường nhanh lấy khóa bằng CAS 0→1; đường chậm đánh dấu 2 và chờ bằng futex. Khi nhả từ 2→0, đánh thức một người chờ; người được đánh thức phải thử lấy khóa lại. Tuần 5 demo mutex giữa nhiều tiến trình.

![Sơ đồ trạng thái mutex trên futex](images/mutex_state_machine.png)

*Hình 1. Thiết kế logic mutex cho Tuần 4, không phải sơ đồ spinlock đã cài ở Tuần 3. Chuyển trạng thái phải dùng thao tác nguyên tử; nếu đường chậm gặp trạng thái 0 thì lấy khóa bằng 0→2 thay vì ngủ.*

**Tuần 6 — semaphore và condition variable.** Semaphore quản lý số quyền sử dụng tài nguyên; condition variable cho phép chờ điều kiện với mutex đi kèm và kiểm tra lại điều kiện sau khi thức dậy.

![Sơ đồ trạng thái semaphore với bộ đếm có dấu](images/semaphore_state_machine.png)

*Hình 2. Mô hình semaphore trừu tượng: P là lấy quyền, V là trả quyền; s > 0 là số quyền còn lại, s < 0 biểu diễn -s yêu cầu chưa được cấp quyền. Mô hình giả định cập nhật bộ đếm, đăng ký chờ và đánh thức được phối hợp an toàn; đây chưa phải thuật toán futex hoàn chỉnh vì không thể chỉ ghép s--/s++ với wait/wake trên s.*

**Tuần 7 — barrier và ứng dụng.** Barrier tái sử dụng để các tiến trình đợi đủ thành viên trong mỗi lượt, cùng bounded buffer và dining philosophers. Chi tiết cài đặt semaphore, condition variable và barrier sẽ được chốt khi triển khai.

## 6. Danh sách file bị sửa

| File | Thay đổi |
| --- | --- |
| `kernel/syscall.h`, `kernel/syscall.c`, `user/usys.pl`, `user/user.h` | Số hiệu, bảng syscall và khai báo phía user |
| `kernel/sysproc.c` | `sys_futex_wait`, `sys_futex_wake`, `sys_shmalloc`, `sys_yield`, `sys_freemem` |
| `kernel/proc.c`, `kernel/defs.h` | `futex_lock`, `wakeup_n` |
| `kernel/vm.c` | `futex_key()`, `uvmcopy()` với `PTE_SH` |
| `kernel/kalloc.c` | `refcnt[]`, `kalloc`, `kfree`, `freerange`, `kref_inc`, `kref_get`, đếm trang trống |
| `kernel/riscv.h` | `PTE_SH` |
| `user/usync.c`, `user/usync.h` | Thư viện đồng bộ |
| `user/futexstub.c`, `user/usynctest.c` | Test tham số futex, test spinlock |
| `user/futexbench.c` | Benchmark futex/spin/yield |
| `user/projecttests.c` | Khung chạy test PASS/FAIL |
| `Makefile`, `.github/workflows/test.yml` | Thêm chương trình user, thêm bước CI |

## 7. Kế hoạch kiểm thử

- **Chức năng.** Tuần 3: `usynctest` kiểm tra khởi tạo, khóa, mở khóa, lấy lại khóa và 100.000 vòng lock/unlock với bộ đếm đúng. Tuần 4–5: P tiến trình tăng bộ đếm chung K lần dưới mutex, kết quả phải bằng P × K và mọi tiến trình kết thúc; kiểm tra tham số futex và tình huống wait/wake sát nhau, kể cả tiến trình bị kill khi đang chờ. Tuần 6–7: semaphore không cấp vượt số tài nguyên; condition variable luôn kiểm tra lại điều kiện dưới mutex; barrier không cho qua trước khi đủ thành viên; bounded buffer không mất hay lặp dữ liệu; dining philosophers không deadlock.
- **Stress (Tuần 7).** Chạy các ca tranh chấp với `CPUS=1` và `CPUS=4`, lặp ít nhất 10 lần, có timeout để phát hiện treo. Ghi cấu hình, commit và kết quả; chỉ đánh dấu PASS khi thỏa điều kiện kiểm tra. Test đơn tiến trình của Tuần 3 chưa chứng minh tính đúng khi tranh chấp.
- **Rò rỉ trang (Tuần 6).** Đo số trang trống bằng syscall `freemem()` trước và sau chu kỳ cấp trang chung, fork, exit/exec và thu hồi tiến trình con bằng wait. Sau khi bỏ mọi ánh xạ và thu hồi tài nguyên, số trang trống phải trở về mốc trong cùng điều kiện đo, không giảm dần qua các lượt; không giải phóng sớm hoặc double-free.
- **Hồi quy.** Tuần 8 chạy `usertests` và `grind`, xử lý lỗi trước code freeze; có thể chạy sớm sau mỗi lần tích hợp.
- **Benchmark.** `user/futexbench.c` cho nhiều tiến trình cùng tranh một khóa, so sánh futex, spinlock và yield lock trên 1–4 CPU. Yield lock cần system call `yield()` mà xv6 chưa có, nên thêm cùng benchmark ở Tuần 6. Mỗi điểm dữ liệu chạy ít nhất 5 lần, báo cáo trung bình và độ lệch.
- **CI.** GitHub Actions kiểm tra định dạng, build và chạy `usertests` trên mỗi lần push; từ Tuần 5 chạy thêm `projecttests`.

## 8. Kế hoạch theo tuần

Mỗi tuần mỗi người một bước nhỏ, các bước ghép lại để đạt bốn mốc của môn: Proposal (Tuần 3), Midterm (Tuần 5), Code freeze (Tuần 8), Demo cuối (Tuần 9).

![Kế hoạch Tuần 3–9](ke_hoach_tuan.png)

Tuần 5 cần mutex trên futex chạy giữa nhiều tiến trình, nên ba việc của Tuần 4 (futex thật, `shmalloc`, mutex) phải xong cùng lúc.
