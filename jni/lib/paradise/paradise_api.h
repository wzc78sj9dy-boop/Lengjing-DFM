#ifndef PARADISE_API_H
#define PARADISE_API_H

#include <stdint.h>
#include <sys/types.h>
#include <stddef.h>

#define PARADISE_GYRO_MASK_GYRO (1u << 0)
#define PARADISE_GYRO_MASK_UNCAL (1u << 1)
#define PARADISE_GYRO_MASK_ALL (PARADISE_GYRO_MASK_GYRO | PARADISE_GYRO_MASK_UNCAL)

/* Hardware breakpoint definitions */
#define HWBP_MAX_POINTS 16
#define HWBP_MAX_RECORDS 0x100

/* Per-register operation mask constants (2 bits per register) */
#define HWBP_OP_NONE  0x0
#define HWBP_OP_READ  0x1
#define HWBP_OP_WRITE 0x2

/* Helper macros for manipulating per-register operation masks */
#define HWBP_SET_MASK(record, reg, op)                              \
    do {                                                            \
        int byte_idx = (reg) >> 2;                                  \
        int bit_offset = ((reg) & 0x3) << 1;                        \
        (record)->mask[byte_idx] &= ~(0x3 << bit_offset);           \
        (record)->mask[byte_idx] |= ((op) & 0x3) << bit_offset;     \
    } while (0)

#define HWBP_GET_MASK(record, reg) \
    (((record)->mask[(reg) >> 2] >> (((reg) & 0x3) << 1)) & 0x3)

/* Register index for mask operations */
enum hwbp_reg_idx {
    HWBP_IDX_PC = 0,
    HWBP_IDX_HIT_COUNT,
    HWBP_IDX_LR,
    HWBP_IDX_SP,
    HWBP_IDX_ORIG_X0,
    HWBP_IDX_SYSCALLNO,
    HWBP_IDX_PSTATE,
    HWBP_IDX_X0, HWBP_IDX_X1, HWBP_IDX_X2, HWBP_IDX_X3,
    HWBP_IDX_X4, HWBP_IDX_X5, HWBP_IDX_X6, HWBP_IDX_X7,
    HWBP_IDX_X8, HWBP_IDX_X9, HWBP_IDX_X10, HWBP_IDX_X11,
    HWBP_IDX_X12, HWBP_IDX_X13, HWBP_IDX_X14, HWBP_IDX_X15,
    HWBP_IDX_X16, HWBP_IDX_X17, HWBP_IDX_X18, HWBP_IDX_X19,
    HWBP_IDX_X20, HWBP_IDX_X21, HWBP_IDX_X22, HWBP_IDX_X23,
    HWBP_IDX_X24, HWBP_IDX_X25, HWBP_IDX_X26, HWBP_IDX_X27,
    HWBP_IDX_X28, HWBP_IDX_X29,
    HWBP_IDX_FPSR, HWBP_IDX_FPCR,
    HWBP_IDX_Q0, HWBP_IDX_Q1, HWBP_IDX_Q2, HWBP_IDX_Q3,
    HWBP_IDX_Q4, HWBP_IDX_Q5, HWBP_IDX_Q6, HWBP_IDX_Q7,
    HWBP_IDX_Q8, HWBP_IDX_Q9, HWBP_IDX_Q10, HWBP_IDX_Q11,
    HWBP_IDX_Q12, HWBP_IDX_Q13, HWBP_IDX_Q14, HWBP_IDX_Q15,
    HWBP_IDX_Q16, HWBP_IDX_Q17, HWBP_IDX_Q18, HWBP_IDX_Q19,
    HWBP_IDX_Q20, HWBP_IDX_Q21, HWBP_IDX_Q22, HWBP_IDX_Q23,
    HWBP_IDX_Q24, HWBP_IDX_Q25, HWBP_IDX_Q26, HWBP_IDX_Q27,
    HWBP_IDX_Q28, HWBP_IDX_Q29, HWBP_IDX_Q30, HWBP_IDX_Q31,
    HWBP_MAX_REG_COUNT
};

enum hwbp_type {
    HWBP_BREAKPOINT_EMPTY = 0,
    HWBP_BREAKPOINT_R = 1,
    HWBP_BREAKPOINT_W = 2,
    HWBP_BREAKPOINT_RW = HWBP_BREAKPOINT_R | HWBP_BREAKPOINT_W,
    HWBP_BREAKPOINT_X = 4,
};

