#ifndef WOLVRIX_TRANSFORM_STRIP_DEBUG_HPP
#define WOLVRIX_TRANSFORM_STRIP_DEBUG_HPP

#include "core/transform.hpp"

#include <string>
#include <vector>

namespace wolvrix::lib::transform
{

    struct StripDebugOptions
    {
        std::string path;
        std::vector<std::string> keepDpicImportPrefixes;
    };

    class StripDebugPass : public Pass
    {
    public:
        StripDebugPass();
        explicit StripDebugPass(StripDebugOptions options);

        PassResult run() override;

    private:
        StripDebugOptions options_;
    };

} // namespace wolvrix::lib::transform

#endif // WOLVRIX_TRANSFORM_STRIP_DEBUG_HPP
