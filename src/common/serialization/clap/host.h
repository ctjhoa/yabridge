// yabridge: a Wine plugin bridge
// Copyright (C) 2020-2022 Robbert van der Helm
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#pragma once

#include <optional>
#include <string>
#include <vector>

#include <bitsery/traits/vector.h>
#include <clap/plugin.h>

#include "../../bitsery/ext/in-place-optional.h"

// Serialization messages for `clap/host.h`

namespace clap {
namespace host {

/**
 * A serializable version of `clap_host_t`'s data fields so we can proxy the
 * host on the Wine side.
 */
struct Host {
    /**
     * Parse a host descriptor so it can be serialized and sent to the Wine
     * plugin host.
     */
    Host(const clap_host_t& original);

    /**
     * Default constructor for bitsery.
     */
    Host() {}

    /**
     * We'll report the maximum of the plugin's supported CLAP version and
     * yabridge's supported CLAP version. I don't know why there's a version
     * field here when the entry point also has a version field.
     */
    clap_version_t clap_version;

    std::string name;
    std::optional<std::string> vendor;
    std::optional<std::string> url;
    std::string version;

    template <typename S>
    void serialize(S& s) {
        s.object(clap_version);

        s.text1b(name, 4096);
        s.ext(vendor, bitsery::ext::InPlaceOptional(),
              [](S& s, auto& v) { s.text1b(v, 4096); });
        s.ext(url, bitsery::ext::InPlaceOptional(),
              [](S& s, auto& v) { s.text1b(v, 4096); });
        s.text1b(version, 4096);
    }
};

}  // namespace host
}  // namespace clap
