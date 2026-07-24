#pragma once
#include "Find.h"
#include "Analyze.h"

namespace lengjing::game::native::coordinate_pool_internal {
namespace coord_dec {

    enum class FindDecFailureStage : uint8_t {
        None = 0,
        EntryMethod = 1,
        V87Marker = 2,
        HashSearch = 3,
        PoolPointer = 4,
        RingOffset = 5,
        IndexExpression = 6,
        Patch = 7,
    };

    enum class FindDecFailureDetail : uint8_t {
        None = 0,
        MaddScanEmpty = 1,
        RingStrideMissing = 2,
        RingStrideCount = 3,
        PoolLoadMissing = 4,
        DecodeExpressionParse = 5,
        IndexExpressionParse = 6,
        CandidateExpressionMissing = 7,
        CandidateCount = 8,
        CandidateDependencyAmbiguous = 9,
        IndexParameterMissing = 10,
    };

    struct param {
        std::string name;
        uint32_t size;
        int32_t disp;
        uint64_t value;
        std::vector<int32_t> offset;
    };

    class FindDec : public Find {

        uint32_t ring_offset = 0;
        uint32_t v87_str_point = 0;
        uint32_t ring_offset_madd_block = 0;
        uint32_t hash_end_madd = 0;

        method* entry = nullptr;

        std::string ring_index_param;

        std::unordered_map<std::string, uint64_t> params;
        FindDecFailureStage failure_stage_ = FindDecFailureStage::None;
        FindDecFailureDetail failure_detail_ = FindDecFailureDetail::None;
        uint16_t madd_count_ = 0;
        uint16_t ring_madd_count_ = 0;
        uint16_t candidate_count_ = 0;
        uint16_t failure_instruction_ = 0;

        bool find_v87_str();
        bool analyze_base_index_calc();
        bool find_pool_ptr_offset();
        bool find_binary_search_end();

        bool analyze_index_calc();

        bool analyze_hash_binary_search();

        bool find_ring_offset();
        bool patch();
    public:
        Analyze analyze;
        std::shared_ptr<Expr> index_expr;
        std::vector<param> mem_param_list;
        int64_t index_offset = 0;
        int32_t pool_ptr_offset = 0;

        uint32_t get_ring_offset() {
            return ring_offset;
        }

        int find_dec(uint64_t entry_address);

        FindDecFailureStage failure_stage() const noexcept {
            return failure_stage_;
        }

        FindDecFailureDetail failure_detail() const noexcept {
            return failure_detail_;
        }

        uint16_t madd_count() const noexcept {
            return madd_count_;
        }

        uint16_t ring_madd_count() const noexcept {
            return ring_madd_count_;
        }

        uint16_t candidate_count() const noexcept {
            return candidate_count_;
        }

        uint16_t failure_instruction() const noexcept {
            return failure_instruction_;
        }

        void compact_runtime_plan() noexcept {
            entry = nullptr;
            analyze.release_analysis_storage();
            binary_.release_analysis_storage();
        }

        void setup_param() {
            for (const auto &p: mem_param_list) {
                params[p.name] = p.value;
            }

            for (const auto &p :analyze.varParams) {
                params[p.name] = p.value;
            }
        }

        uint64_t decode_ring_slot(uint64_t index) {
            params[ring_index_param] = index;
            return index_expr->eval(params);
        }
    };
}
}  // namespace lengjing::game::native::coordinate_pool_internal
