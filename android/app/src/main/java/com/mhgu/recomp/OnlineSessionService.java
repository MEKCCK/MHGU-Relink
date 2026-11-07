package com.mhgu.recomp;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.net.wifi.WifiManager;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.PowerManager;

public final class OnlineSessionService extends Service {
    private static final String CHANNEL_ID = "online_session";
    private static final int NOTIFICATION_ID = 1;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private PowerManager.WakeLock cpuLock;
    private WifiManager.WifiLock wifiLock;

    private final Runnable checkSession = new Runnable() {
        @Override
        public void run() {
            if (!GameActivity.isMultiplayerActive()) {
                stopSelf();
                return;
            }
            handler.postDelayed(this, 3000);
        }
    };

    @Override
    public void onCreate() {
        super.onCreate();
        NotificationManager notifications = getSystemService(NotificationManager.class);
        notifications.createNotificationChannel(new NotificationChannel(
                CHANNEL_ID,
                getString(R.string.online_session_channel),
                NotificationManager.IMPORTANCE_LOW));

        PowerManager power = (PowerManager) getSystemService(Context.POWER_SERVICE);
        cpuLock = power.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "MHGU:OnlineSession");
        cpuLock.setReferenceCounted(false);
        WifiManager wifi = (WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);
        if (wifi != null) {
            wifiLock = wifi.createWifiLock(WifiManager.WIFI_MODE_FULL_HIGH_PERF, "MHGU:OnlineSession");
            wifiLock.setReferenceCounted(false);
        }
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        Notification notification = new Notification.Builder(this, CHANNEL_ID)
                .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
                .setContentTitle(getString(R.string.online_session_title))
                .setContentText(getString(R.string.online_session_text))
                .setOngoing(true)
                .build();
        startForeground(NOTIFICATION_ID, notification);
        if (!cpuLock.isHeld()) cpuLock.acquire();
        if (wifiLock != null && !wifiLock.isHeld()) wifiLock.acquire();
        handler.removeCallbacks(checkSession);
        handler.post(checkSession);
        return START_NOT_STICKY;
    }

    @Override
    public void onDestroy() {
        handler.removeCallbacks(checkSession);
        if (cpuLock != null && cpuLock.isHeld()) cpuLock.release();
        if (wifiLock != null && wifiLock.isHeld()) wifiLock.release();
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
