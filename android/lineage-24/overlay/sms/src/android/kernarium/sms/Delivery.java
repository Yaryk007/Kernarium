// SPDX-License-Identifier: Apache-2.0
package android.kernarium.sms;

import android.Manifest;
import android.app.ActivityManager;
import android.app.AppOpsManager;
import android.app.role.RoleManager;
import android.content.ContentValues;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.UserHandle;
import android.provider.Telephony;
import android.util.Log;
import java.security.SecureRandom;
import java.util.ArrayList;
import java.util.List;
import java.util.TimeZone;

/**
 * Plans what handing a message to Android takes, and makes the steps: SMS_DELIVER to the default SMS
 * application, which stores it and notifies, then SMS_RECEIVED to the applications that may receive SMS.
 * With no SMS application the message is stored in the inbox here, when an SMS provider is installed; with
 * neither, SMS_RECEIVED alone is all there is to send. The plan is made before anything acts, so that a
 * message that cannot be handed over is refused, not left uncertain. The broadcasts are sent, not
 * awaited: what the service reports is that Android took the message, not that an application has read
 * or stored it.
 */
final class Delivery implements DeliveryRecord.Planner {
    private static final String TAG = "AndariumSms";
    private static final String MESSAGING = "com.android.messaging";
    private static final String SMS_AUTHORITY = "sms";
    private static final SecureRandom RANDOM = new SecureRandom();

    private final Context context;

    Delivery(Context context) {
        this.context = context;
    }

    @Override
    public DeliveryRecord.Plan plan(Message message) {
        final long now = System.currentTimeMillis();
        final byte[][] pdus = DeliverPdu.encode(
                message.sender, message.body, now, TimeZone.getDefault(), RANDOM.nextInt(256));
        final UserHandle user = UserHandle.of(ActivityManager.getCurrentUser());
        final String application = smsApplication(user);
        final List<DeliveryRecord.Step> steps = new ArrayList<>();
        if (application != null) {
            steps.add(() -> {
                final Intent deliver = intent(Telephony.Sms.Intents.SMS_DELIVER_ACTION, pdus);
                deliver.setPackage(application);
                context.sendBroadcastAsUser(deliver, user, Manifest.permission.RECEIVE_SMS,
                        AppOpsManager.OP_RECEIVE_SMS);
            });
        } else if (hasSmsProvider(user)) {
            steps.add(() -> store(user, message, now));
        }
        steps.add(() -> context.sendBroadcastAsUser(intent(Telephony.Sms.Intents.SMS_RECEIVED_ACTION, pdus), user,
                Manifest.permission.RECEIVE_SMS, AppOpsManager.OP_RECEIVE_SMS));
        Log.i(TAG, "planned " + steps.size() + " step(s) for a message of " + pdus.length + " part(s), to "
                + (application != null ? application : steps.size() > 1 ? "the inbox" : "receivers only"));
        return new DeliveryRecord.Plan(pdus.length, steps);
    }

    private static Intent intent(String action, byte[][] pdus) {
        final Intent intent = new Intent(action);
        intent.putExtra("pdus", pdus);
        intent.putExtra("format", "3gpp");
        intent.addFlags(Intent.FLAG_RECEIVER_FOREGROUND);
        return intent;
    }

    /** The SMS role holder, else Messaging when installed, else none. */
    private String smsApplication(UserHandle user) {
        final RoleManager roles = context.getSystemService(RoleManager.class);
        if (roles != null) {
            final List<String> holders = roles.getRoleHoldersAsUser(RoleManager.ROLE_SMS, user);
            if (!holders.isEmpty()) return holders.get(0);
        }
        try {
            context.createContextAsUser(user, 0).getPackageManager()
                    .getApplicationInfo(MESSAGING, PackageManager.ApplicationInfoFlags.of(0));
            return MESSAGING;
        } catch (PackageManager.NameNotFoundException e) {
            return null;
        }
    }

    /** Whether an SMS provider is installed: a package query that starts no process. */
    private boolean hasSmsProvider(UserHandle user) {
        return context.createContextAsUser(user, 0).getPackageManager()
                .resolveContentProvider(SMS_AUTHORITY, PackageManager.ComponentInfoFlags.of(0)) != null;
    }

    private void store(UserHandle user, Message message, long now) {
        final ContentValues values = new ContentValues();
        values.put(Telephony.Sms.ADDRESS, message.sender);
        values.put(Telephony.Sms.BODY, message.body);
        values.put(Telephony.Sms.DATE, now);
        values.put(Telephony.Sms.DATE_SENT, now);
        values.put(Telephony.Sms.READ, 0);
        values.put(Telephony.Sms.SEEN, 0);
        final Uri row = context.createContextAsUser(user, 0).getContentResolver()
                .insert(Telephony.Sms.Inbox.CONTENT_URI, values);
        if (row == null) throw new IllegalStateException("the inbox gave no row");
    }
}
