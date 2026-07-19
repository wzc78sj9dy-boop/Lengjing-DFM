#pragma once
#include "Find.h"
#include "Analyze.h"

namespace lengjing::game::native::coordinate_pool_internal {
namespace coord_dec {
   
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