enum hwbp_len {
    HWBP_BREAKPOINT_LEN_1 = 1,
    HWBP_BREAKPOINT_LEN_2 = 2,
    HWBP_BREAKPOINT_LEN_3 = 3,
    HWBP_BREAKPOINT_LEN_4 = 4,
    HWBP_BREAKPOINT_LEN_5 = 5,
    HWBP_BREAKPOINT_LEN_6 = 6,
    HWBP_BREAKPOINT_LEN_7 = 7,
    HWBP_BREAKPOINT_LEN_8 = 8,
};

enum hwbp_scope {
    SCOPE_MAIN_THREAD = 0,
    SCOPE_OTHER_THREADS = 1,
    SCOPE_ALL_THREADS = 2
};

struct hwbp_record {
    uint8_t mask[18];
    pid_t tid;              /* thread id that hit this record */
    uint64_t hit_count;
    uint64_t pc;
    uint64_t lr;
    uint64_t sp;
    uint64_t orig_x0;
    uint64_t syscallno;
    uint64_t pstate;
    uint64_t x0, x1, x2, x3, x4, x5, x6, x7, x8, x9;
    uint64_t x10, x11, x12, x13, x14, x15, x16, x17, x18, x19;
    uint64_t x20, x21, x22, x23, x24, x25, x26, x27, x28, x29;
    uint32_t fpsr;
    uint32_t fpcr;
    __uint128_t q0, q1, q2, q3, q4, q5, q6, q7, q8, q9;
    __uint128_t q10, q11, q12, q13, q14, q15, q16, q17, q18, q19;
    __uint128_t q20, q21, q22, q23, q24, q25, q26, q27, q28, q29;
    __uint128_t q30, q31;
};

struct hwbp_point_config {
    enum hwbp_type bt;
    enum hwbp_len bl;
    enum hwbp_scope bs;
    uint64_t hit_addr;
};

class paradise_driver {
private:
    pid_t pid;
    int fd;

    int install_driver_fd();
    void ensure_connected();

public:
    // 构造时连接 Paradise 驱动
    paradise_driver();
    
    // 构析方法
    ~paradise_driver();

    // 初始化目标 pid，读写前务必调用一次
    void initialize(pid_t target_pid);
    
    // 获取进程pid，传入进程名称，从内核层安全获取pid
    pid_t get_pid(const char *name);
    
    // 获取模块基址，传入模块名，从内核层安全获取模块基址
    uintptr_t get_module_base(const char *name);

    // 获取模块映射范围 [base, end)，end 为最高一段 VMA 的 vm_end，便于一次覆盖整个 so，如 libc 多段
    bool get_module_range(const char *name, uintptr_t *base_out, uintptr_t *end_out);

    // 获取模块结束地址，传入模块名，从内核层安全获取模块结束地址
    uintptr_t get_module_end(const char *name);

    /*
    用法示例：
        uintptr_t lo, hi;
        if (get_module_range("libc.so", &lo, &hi)) {
            // 映射包络为 [lo, hi)，按需分段读取
        }
        // 或仅获取结束地址：
        uintptr_t end = get_module_end("libc.so");
    */
    
    // 更新陀螺仪数据
    bool gyro_update(float x, float y, uint32_t type_mask = PARADISE_GYRO_MASK_ALL, bool enable = true);
    
    // 检查进程是否存活 (alive_out: 1为存活，0为未存活)
    bool is_process_alive(pid_t check_pid, int *alive_out);
    
    // 隐藏或取消隐藏指定进程
    bool hide_process(pid_t target_pid, bool hide);
    
    // 隐藏或取消隐藏指定路径
    bool hide_path(const char *path, bool hide);
    
    // 获取进程列表位图
    bool list_processes(uint8_t *bitmap, size_t bitmap_size, size_t *process_count_out);
    
    // 硬件层读取数据，传入地址、接收指针、类型大小
    bool read(uintptr_t addr, void *buffer, size_t size);
    
    // 硬件层修改数据，传入地址、数据指针、类型大小
    bool write(uintptr_t addr, void *buffer, size_t size);

    // 内核层映射读取数据，传入地址、接收指针、类型大小
    bool read_fast(uintptr_t addr, void *buffer, size_t size);

    // 内核层映射修改数据，传入地址、数据指针、类型大小
    bool write_fast(uintptr_t addr, void *buffer, size_t size);

    // 初始化触摸注入，传入用户屏幕分辨率用于坐标映射
    bool touch_init(int screen_width, int screen_height);

    // 手指按下
    bool touch_down(int slot, int x, int y);

