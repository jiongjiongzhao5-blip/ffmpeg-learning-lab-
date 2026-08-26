// ============================================================================
// 文件名：annexb_parser.cpp
// 功能：解析 H.264 视频码流中的 Annex-B 格式 NALU（网络抽象层单元）
// 说明：支持起始码 0x000001（3字节）和 0x00000001（4字节），
//       采用零拷贝方式，只记录指针和长度，不复制数据。
// 标准参考：ITU-T H.264 (ISO/IEC 14496-10) 第 7.4.1 节
// ============================================================================

#include <iostream>
#include <vector>
#include <cstdint>      // uint8_t, uint32_t 等定长类型
#include <string_view>  // （本示例未实际使用，可忽略）
#include <iomanip>      // 格式化输出（本示例未使用）

// ----------------------------------------------------------------------------
// 枚举类型：NALU 单元类型（根据 H.264 标准 Table 7-1）
// ----------------------------------------------------------------------------
enum class NalUnitType : uint8_t {
    Unspecified = 0,   // 未指定
    NonIDR = 1,   // 非 IDR 帧的条带（Slice）
    DataPartitionA = 2,  // 数据分区 A
    DataPartitionB = 3,  // 数据分区 B
    DataPartitionC = 4,  // 数据分区 C
    IDR = 5,   // IDR 关键帧条带（瞬时解码刷新）
    SEI = 6,   // 补充增强信息（如用户数据）
    SPS = 7,   // 序列参数集（编码序列的全局参数）
    PPS = 8,   // 图像参数集（每帧的局部参数）
    AUD = 9,   // 访问单元分隔符
    EndOfSeq = 10,  // 序列结束
    EndOfStream = 11,  // 码流结束
    FillerData = 12   // 填充数据
};

// ----------------------------------------------------------------------------
// 结构体：描述一个完整的 NALU 信息（包含起始码）
// ----------------------------------------------------------------------------
struct NaluDescriptor {
    uint8_t forbidden_zero_bit{ 0 };   // 禁止位（必须为 0，否则码流错误）
    uint8_t nal_ref_idc{ 0 };          // 重要性标识（0~3，值越大越重要）
    NalUnitType nal_unit_type{ NalUnitType::Unspecified }; // NALU 类型
    size_t start_code_len{ 0 };        // 起始码长度（3 或 4 字节）
    const uint8_t* payload{ nullptr }; // 指向实际载荷数据的首地址（跳过起始码和 NAL 头）
    size_t payload_size{ 0 };          // 载荷数据的字节数（不含起始码和 NAL 头）
    const uint8_t* nalu_start{ nullptr }; // 指向包含起始码的完整 NALU 起始地址
    size_t total_size{ 0 };            // 完整 NALU 的总字节数（含起始码）
};

// ----------------------------------------------------------------------------
// 类：Annex-B 格式解析器（静态方法）
// ----------------------------------------------------------------------------
class AnnexbParser {
public:
    // ------------------------------------------------------------------------
    // 静态方法：在 [p, end) 范围内查找下一个 Annex-B 起始码
    // 参数：
    //   p          - 当前搜索起始位置（输入）
    //   end        - 搜索范围的结束地址（输入）
    //   out_code_len - 输出找到的起始码长度（3 或 4）
    // 返回值：
    //   找到则返回指向起始码首字节的指针，否则返回 nullptr
    // ------------------------------------------------------------------------
    static const uint8_t* find_start_code(const uint8_t* p, const uint8_t* end, size_t& out_code_len) {
        // 至少需要 3 个字节才能匹配 0x000001
        while (p + 2 < end) {
            // 检查连续两个 0x00
            if (p[0] == 0x00 && p[1] == 0x00) {
                // 情况1：3字节起始码 0x00 00 01
                if (p[2] == 0x01) {
                    out_code_len = 3;
                    return p;
                }
                // 情况2：4字节起始码 0x00 00 00 01（必须确保有第4个字节）
                if (p + 3 < end && p[2] == 0x00 && p[3] == 0x01) {
                    out_code_len = 4;
                    return p;
                }
            }
            ++p; // 逐字节移动窗口
        }
        return nullptr; // 未找到任何起始码
    }

