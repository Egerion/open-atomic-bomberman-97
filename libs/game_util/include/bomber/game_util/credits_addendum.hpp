#pragma once

#include <iterator>
#include <string_view>

#include "bomber/assets/bmtext.hpp"

// THE PORT'S OWN CREDITS ADDENDUM — appended to the ORIGINAL CREDITS.BM when
// the .BM viewer opens it, never written to disk.
//
// CREDITS.BM belongs to the user's install: it is 1997 game data, it is never
// committed, and writing into someone's installation directory to add our own
// credits would be indefensible. So the addendum is synthesized here and pushed
// onto the parsed BmDocument in BmScreen::enter — the file on disk is opened
// read-only and left byte-identical.
//
// It is appended to the SAME document rather than given a page of its own for
// two reasons: the viewer's scroll, paging, clipping and inline-image blit
// already work and a second screen would duplicate all of it (plus invent a key
// binding sub_41302D does not have); and a reader who reaches the end of the
// original credits is exactly the reader this belongs in front of.
//
// The text is authored in `.BM` SYNTAX and run through the same
// bmtext::parse the install's own files go through, so it obeys the same tab
// rule, the same `<IMGname>` markup and the same line model — there is no second
// layout path to keep in step. It uses literal spaces rather than tabs, so the
// drifting tab stop (bmtext::expand_tabs) does not enter into it.
//
// LAYOUT BUDGET (measured against the install's FONT6.FON — 16 px cell, 12 px
// space, proportional glyphs): 65 rows, the widest measuring 349 px of the
// viewer's 532 px per-line clip, none longer than 38 characters. The headless
// test bounds the character count, which is the install-free proxy for that
// measurement; the pixel figures come from the real font and are re-checkable
// by rendering the page (`--bm-shot CREDITS <out.bmp> <scroll>`).
//
// The photograph is 100x100 and, like every other inline image, is CENTRED on
// its row (bmscreen.cpp) — so it reaches three rows above and below its own.
// The blank rows around it are what keep the heading and the following
// paragraph out from under it; the test pins that clearance too. Its column,
// 293 px from the text inset, is the one the original's own QALOGO sits in, so
// the addendum reads as part of the same page. The widest row that shares the
// photograph's band is the email address at 211 px, i.e. 82 px clear of it.

namespace bomber::game {

// The reserved `<IMG...>` base name that resolves to the compiled-in photograph
// instead of a DATA/RES lookup. Chosen so it cannot collide with a name in a
// shipped or user-authored `.BM` (all of those are plain asset base names).
inline constexpr std::string_view kAuthorPhotoTag = "OPENBM_AUTHOR";

// The addendum in `.BM` source form. Anything the owner wants changed is
// changed HERE and nowhere else.
inline constexpr std::string_view kCreditsAddendumBm =
    "\n"
    "\n"
    "------------------------------------\n"
    "\n"
    "OPEN BOMBERMAN\n"
    "A clean-room rewrite of the 1997 game\n"
    "\n"
    "\n"
    "\n"
    "\n"
    "\n"
    "Directed by                 <IMGOPENBM_AUTHOR>\n"
    "   Ege Demirbas\n"
    "   egedemirbas@gmail.com\n"
    "\n"
    "\n"
    "\n"
    "\n"
    "How it was made playable again\n"
    "\n"
    "   Every line of the code was\n"
    "   written by Claude, from\n"
    "   natural-language direction.\n"
    "   No source was typed by hand.\n"
    "\n"
    "   A modern C++20 rewrite, checked\n"
    "   against the 1997 binary rather\n"
    "   than guessed at.  Every mechanic\n"
    "   is first distilled into a\n"
    "   documented fact carrying the\n"
    "   address that proves it, then\n"
    "   ported to mirror the original's\n"
    "   arithmetic, then pinned by tests.\n"
    "\n"
    "   The gameplay core is\n"
    "   deterministic:  integer maths\n"
    "   only, twenty ticks a second, no\n"
    "   wall clock, no I/O.  Golden state\n"
    "   hashes pin whole scenarios, so\n"
    "   behaviour cannot drift quietly.\n"
    "\n"
    "   One number is measured, not\n"
    "   extracted.  The original free-runs\n"
    "   its per-frame mechanics at display\n"
    "   rate, so there is no rate to read\n"
    "   out of the binary.  Nine sub-frames\n"
    "   per tick comes from counting them:\n"
    "   about 184 gameplay callbacks a\n"
    "   second on the reference machine.\n"
    "\n"
    "   BM95.EXE itself was made to run on\n"
    "   Windows 11 first.  Without a live\n"
    "   original to compare against, most\n"
    "   of this would have been guesswork.\n"
    "\n"
    "   And the online play the 1997 game\n"
    "   never shipped in a form that\n"
    "   survives a modern NAT:  lobby\n"
    "   codes, a direct link where the\n"
    "   routers allow one, a relay where\n"
    "   they do not.\n"
    "\n"
    "\n"
    "------------------------------------";

// Parsed form. Cheap enough to build per screen entry (60-odd short lines).
inline assets::bmtext::BmDocument credits_addendum() {
    return assets::bmtext::parse(kCreditsAddendumBm);
}

// Appends the addendum's rows to `doc`. Called by BmScreen::enter for the
// CREDITS document only.
inline void append_credits_addendum(assets::bmtext::BmDocument& doc) {
    assets::bmtext::BmDocument add = credits_addendum();
    doc.lines.insert(doc.lines.end(), std::make_move_iterator(add.lines.begin()),
                     std::make_move_iterator(add.lines.end()));
}

}  // namespace bomber::game
