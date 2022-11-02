/**
 * This file is part of the "libunicode" project
 *   Copyright (c) 2020 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <unicode/grapheme_segmenter.h>
#include <unicode/scan.h>
#include <unicode/utf8.h>
#include <unicode/width.h>

#include <fmt/format.h>

#include <algorithm>
#include <cassert>
#include <iterator>
#include <numeric>
#include <string_view>

#if defined(__SSE2__)
    #include <immintrin.h>
#endif

using std::distance;
using std::get;
using std::holds_alternative;
using std::max;
using std::min;
using std::string_view;

namespace unicode
{

namespace
{
    int countTrailingZeroBits(unsigned int value) noexcept
    {
#if defined(_WIN32)
        return _tzcnt_u32(value);
#else
        return __builtin_ctz(value);
#endif
    }

    template <typename T>
    constexpr bool ascending(T low, T val, T high) noexcept
    {
        return low <= val && val <= high;
    }

    constexpr bool is_control(char ch) noexcept
    {
        return static_cast<uint8_t>(ch) < 0x20;
    }

    // Tests if given UTF-8 byte is part of a complex Unicode codepoint, that is, a value greater than U+7E.
    constexpr bool is_complex(char ch) noexcept
    {
        return static_cast<uint8_t>(ch) & 0x80;
    }

    // Tests if given UTF-8 byte is a single US-ASCII text codepoint. This excludes control characters.
    constexpr bool is_ascii(char ch) noexcept
    {
        return !is_control(ch) && !is_complex(ch);
    }
} // namespace

size_t detail::scan_for_text_ascii(string_view text, size_t maxColumnCount) noexcept
{
    auto input = text.data();
    auto const end = text.data() + min(text.size(), maxColumnCount);

#if defined(__SSE2__)
    __m128i const ControlCodeMax = _mm_set1_epi8(0x20); // 0..0x1F
    __m128i const Complex = _mm_set1_epi8(static_cast<char>(0x80));

    while (input < end - sizeof(__m128i))
    {
        __m128i batch = _mm_loadu_si128((__m128i*) input);
        __m128i isControl = _mm_cmplt_epi8(batch, ControlCodeMax);
        __m128i isComplex = _mm_and_si128(batch, Complex);
        //__m128i isComplex = _mm_cmplt_epi8(batch, Complex);
        __m128i testPack = _mm_or_si128(isControl, isComplex);
        if (int const check = _mm_movemask_epi8(testPack); check != 0)
        {
            int advance = countTrailingZeroBits(static_cast<unsigned>(check));
            input += advance;
            break;
        }
        input += sizeof(__m128i);
    }
#endif

    while (input != end && is_ascii(*input))
        ++input;

    // if (static_cast<size_t>(distance(text.data(), input)))
    //     fmt::print(
    //         "countAsciiTextChars: {} bytes: \"{}\"\n",
    //         static_cast<size_t>(distance(text.data(), input)),
    //         (string_view(text.data(), static_cast<size_t>(distance(text.data(), input)))));

    return static_cast<size_t>(distance(text.data(), input));
}

scan_result detail::scan_for_text_nonascii(scan_state& state,
                                           string_view text,
                                           size_t maxColumnCount) noexcept
{
    size_t count = 0;

    char const* start = text.data();
    char const* end = start + text.size();
    char const* input = start;
    char const* clusterStart = start;
    char const* lastCodepointStart = start;

    unsigned byteCount = 0; // bytes consume for the current codepoint

    // TODO: move currentClusterWidth to scan_state.
    size_t currentClusterWidth = 0; // current grapheme cluster's East Asian Width

    char const* resultStart = state.utf8.expectedLength ? start - state.utf8.currentLength : start;
    char const* resultEnd = resultStart;

    while (input != end && count <= maxColumnCount && is_complex(*input))
    {
        auto const result = from_utf8(state.utf8, static_cast<uint8_t>(*input++));
        ++byteCount;

        if (holds_alternative<Incomplete>(result))
            continue;

        if (holds_alternative<Success>(result))
        {
            auto const prevCodepoint = state.lastCodepointHint;
            auto const nextCodepoint = get<Success>(result).value;
            auto const nextWidth = max(currentClusterWidth, static_cast<size_t>(width(nextCodepoint)));
            state.lastCodepointHint = nextCodepoint;
            if (grapheme_segmenter::breakable(prevCodepoint, nextCodepoint))
            {
                // Flush out current grapheme cluster's East Asian Width.
                count += currentClusterWidth;

                if (count + nextWidth > maxColumnCount)
                {
                    // Currently scanned grapheme cluster won't fit. Break at start.
                    currentClusterWidth = 0;
                    input -= byteCount;
                    break;
                }

                // And start a new grapheme cluster.
                currentClusterWidth = nextWidth;
                clusterStart = lastCodepointStart;
                lastCodepointStart = input - byteCount;
                byteCount = 0;
                resultEnd = input;
            }
            else
            {
                resultEnd = input;
                // Increase width on VS16 but do not decrease on VS15.
                if (nextCodepoint == 0xFE0F) // VS16
                {
                    currentClusterWidth = 2;
                    if (count + currentClusterWidth > maxColumnCount)
                    {
                        // Rewinding by {byteCount} bytes (overflow due to VS16).
                        currentClusterWidth = 0;
                        input = clusterStart;
                        break;
                    }
                }

                // Consumed {byteCount} bytes for grapheme cluster.
                lastCodepointStart = input - byteCount;
            }
        }
        else
        {
            assert(holds_alternative<Invalid>(result));
            ++count;
            currentClusterWidth = 0;
            state.lastCodepointHint = 0;
            byteCount = 0;
        }
    }
    count += currentClusterWidth;

    // if (count)
    //     fmt::print("countNonAsciiTextChars: {} codepoints: \"{}\"\n",
    //                count,
    //                string_view(start, size_t(distance(start, input))));

    assert(resultStart <= resultEnd);

    return { count, input, resultStart, resultEnd };
}

scan_result scan_for_text(scan_state& state, string_view text, size_t maxColumnCount) noexcept
{
    //       ----(a)--->   A   -------> END
    //                   ^   |
    //                   |   |
    // Start            (a) (b)
    //                   |   |
    //                   |   v
    //       ----(b)--->   B   -------> END

    enum class NextState
    {
        Trivial,
        Complex
    };

    auto result = scan_result { 0, text.data(), text.data(), text.data() };

    // If state indicates that we previously started consuming a UTF-8 sequence but did not complete yet,
    // attempt to finish that one first.
    if (state.utf8.expectedLength != 0)
    {
        result = detail::scan_for_text_nonascii(state, text, maxColumnCount);
        text = std::string_view(result.end,
                                static_cast<size_t>(std::distance(result.end, text.data() + text.size())));
    }

    auto nextState = is_complex(text.front()) ? NextState::Complex : NextState::Trivial;
    while (result.count < maxColumnCount && result.next != (text.data() + text.size()))
    {
        switch (nextState)
        {
            case NextState::Trivial: {
                auto const count = detail::scan_for_text_ascii(text, maxColumnCount - result.count);
                if (!count)
                    return result;
                result.count += count;
                result.next += count;
                result.end += count;
                nextState = NextState::Complex;
                text.remove_prefix(count);
                break;
            }
            case NextState::Complex: {
                auto const sub = detail::scan_for_text_nonascii(state, text, maxColumnCount - result.count);
                nextState = NextState::Trivial;
                result.count += sub.count;
                result.next = sub.next;
                result.end = sub.end;
                text.remove_prefix(static_cast<size_t>(std::distance(sub.start, sub.end)));
                break;
            }
        }
    }

    assert(result.start <= result.end);
    assert(result.end <= result.next);

    return result;
}

} // namespace unicode