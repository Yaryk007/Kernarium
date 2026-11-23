// SPDX-License-Identifier: Apache-2.0
package org.example.andarium.smsprobe;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;
import java.io.FileDescriptor;
import java.io.FileOutputStream;
import java.io.IOException;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;

/**
 * The calls that an application's domain must not be able to make, made from an application's own process (the
 * runner starts the receiver with {@code am broadcast -n}, so the zygote forks it in the application's domain).
 * The extra {@code do} names the call and {@code nonce} comes back in the result file:
 *
 * <ul>
 * <li>{@code vsock}: a vsock socket; files/vsock.txt holds {@code <nonce>} and {@code socket: errno=13 EACCES}
 * or {@code socket: ok};
 * <li>{@code vm}: the platform's virtual machine API, asked to create and start one; files/vm.txt holds
 * {@code <nonce>} and {@code vm: failed at <step>: <exception>} or {@code vm: started}. The API is reached
 * by reflection, since a build of the public API may not list it;
 * <li>{@code reset}: deletes files/received.txt.
 * </ul>
 */
public final class CommandReceiver extends BroadcastReceiver {
    /** Linux's AF_VSOCK; android.system.OsConstants has no public name for it. */
    private static final int AF_VSOCK = 40;

    @Override
    public void onReceive(Context context, Intent intent) {
        final String call = intent.getStringExtra("do");
        final String nonce = intent.getStringExtra("nonce");
        if ("vsock".equals(call)) {
            write(context, "vsock.txt", nonce, socket());
        } else if ("vm".equals(call)) {
            write(context, "vm.txt", nonce, virtualMachine(context));
        } else if ("reset".equals(call)) {
            context.deleteFile(ReceivedReceiver.FILE);
        }
    }

    static String socket() {
        try {
            final FileDescriptor descriptor = Os.socket(AF_VSOCK, OsConstants.SOCK_STREAM, 0);
            Os.close(descriptor);
            return "socket: ok";
        } catch (ErrnoException e) {
            return "socket: errno=" + e.errno + " " + OsConstants.errnoName(e.errno);
        }
    }

    private static String virtualMachine(Context context) {
        String step = "finding the manager";
        try {
            final Class<?> managerClass = Class.forName("android.system.virtualmachine.VirtualMachineManager");
            final Object manager = context.getSystemService(managerClass);
            if (manager == null) return "vm: failed at " + step + ": no service";
            step = "building a configuration";
            final Class<?> builderClass = Class.forName("android.system.virtualmachine.VirtualMachineConfig$Builder");
            final Object builder = builderClass.getConstructor(Context.class).newInstance(context);
            builderClass.getMethod("setProtectedVm", boolean.class).invoke(builder, false);
            builderClass.getMethod("setPayloadBinaryName", String.class).invoke(builder, "libsmsprobe.so");
            final Object config = builderClass.getMethod("build").invoke(builder);
            step = "creating the machine";
            final Method create = manager.getClass().getMethod("getOrCreate", String.class, config.getClass());
            final Object machine = create.invoke(manager, "smsprobe", config);
            step = "running the machine";
            machine.getClass().getMethod("run").invoke(machine);
            return "vm: started";
        } catch (ReflectiveOperationException | RuntimeException e) {
            final Throwable cause = e.getCause() != null ? e.getCause() : e;
            return "vm: failed at " + step + ": " + cause.getClass().getName();
        }
    }

    private static void write(Context context, String name, String nonce, String result) {
        try (FileOutputStream out = context.openFileOutput(name, Context.MODE_PRIVATE)) {
            out.write((nonce + "\n" + result + "\n").getBytes(StandardCharsets.US_ASCII));
        } catch (IOException e) {
            // The runner finds no result and says so.
        }
    }
}
