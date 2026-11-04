#include "UsmapGen/Layout.h"

namespace UsmapGen {

std::optional<Kind> translateKind(const TypeNumbering numbering, const std::uint32_t raw) {
    constexpr std::uint32_t firstDifferent = std::uint32_t(Kind::InlineMulticastDelegate);
    switch (numbering) {
    case TypeNumbering::Engine418:
    case TypeNumbering::Engine422:
        if (raw < firstDifferent)
            return Kind(raw);
        if (raw == firstDifferent)
            return Kind::MulticastDelegate;
        if (raw == firstDifferent + 1)
            return Kind::Text;
        if (raw == firstDifferent + 2)
            return Kind::Enum;
        return std::nullopt;
    case TypeNumbering::Engine423:
        if (raw > std::uint32_t(Kind::FieldPath))
            return std::nullopt;
        return Kind(raw);
    case TypeNumbering::Engine5:
        if (raw > std::uint32_t(Kind::VerseLegacyValue))
            return std::nullopt;
        return Kind(raw);
    }
    return std::nullopt;
}

}
