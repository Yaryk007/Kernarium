// SPDX-License-Identifier: GPL-2.0-or-later
//
// The golden vectors of the host channel and its payload (tests/unit/andarium/fixtures/sms-golden-frames.txt),
// shared with the host's tests: `name: <fields> => <hex>` or `name: <hex>`, `#` for a comment.

package android.kernarium.sms;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

final class Golden {
    private final Map<String, String> vectors = new LinkedHashMap<>();

    static Golden load() throws IOException {
        final String path = System.getProperty("sms.golden");
        if (path == null) throw new IOException("-Dsms.golden names the golden vectors file");
        final Golden golden = new Golden();
        for (String line : Files.readAllLines(Paths.get(path), StandardCharsets.UTF_8)) {
            if (line.isEmpty() || line.startsWith("#")) continue;
            final int colon = line.indexOf(':');
            if (colon < 0) throw new IOException("not a vector: " + line);
            if (golden.vectors.put(line.substring(0, colon), line.substring(colon + 1).trim()) != null) {
                throw new IOException("a name twice: " + line.substring(0, colon));
            }
        }
        return golden;
    }

    /** The names that start with {@code prefix}, in the file's order. */
    List<String> names(String prefix) {
        final List<String> names = new ArrayList<>();
        for (String name : vectors.keySet()) {
            if (name.startsWith(prefix)) names.add(name);
        }
        return names;
    }

    private String value(String name) {
        final String value = vectors.get(name);
        if (value == null) throw new IllegalArgumentException("no vector " + name);
        return value;
    }

    /** The fields before the arrow, or the whole value when there is no arrow. */
    String[] fields(String name) {
        final String value = value(name);
        final int arrow = value.indexOf("=>");
        final String before = (arrow < 0 ? value : value.substring(0, arrow)).trim();
        return before.isEmpty() ? new String[0] : before.split(" ");
    }

    /** What is after the arrow, or the whole value when there is no arrow. */
    String after(String name) {
        final String value = value(name);
        final int arrow = value.indexOf("=>");
        return (arrow < 0 ? value : value.substring(arrow + 2)).trim();
    }

    static byte[] bytes(String hex) {
        final byte[] bytes = new byte[hex.length() / 2];
        for (int i = 0; i < bytes.length; ++i) bytes[i] = (byte) Integer.parseInt(hex.substring(2 * i, 2 * i + 2), 16);
        return bytes;
    }
}
