package com.mhgu.relink;

import android.content.Context;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.util.Log;
import android.view.Surface;

/**
 * El giroscopio del teléfono para el runtime ([Controls] Motion: Off, Aiming, Always). Se registra solo mientras el
 * juego lo pida (GameActivity.setMotionEnabled, desde runtime/ui.c) y la actividad esté en primer plano; al apagarse o
 * al pasar a segundo plano se desregistra y manda un cero, para que no quede ningún giro pendiente.
 *
 * Cada muestra son dos velocidades angulares (rad/s) en los ejes de la pantalla tal como se ve: horizontal al girar el
 * teléfono hacia la derecha del jugador, vertical al levantar el borde de arriba (mirar hacia arriba). El lado nativo
 * (runtime/host_input.c) las integra en el hilo del juego y las convierte en el giro de la cámara de caza.
 */
final class MotionSensor implements SensorEventListener {
    private static final String TAG = "MHGU";

    private final SensorManager manager;
    private final Sensor gyro;
    private volatile boolean enabled;   // (el juego lo pidió: [Controls] Motion no es Off)
    private volatile boolean resumed;   // (la actividad está en primer plano)
    private volatile boolean sampling;  // (registrado: enabled y resumed, y hay giroscopio)
    private volatile int rotation = Surface.ROTATION_0;  // (la pantalla, para poner las muestras en sus ejes)
    private volatile boolean nativeOk = true;

    MotionSensor(Context context) {
        manager = (SensorManager) context.getSystemService(Context.SENSOR_SERVICE);
        gyro = manager != null ? manager.getDefaultSensor(Sensor.TYPE_GYROSCOPE) : null;
        if (gyro == null) Log.i(TAG, "motion: no gyroscope");
    }

    /** [Controls] Motion pasó a Off o dejó de estarlo. */
    void setEnabled(boolean on) {
        if (enabled == on) return;
        enabled = on;
        update();
    }

    /** La pantalla giró (GameActivity.onConfigurationChanged): las muestras, en los ejes que se ven. */
    void setRotation(int displayRotation) {
        rotation = displayRotation;
    }

    /** La actividad volvió al frente (GameActivity.onResume): su rotación, para los ejes de la pantalla. */
    void onResume(int displayRotation) {
        rotation = displayRotation;
        resumed = true;
        update();
    }

    /** La actividad pasó a segundo plano (GameActivity.onPause). */
    void onPause() {
        resumed = false;
        update();
    }

    private void update() {
        boolean want = enabled && resumed && manager != null && gyro != null;
        if (want == sampling) return;
        if (want) {
            sampling = true;
            if (!manager.registerListener(this, gyro, SensorManager.SENSOR_DELAY_GAME)) {
                sampling = false;
                Log.w(TAG, "motion: the gyroscope does not register");
            } else {
                Log.i(TAG, "motion: on");
            }
        } else {
            sampling = false;
            if (manager != null) manager.unregisterListener(this, gyro);
            send(0.0f, 0.0f);  // (nada pendiente: la próxima actualización del juego no es un giro)
            Log.i(TAG, "motion: off");
        }
    }

    @Override
    public void onSensorChanged(SensorEvent event) {
        if (!sampling) return;
        float gx = event.values[0], gy = event.values[1], sx, sy;
        // (los ejes del sensor son los del teléfono en vertical: la pantalla los gira con su rotación)
        switch (rotation) {
            case Surface.ROTATION_90:  sx = -gy; sy =  gx; break;
            case Surface.ROTATION_180: sx = -gx; sy = -gy; break;
            case Surface.ROTATION_270: sx =  gy; sy = -gx; break;
            default:                   sx =  gx; sy =  gy; break;
        }
        // (girar hacia la derecha es positivo en el eje vertical de la pantalla; mirar arriba, negativo en el horizontal)
        send(sy, -sx);
    }

    @Override
    public void onAccuracyChanged(Sensor sensor, int accuracy) {}

    private void send(float horizontal, float vertical) {
        if (!nativeOk) return;
        try {
            sample(horizontal, vertical);
        } catch (UnsatisfiedLinkError e) {
            nativeOk = false;
            Log.w(TAG, "motion: no native sample (" + e + ")");
        }
    }

    /** runtime/host_input.c: las velocidades angulares (rad/s) de este momento. */
    private static native void sample(float horizontal, float vertical);
}