    // 手指移动
    bool touch_move(int slot, int x, int y);

    // 手指抬起
    bool touch_up(int slot);

    // 销毁触摸注入
    bool touch_destroy();

    // 获取硬件断点/观察点槽位数量
    bool hwbp_get_info(uint64_t *num_brps, uint64_t *num_wrps);

    // 设置硬件断点，points 数组最多 HWBP_MAX_POINTS 个，hit_addr=0 的条目被忽略
    bool hwbp_set(pid_t target_pid, struct hwbp_point_config *points, int count);

    // 移除指定进程的所有硬件断点
    bool hwbp_remove(pid_t target_pid);

    // 读取断点命中记录，point_index 指定哪个观测点 (0-15)
    // records_out 为输出缓冲区，max_records 为最多读取的记录数
    // 返回实际读取的记录数，-1 表示错误
    int hwbp_read_records(pid_t target_pid, int point_index,
                          struct hwbp_record *records_out, int max_records,
                          uint64_t *hit_addr_out, int *total_records_out);

    // 写回寄存器值：修改指定记录的 mask 和目标寄存器值
    // 将 mask 中对应位设为 HWBP_OP_WRITE 的寄存器，在下次断点命中时写入 record 中的值
    // point_index: 观测点索引 (0-15)，record_index: 记录槽位索引
    bool hwbp_write_record(pid_t target_pid, int point_index, int record_index,
                           const struct hwbp_record *record);

    // 直接读取目标进程的浮点寄存器
    // reg_mask: bit0=V0, bit1=V1, ... bit31=V31
    // vregs_out: 128-bit * 32 输出缓冲区, fpsr_out/fpcr_out: 状态寄存器输出
    bool fpr_read(pid_t target_pid, uint32_t reg_mask,
                  uint8_t vregs_out[32][16], uint32_t *fpsr_out, uint32_t *fpcr_out);

    // 直接写入目标进程的浮点寄存器
    // reg_mask: 哪些 V 寄存器需要写入
    // vregs: 128-bit * 32 输入值
    bool fpr_write(pid_t target_pid, uint32_t reg_mask,
                   const uint8_t vregs[32][16]);

    // 直接读取目标进程的通用寄存器 X0-X30 + SP + PC + PSTATE
    bool gpr_read(pid_t target_pid, uint64_t regs_out[31],
                  uint64_t *sp_out, uint64_t *pc_out, uint64_t *pstate_out);

    // 直接写入目标进程的通用寄存器 X0-X30 + SP + PC
    bool gpr_write(pid_t target_pid, const uint64_t regs[31],
                   uint64_t sp, uint64_t pc);

    // 批量写入 float 值到指定 V 寄存器的低 32 位
    bool fpr_write_floats(pid_t target_pid, uint32_t count,
                          const uint32_t reg_indices[8], const float values[8]);

    // 同时完成读取和写入
    bool fpr_read_modify_write(pid_t target_pid, uint32_t read_mask,
                               uint32_t write_count, const uint32_t write_indices[8],
                               const float write_values[8], uint8_t out_vregs[32][16]);

    // PTE UXN 执行断点；
    bool ptebp_get_info(uint64_t *num_brps, uint64_t *num_wrps);
    bool ptebp_set(pid_t target_pid, struct hwbp_point_config *points, int count);
    bool ptebp_remove(pid_t target_pid);
    int ptebp_read_records(pid_t target_pid, int point_index,
                           struct hwbp_record *records_out, int max_records,
                           uint64_t *hit_addr_out, int *total_records_out);
    bool ptebp_write_record(pid_t target_pid, int point_index, int record_index,
                            const struct hwbp_record *record);

    // 模板方法，传入地址，返回地址上的值
    template <typename T>
    T read(uintptr_t addr)
    {
        T res{};
        if (this->read(addr, &res, sizeof(T)))
            return res;
        return {};
    }

    // 模板方法，传入地址，修改后的值
    template <typename T>
    bool write(uintptr_t addr, T value)
    {
        return this->write(addr, &value, sizeof(T));
    }

    // 基于 vmap 的模板读取
    template <typename T>
    T read_fast(uintptr_t addr)
    {
        T res{};
        if (this->read_fast(addr, &res, sizeof(T)))
            return res;
        return {};
    }

    // 基于 vmap 的模板写入
    template <typename T>
    bool write_fast(uintptr_t addr, T value)
    {
        return this->write_fast(addr, &value, sizeof(T));
    }
};

#endif
