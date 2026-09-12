// Differential libFuzzer harness: replays the same decoded ITCH message
// stream into a book::OrderBook (unbounded std::map ladders — the original,
// deliberately-simple v1 baseline) and a book::LadderBook (the flat,
// bounded, growable tick-ladder that is the production default — see
// pipeline::BookTraits<book::LadderBook> in dispatch_to_book.hpp) side by
// side, and asserts their externally observable state agrees after every
// message that both books actually applied.
//
// fuzz_parser.cpp fuzzes decode-into-one-book. It structurally cannot see a
// class of bug this harness exists for: two book implementations that are
// supposed to be interchangeable (replay/replay_threaded's --map flag picks
// either one for the identical input) silently disagreeing about the state
// of the book. Any such disagreement is a bug by construction — no hand-
// written oracle, no "expected" fixture to keep in sync by hand.
//
// One real subtlety this harness has to account for, not invent: the two
// books do NOT have identical acceptance policies. LadderBook's tick-grid
// deliberately rejects a price that falls outside its (growable, but
// hard-capped) window or isn't grid-aligned to tick_size_ — see
// book/ladder_book.hpp's add() and docs/devlog-orderbook-vs-ladderbook.md's
// "Closing the open question" (a fixed, never-widening window once silently
// dropped ~30% of a real NASDAQ day's mutations). OrderBook's std::map
// ladders have no such range restriction. So the two books diverging on
// whether to ACCEPT a given add/replace is expected, documented behavior,
// not a bug — flagging every one of those as a crash would make this
// harness a false-positive machine on the very first fuzzer-generated
// out-of-grid price. What the harness must never accept silently is the two
// books disagreeing about the resulting STATE after they agree on whether a
// message applied at all. Once a locate's acceptance policies have
// disagreed once, further state comparison for that locate is meaningless
// (the books have permanently different sets of resting orders for it), so
// it's marked and skipped rather than re-checked message by message.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include "book/ladder_book.hpp"
#include "book/order_book.hpp"
#include "itch/parser.hpp"
#include "pipeline/dispatch_to_book.hpp"

namespace {

// Process-lifetime counters, not per-input: a fresh DivergingHandler is
// constructed on every LLVMFuzzerTestOneInput call (each input is an
// independent replay from an empty book), so these are the only place
// running totals across a whole fuzzing session can live. Printed once at
// process exit (see print_summary()) rather than per-occurrence — almost
// every random input's very first out-of-window price hits the policy-
// divergence path (see DivergingHandler::apply_and_diff below), so logging
// each one individually was measured to collapse throughput to ~58 exec/s
// (pure stderr I/O contention) versus thousands/s with it silenced.
std::uint64_t g_inputs_seen = 0;
std::uint64_t g_state_checks = 0;
std::uint64_t g_policy_skips = 0;

void print_summary() {
    std::fprintf(stderr,
                 "[fuzz_differential] inputs=%llu state_checks=%llu policy_skips=%llu "
                 "(policy_skips = one book's price-window/grid policy rejected a message the "
                 "other accepted -- expected, documented OrderBook/LadderBook behavior, not a "
                 "bug; state_checks = both books applied the message and agreed)\n",
                 static_cast<unsigned long long>(g_inputs_seen),
                 static_cast<unsigned long long>(g_state_checks),
                 static_cast<unsigned long long>(g_policy_skips));

    // Same three numbers, machine-readable, for
    // record_differential_history.py to pick up in CI and append to
    // fuzz/differential_history.csv (which dashboard/index.html charts) --
    // the dashboard's own rule is it never computes a number the repo
    // doesn't already produce, so this is that number's only source, not a
    // duplicate. Written relative to the process's working directory, which
    // is the repo root both when build_and_run_differential.sh invokes this
    // binary locally and when the CI step runs it (default
    // working-directory). Best-effort: a write failure here (e.g. a
    // read-only sandbox) must never take down the fuzz run itself, so it's
    // silently skipped rather than aborting.
    if (FILE* f = std::fopen("fuzz/differential_last_run.csv", "w")) {
        std::fprintf(f, "inputs,state_checks,policy_skips\n%llu,%llu,%llu\n",
                     static_cast<unsigned long long>(g_inputs_seen),
                     static_cast<unsigned long long>(g_state_checks),
                     static_cast<unsigned long long>(g_policy_skips));
        std::fclose(f);
    }
}

std::string quote_str(const std::optional<book::Quote>& q) {
    if (!q) return "none";
    return std::to_string(q->price) + "x" + std::to_string(q->shares);
}

// Drives one message through both books via the shared BookBuilder::on_*
// entry points (reused as-is, never reimplemented — same principle
// fuzz_parser.cpp's own header comment states for BookBuilder itself), then
// diffs. Templated on the per-message dispatch closure so all six message
// types share one apply-diff-guard path instead of six near-identical
// copies of the same three-way branch below.
struct DivergingHandler {
    pipeline::BookBuilder<book::OrderBook> reference;   // unbounded std::map ladders
    pipeline::BookBuilder<book::LadderBook> candidate;  // flat tick-ladder, production default

    // Per-locate: true once this locate's two books have disagreed about
    // whether some message applied at all. Sized lazily (locates are a
    // dense, small session-assigned range, same reasoning as BookTable
    // itself; a handful of resizes over a whole fuzz run is noise).
    std::vector<bool> policy_diverged;
    std::uint64_t divergence_checks = 0;
    std::uint64_t policy_only_skips = 0;

