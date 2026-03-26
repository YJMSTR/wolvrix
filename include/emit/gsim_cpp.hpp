#ifndef WOLVRIX_EMIT_GSIM_CPP_HPP
#define WOLVRIX_EMIT_GSIM_CPP_HPP

#include "core/emit.hpp"

namespace wolvrix::lib::emit
{

    class EmitGsimCpp : public Emit
    {
    public:
        using Emit::Emit;

    private:
        EmitResult emitImpl(const wolvrix::lib::grh::Design &design,
                            std::span<const wolvrix::lib::grh::Graph *const> topGraphs,
                            const EmitOptions &options) override;
    };

} // namespace wolvrix::lib::emit

#endif // WOLVRIX_EMIT_GSIM_CPP_HPP
