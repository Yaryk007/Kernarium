// SPDX-License-Identifier: Apache-2.0
package org.example.andarium.smsprobe;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.provider.Telephony;
import android.telephony.SmsMessage;
import android.util.Base64;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;

/**
 * Records each SMS_RECEIVED broadcast it is handed, one line a message in files/received.txt:
 * {@code <sender base64> <text base64>}, the text of all parts joined, so that a line break in either cannot
 * break the form. The runner reads the file with run-as.
 */
public final class ReceivedReceiver extends BroadcastReceiver {
    static final String FILE = "received.txt";

    @Override
    public void onReceive(Context context, Intent intent) {
        final SmsMessage[] parts = Telephony.Sms.Intents.getMessagesFromIntent(intent);
        if (parts == null || parts.length == 0) return;
        final StringBuilder text = new StringBuilder();
        for (SmsMessage part : parts) {
            final String body = part.getMessageBody();
            if (body != null) text.append(body);
        }
        final String sender = parts[0].getOriginatingAddress();
        final String line = encode(sender == null ? "" : sender) + " " + encode(text.toString()) + "\n";
        try (FileOutputStream out = context.openFileOutput(FILE, Context.MODE_APPEND)) {
            out.write(line.getBytes(StandardCharsets.US_ASCII));
        } catch (IOException e) {
            // The runner finds the line missing and says so.
        }
    }

    private static String encode(String value) {
        return Base64.encodeToString(value.getBytes(StandardCharsets.UTF_8), Base64.NO_WRAP);
    }
}