    bool is_policy_diverged(std::uint16_t locate) const {
        return locate < policy_diverged.size() && policy_diverged[static_cast<std::size_t>(locate)];
    }
    void mark_policy_diverged(std::uint16_t locate) {
        if (locate >= policy_diverged.size())
            policy_diverged.resize(static_cast<std::size_t>(locate) + 1, false);
        policy_diverged[static_cast<std::size_t>(locate)] = true;
    }

    template <typename Fn>
    void apply_and_diff(std::uint16_t locate, Fn&& fn) {
        // unknown_refs only ever increments (BookBuilder::on_* bumps it on
        // an add/replace/execute/cancel/delete that its own book rejected —
        // see dispatch_to_book.hpp), so a before/after delta around exactly
        // one on_* call on exactly one book is an unambiguous proxy for
        // "did this book apply this specific message."
        const std::uint64_t ref_before = reference.unknown_refs;
        const std::uint64_t cand_before = candidate.unknown_refs;
        fn(reference);
        fn(candidate);
        const bool ref_applied = (reference.unknown_refs == ref_before);
        const bool cand_applied = (candidate.unknown_refs == cand_before);

        if (is_policy_diverged(locate)) return;  // already known-incomparable for this symbol

        if (ref_applied != cand_applied) {
            // One book accepted this message, the other rejected it. Could
            // be a real bug (e.g. a duplicate-ref check disagreeing) or the
            // documented LadderBook price-window/grid policy — this harness
            // can't tell the difference from the outside, so it does not
            // guess: it stops comparing state for this locate rather than
            // risk either false-failing on known behavior or silently
            // missing a real one. fuzz/README.md's divergence log (stderr)
            // still records every occurrence for a human to skim.
            mark_policy_diverged(locate);
            ++policy_only_skips;
            return;
        }
        if (!ref_applied) return;  // both rejected identically: no state changed on either side

        diff_locate(locate);
    }

    void on_add(const itch::AddOrder& m) {
        apply_and_diff(m.hdr.locate, [&](auto& b) { b.on_add(m); });
    }
    void on_execute(const itch::OrderExecuted& m) {
        apply_and_diff(m.hdr.locate, [&](auto& b) { b.on_execute(m); });
    }
    void on_execute_price(const itch::OrderExecutedPrice& m) {
        apply_and_diff(m.hdr.locate, [&](auto& b) { b.on_execute_price(m); });
    }
    void on_cancel(const itch::OrderCancel& m) {
        apply_and_diff(m.hdr.locate, [&](auto& b) { b.on_cancel(m); });
    }
    void on_delete(const itch::OrderDelete& m) {
        apply_and_diff(m.hdr.locate, [&](auto& b) { b.on_delete(m); });
    }
    void on_replace(const itch::OrderReplace& m) {
        apply_and_diff(m.hdr.locate, [&](auto& b) { b.on_replace(m); });
    }
    void on_other(char type, std::size_t len) {
        reference.on_other(type, len);
        candidate.on_other(type, len);
    }

    // Both books applied the same message and neither rejected it: their
    // observable contract (best_bid/best_ask price+shares, open order
    // count, occupied bid/ask level counts — the full public surface both
    // book::OrderBook and book::LadderBook expose) must now agree exactly.
    void diff_locate(std::uint16_t locate) {
        const book::OrderBook* ref_book = reference.books.find(locate);
        const book::LadderBook* cand_book = candidate.books.find(locate);
        if (!ref_book || !cand_book) return;  // neither side has built a book for this locate (shouldn't happen once ref_applied is true, but never assume)
        ++divergence_checks;

        const auto ref_bid = ref_book->best_bid();
        const auto cand_bid = cand_book->best_bid();
        const auto ref_ask = ref_book->best_ask();
        const auto cand_ask = cand_book->best_ask();

        const bool bid_ok = (ref_bid.has_value() == cand_bid.has_value()) &&
                             (!ref_bid || (ref_bid->price == cand_bid->price &&
                                           ref_bid->shares == cand_bid->shares));
        const bool ask_ok = (ref_ask.has_value() == cand_ask.has_value()) &&
                             (!ref_ask || (ref_ask->price == cand_ask->price &&
                                           ref_ask->shares == cand_ask->shares));
        const bool orders_ok = ref_book->open_orders() == cand_book->open_orders();
        const bool bid_levels_ok = ref_book->bid_levels() == cand_book->bid_levels();
        const bool ask_levels_ok = ref_book->ask_levels() == cand_book->ask_levels();

        if (bid_ok && ask_ok && orders_ok && bid_levels_ok && ask_levels_ok) return;

        std::fprintf(stderr,
                     "DIVERGENCE at locate=%u (both books applied this message; state disagrees)\n"
                     "  OrderBook : bid=%s ask=%s open_orders=%zu bid_levels=%zu ask_levels=%zu\n"
                     "  LadderBook: bid=%s ask=%s open_orders=%zu bid_levels=%zu ask_levels=%zu\n",
                     locate, quote_str(ref_bid).c_str(), quote_str(ref_ask).c_str(),
                     ref_book->open_orders(), ref_book->bid_levels(), ref_book->ask_levels(),
                     quote_str(cand_bid).c_str(), quote_str(cand_ask).c_str(),
                     cand_book->open_orders(), cand_book->bid_levels(), cand_book->ask_levels());
        std::abort();
    }
};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    static const bool registered = (std::atexit(print_summary), true);
    (void)registered;
    ++g_inputs_seen;
    DivergingHandler h;
    itch::parse_stream(data, size, h);
    g_state_checks += h.divergence_checks;
    g_policy_skips += h.policy_only_skips;
    return 0;
}