    // ------------------------------------------------------------------------
    // 静态方法：解析整个内存块中的所有 NALU（零拷贝）
    // 参数：
    //   data - 内存块首地址
    //   size - 内存块字节数
    // 返回值：
    //   包含所有 NALU 描述信息的 vector
    // 算法：
    //   1. 用 find_start_code 找到第一个起始码
    //   2. 再向后查找下一个起始码，两者之间的数据即为一个完整 NALU
    //   3. 解析 NAL 头（1字节）获取类型、重要性等
    //   4. 记录载荷起始位置和大小（跳过起始码和头）
    //   5. 重复直到末尾
    // ------------------------------------------------------------------------
    static std::vector<NaluDescriptor> parse_stream(const uint8_t* data, size_t size) {
        std::vector<NaluDescriptor> nalu_list;
        if (!data || size < 4) return nalu_list; // 至少需要4字节才能有完整起始码

        const uint8_t* ptr = data;              // 当前扫描指针
        const uint8_t* end = data + size;       // 末尾哨兵
        size_t current_code_len = 0;            // 当前找到的起始码长度

        // 查找第一个起始码
        const uint8_t* current_nalu = find_start_code(ptr, end, current_code_len);

        // 循环处理所有找到的 NALU
        while (current_nalu && current_nalu < end) {
            // 从当前起始码之后开始查找下一个起始码
            const uint8_t* next_search_pos = current_nalu + current_code_len;
            size_t next_code_len = 0;
            const uint8_t* next_nalu = find_start_code(next_search_pos, end, next_code_len);

            // 确定当前 NALU 的结束位置：如果有下一个起始码，则结束于下一个起始码之前；
            // 否则结束于整个数据块的末尾
            const uint8_t* nalu_end = next_nalu ? next_nalu : end;

            // 计算当前 NALU 的总长度（包含起始码）
            size_t nalu_total_len = nalu_end - current_nalu;

            // 确保总长度大于起始码长度，否则数据无效
            if (nalu_total_len > current_code_len) {
                NaluDescriptor desc;
                desc.nalu_start = current_nalu;
                desc.total_size = nalu_total_len;
                desc.start_code_len = current_code_len;

                // NAL 头占 1 字节，位于起始码之后
                uint8_t header = *(current_nalu + current_code_len);
                // 解析 NAL 头字段：
                //   bit7: forbidden_zero_bit
                //   bit6-5: nal_ref_idc
                //   bit4-0: nal_unit_type
                desc.forbidden_zero_bit = (header >> 7) & 0x01;
                desc.nal_ref_idc = (header >> 5) & 0x03;
                desc.nal_unit_type = static_cast<NalUnitType>(header & 0x1F);

                // 载荷数据：跳过起始码和 1 字节 NAL 头
                desc.payload = current_nalu + current_code_len + 1;
                // 载荷大小 = 总大小 - 起始码长度 - 1字节头
                desc.payload_size = nalu_total_len - current_code_len - 1;

                nalu_list.push_back(desc);
            }

            // 移动指针到下一个 NALU 的起始位置，继续循环
            current_nalu = next_nalu;
            current_code_len = next_code_len;
        }

        return nalu_list;
    }
};

// ----------------------------------------------------------------------------
// 主函数：演示解析功能
// ----------------------------------------------------------------------------
int main() {
    // 模拟一段 H.264 Annex-B 裸流，包含三个典型 NALU：
    //   - SPS (类型 7)  起始码 0x00000001 (4字节)
    //   - PPS (类型 8)  起始码 0x00000001 (4字节)
    //   - IDR (类型 5)  起始码 0x000001   (3字节)
    // 每个 NALU 的头部（1字节）已被精心构造：
    //   0x67 = 0b0110 0111 => forbidden=0, ref_idc=3, type=7
    //   0x68 = 0b0110 1000 => forbidden=0, ref_idc=3, type=8
    //   0x65 = 0b0110 0101 => forbidden=0, ref_idc=3, type=5
    const std::vector<uint8_t> h264_stream = {
        0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1E, 0xAB,       // SPS
        0x00, 0x00, 0x00, 0x01, 0x68, 0xCE, 0x3C, 0x80,             // PPS
        0x00, 0x00, 0x01,       0x65, 0x88, 0x84, 0x00, 0x10, 0xFF  // IDR Slice
    };

    // 调用解析器，获取所有 NALU 的描述信息
    auto nalus = AnnexbParser::parse_stream(h264_stream.data(), h264_stream.size());

    // 打印每个 NALU 的关键信息
    for (size_t i = 0; i < nalus.size(); ++i) {
        const auto& n = nalus[i];
        std::cout << "NALU [" << i << "]: "
            << "Type=" << static_cast<int>(n.nal_unit_type) << " | "
            << "RefIdc=" << static_cast<int>(n.nal_ref_idc) << " | "
            << "StartCodeLen=" << n.start_code_len << " | "
            << "TotalSize=" << n.total_size << " Bytes | "
            << "PayloadSize=" << n.payload_size << " Bytes\n";
    }

    return 0;
}