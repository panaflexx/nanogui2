/*
 * pagemade/plugin/plugin.h — the import and export plugins.
 *
 * Every file format pagemade reads or writes lives here, one file per
 * format and direction, and nothing outside this folder knows a format's
 * insides:
 *
 *   svg_import.cpp   SVG -> VectorScene (a VectorDecoder)
 *   svg_place.*      placing a decoded graphic: whole, in pieces, or as text
 *   svg_export.*     items -> SVG and plain text, for Copy
 *   pdf_parse.*      a PDF's objects: cross-reference, streams, filters, pages
 *   pdf_import.cpp   PDF page -> VectorScene (a VectorDecoder)
 *   pdf_export.*     a publication -> PDF (the draw list's second backend)
 *   xml.*            the small XML reader the importers share
 *
 * Importers are decoders registered on a loader: vector formats on the
 * VectorLoader (vector.h), pictures on the ImageLoader (image.h). Place,
 * drag and drop, paste and reopening a publication all go through the
 * loader, which picks a decoder by sniffing the bytes, so a new format
 * needs no change outside this folder beyond its registration here.
 * Decoders are linked in, never loaded from disk.
 */
#pragma once

namespace pagemade {

class VectorLoader;

/* The built-in vector formats: SVG and PDF. vector_loader() calls this once. */
void register_vector_plugins(VectorLoader &loader);

/* Each registers one decoder. */
void register_svg_decoder(VectorLoader &loader);
void register_pdf_decoder(VectorLoader &loader);

} // namespace pagemade
