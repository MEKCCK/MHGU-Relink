"""Compile the actual SDL editor against small JVM Android input spies."""
from pathlib import Path
import hashlib
import json
import os
import subprocess

project = Path(__file__).resolve().parents[1]
output = Path('D:/a9726/Documents/ChatGPT/安卓移植GU汉化软件/analysis/controller-ip-0.2.2-20261113/host-editor')
output.mkdir(parents=True, exist_ok=True)
java = Path(os.environ['JAVA_HOME']) / 'bin'
stubs = {
 'android/content/Context.java': 'package android.content; public class Context {}',
 'android/text/InputType.java': 'package android.text; public class InputType { public static final int TYPE_CLASS_TEXT=1; }',
 'android/view/inputmethod/InputConnection.java': 'package android.view.inputmethod; public interface InputConnection {}',
 'android/view/inputmethod/EditorInfo.java': 'package android.view.inputmethod; public class EditorInfo { public int inputType,imeOptions; public static final int IME_FLAG_NO_EXTRACT_UI=1,IME_FLAG_NO_FULLSCREEN=2; }',
 'android/view/KeyEvent.java': '''package android.view; public class KeyEvent {
 public static final int ACTION_DOWN=0,ACTION_UP=1,KEYCODE_BACK=4; private final int action;
 public KeyEvent(int action) { this.action=action; } public int getAction() { return action; } }''',
 'android/view/MotionEvent.java': '''package android.view; public class MotionEvent {
 public final int device; public final float x,y,hx,hy; public final boolean joystick;
 public MotionEvent(int d,float x,float y,float hx,float hy,boolean j) { device=d;this.x=x;this.y=y;this.hx=hx;this.hy=hy;joystick=j; } }''',
 'android/view/View.java': '''package android.view;
 import android.content.Context; import android.view.inputmethod.*;
 public class View { public static final int VISIBLE=0; public static View focused;
 public interface OnKeyListener { boolean onKey(View v,int key,KeyEvent event); }
 public interface OnGenericMotionListener { boolean onGenericMotion(View v,MotionEvent event); }
 private OnKeyListener key; private OnGenericMotionListener motion;
 public View(Context c) {} public void setFocusable(boolean b) {} public void setFocusableInTouchMode(boolean b) {}
 public boolean requestFocus() { focused=this; return true; }
 public void setOnKeyListener(OnKeyListener k) { key=k; }
 public void setOnGenericMotionListener(OnGenericMotionListener m) { motion=m; }
 public boolean dispatchGenericMotionEvent(MotionEvent e) { return focused==this && motion!=null && motion.onGenericMotion(this,e); }
 public boolean dispatchKeyEvent(int code,KeyEvent e) { return focused==this && key!=null && key.onKey(this,code,e); }
 public int getVisibility() { return VISIBLE; } public boolean onCheckIsTextEditor() { return false; }
 public boolean onKeyPreIme(int code,KeyEvent e) { return false; }
 public InputConnection onCreateInputConnection(EditorInfo e) { return null; } }''',
 'org/libsdl/app/SDLActivity.java': '''package org.libsdl.app; import android.view.*; import android.view.inputmethod.*;
 public class SDLActivity { public static SDLDummyEdit mTextEdit; public static int axes,keys,hidden;
 public static MotionEvent last; private static final View.OnGenericMotionListener listener=(v,e)->{
 if (!e.joystick) return false; axes++; last=e; return true; };
 public static View.OnGenericMotionListener getMotionListener() { return listener; }
 public static boolean handleKeyEvent(View v,int code,KeyEvent e,InputConnection ic) { keys++; return true; }
 public static void onNativeKeyboardFocusLost() { hidden++; } }''',
 'org/libsdl/app/SDLInputConnection.java': '''package org.libsdl.app; import android.view.*; import android.view.inputmethod.*;
 public class SDLInputConnection implements InputConnection { public SDLInputConnection(View v,boolean full) {} }''',
}
for path, text in stubs.items():
    p=output/'stubs'/path
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(text, encoding='utf-8')
records=[]
production=project/'android/app/src/main/java/org/libsdl/app/SDLDummyEdit.java'
for reference in (True, False):
    name='baseline' if reference else 'fixed'
    folder=output/name
    folder.mkdir(exist_ok=True)
    source=subprocess.check_output(['git','show','35a8c23:android/app/src/main/java/org/libsdl/app/SDLDummyEdit.java'],cwd=project) if reference else production.read_bytes()
    editor=folder/'SDLDummyEdit.java'
    editor.write_bytes(source)
    compile=subprocess.run([str(java/'javac.exe'),'-encoding','UTF-8','-d',str(folder),
        *map(str,(output/'stubs').rglob('*.java')),str(editor),str(project/'tests/java/AndroidEditorInputTest.java')],capture_output=True,timeout=60)
    (folder/'compile.log').write_bytes(compile.stdout+compile.stderr)
    if compile.returncode:
        print((compile.stdout+compile.stderr).decode(errors='replace'))
    compile.check_returncode()
    run=subprocess.run([str(java/'java.exe'),'-ea','-Dreference='+str(reference).lower(),'-cp',str(folder),'org.libsdl.app.AndroidEditorInputTest'],capture_output=True,timeout=30)
    (folder/'regression.log').write_bytes(run.stdout+run.stderr)
    print((run.stdout+run.stderr).decode(errors='replace'),end='')
    run.check_returncode()
    records.append({'name':name,'passed':True,'editor_sha256':hashlib.sha256(source).hexdigest()})
(output/'results.json').write_text(json.dumps(records,indent=2),encoding='utf-8')
