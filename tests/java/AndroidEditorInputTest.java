package org.libsdl.app;

import android.content.Context;
import android.text.InputType;
import android.view.*;
import android.view.inputmethod.*;

/** Tests the real editor's event registration; Android framework and JNI are spies. */
public final class AndroidEditorInputTest {
    public static void main(String[] args) {
        boolean reference=Boolean.getBoolean("reference");
        View surface=new View(new Context());
        surface.setOnGenericMotionListener(SDLActivity.getMotionListener());
        SDLDummyEdit edit=new SDLDummyEdit(new Context());
        SDLActivity.mTextEdit=edit;
        edit.setInputType(InputType.TYPE_CLASS_TEXT);
        EditorInfo info=new EditorInfo();
        assert edit.onCheckIsTextEditor();
        assert edit.onCreateInputConnection(info)!=null;
        assert info.inputType==InputType.TYPE_CLASS_TEXT;
        assert info.imeOptions==(EditorInfo.IME_FLAG_NO_EXTRACT_UI|EditorInfo.IME_FLAG_NO_FULLSCREEN);
        int expectedAxes=0;
        for (int round=0;round<100;round++) {
            // Show editor, accept/cancel it while focus lingers, then reopen it.
            for (int phase=0;phase<3;phase++) {
                edit.requestFocus();
                if (phase==2) {
                    assert !edit.onKeyPreIme(KeyEvent.KEYCODE_BACK,new KeyEvent(KeyEvent.ACTION_UP));
                }
                int beforeKeys=SDLActivity.keys;
                assert edit.dispatchKeyEvent(96,new KeyEvent(KeyEvent.ACTION_DOWN));
                assert edit.dispatchKeyEvent(96,new KeyEvent(KeyEvent.ACTION_UP));
                assert SDLActivity.keys==beforeKeys+2;
                for (int device:new int[]{7,19}) {
                    for (float[] axes:new float[][]{{-1,0,0,0},{1,0,0,0},{0,1,0,0},{0,-1,0,0},
                            {0,0,-1,0},{0,0,1,0},{0,0,0,-1},{0,0,0,1},{0,0,0,0}}) {
                        MotionEvent event=new MotionEvent(device,axes[0],axes[1],axes[2],axes[3],true);
                        assert edit.dispatchGenericMotionEvent(event)==!reference;
                        if (!reference) {
                            expectedAxes++;
                            assert SDLActivity.last==event; // No axis/source/device rewriting or duplicates.
                        }
                        assert SDLActivity.axes==expectedAxes;
                    }
                }
                int before=SDLActivity.axes;
                assert !edit.dispatchGenericMotionEvent(new MotionEvent(7,0,0,0,0,false));
                assert SDLActivity.axes==before;
            }
            // Mirrors the focus restoration done by SDLSurface.handleResume().
            surface.requestFocus();
            assert surface.dispatchGenericMotionEvent(new MotionEvent(7,1,0,0,0,true));
            assert SDLActivity.axes==++expectedAxes;
        }
        assert SDLActivity.hidden==100;
        System.out.println(reference
                ? "PASS baseline: focused editor drops joystick/hat motion; keys and surface focus recovery work."
                : "PASS fixed: 100 repeated editor cycles, 5400 axis/hat/neutral events delivered once; keys, IME Back and editor options preserved.");
    }
}
