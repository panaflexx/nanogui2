/*
 * pagemade/plugin/plugin.cpp — see plugin.h.
 */
#include "plugin/plugin.h"

#include "vector.h"

namespace pagemade {

void register_vector_plugins(VectorLoader &loader) {
    register_svg_decoder(loader);
    register_pdf_decoder(loader);
}

} // namespace pagemade
