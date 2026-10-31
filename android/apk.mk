# The APK (Makefile, PLATFORM=android; settings in android/android.mk), made with the SDK's own tools, no Gradle:
# SDL's Java sources (javac + d8 -> classes.dex), the manifest (aapt2), then the libraries of every ABI built so far
# (build-android/lib/<abi>/, without their debug info, stored uncompressed and 16 KB aligned: Android maps them
# straight from the APK; the build folder keeps them whole, for symbols in crash backtraces), aligned
# (zipalign) and signed (apksigner) with the SDK's debug key in ~/.android/debug.keystore (made once).
#   make PLATFORM=android ABI=x86_64 install    the APK onto the running emulator / the connected phone (adb)
BUILD_TOOLS := $(lastword $(sort $(wildcard $(ANDROID_SDK)/build-tools/*)))
ANDROID_JAR := $(lastword $(sort $(wildcard $(ANDROID_SDK)/platforms/android-*/android.jar)))
TARGET_API  := $(patsubst android-%,%,$(notdir $(patsubst %/,%,$(dir $(ANDROID_JAR)))))
ADB         := $(ANDROID_SDK)/platform-tools/adb
APK         := $(B)/mhgurecomp.apk
APK_O       := obj-android/apk
JAVA_SRC    := $(SDL3_DIR)/src/android-project/app/src/main/java/org/libsdl/app
DEBUG_KEY   := $(HOME)/.android/debug.keystore
# versionCode: the commits so far (grows with every build of a newer commit); versionName: the build's VERSION
APK_VERSION_CODE := $(shell git rev-list --count HEAD 2>/dev/null || echo 1)
# (the .bat tools of the SDK need the JDK, found through JAVA_HOME)
SDK_ENV     := JAVA_HOME="$(call win_path,$(JDK))" PATH="$(JDK)/bin:$$PATH"

apk: $(APK)

$(APK_O)/classes.dex: $(SDL3_DIR)/.built
	rm -rf $(APK_O)/classes $(APK_O)/dex && mkdir -p $(APK_O)/classes $(APK_O)/dex
	"$(JDK)/bin/javac" -nowarn -Xlint:-options -source 8 -target 8 -encoding UTF-8 -bootclasspath "$(call win_path,$(ANDROID_JAR))" \
	    -d $(APK_O)/classes $(JAVA_SRC)/*.java
	$(SDK_ENV) "$(BUILD_TOOLS)/d8.bat" --release --min-api $(API) --lib "$(call win_path,$(ANDROID_JAR))" --output $(APK_O)/dex \
	    $$(find $(APK_O)/classes -name '*.class')
	cp $(APK_O)/dex/classes.dex $@

$(DEBUG_KEY):
	@mkdir -p $(dir $@)
	"$(JDK)/bin/keytool" -genkeypair -keystore "$(call win_path,$@)" -storepass android -alias androiddebugkey \
	    -keypass android -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Android Debug,O=Android,C=US"

# (the libraries: whichever ABIs build-android/lib/ has; rebuilt whenever this ABI's change)
$(APK): android/AndroidManifest.xml $(APK_O)/classes.dex $(EXE) $(if $(GEN_SRC),$(GAMELIB)) $(SDL_DEPS) $(DEBUG_KEY) android/apk.py
	@mkdir -p $(B)/lib/$(ABI) $(APK_O)
	cp $(SDL_DEPS) $(B)/lib/$(ABI)/
	"$(BUILD_TOOLS)/aapt2" link -o $(APK_O)/base.apk --manifest android/AndroidManifest.xml -I "$(call win_path,$(ANDROID_JAR))" \
	    --min-sdk-version $(API) --target-sdk-version $(TARGET_API) --version-code $(APK_VERSION_CODE) --version-name $(VERSION)
	rm -rf $(APK_O)/lib && for d in $(B)/lib/*/; do a=$$(basename $$d); mkdir -p $(APK_O)/lib/$$a; \
	    for f in $$d*.so; do "$(LLVM)/llvm-strip" --strip-debug -o $(APK_O)/lib/$$a/$$(basename $$f) $$f || exit 1; done; done
	$(PY) android/apk.py $(APK_O)/base.apk $(APK_O)/unaligned.apk $(APK_O)/classes.dex $(APK_O)/lib
	"$(BUILD_TOOLS)/zipalign" -f -P 16 4 $(APK_O)/unaligned.apk $(APK_O)/aligned.apk
	$(SDK_ENV) "$(BUILD_TOOLS)/apksigner.bat" sign --ks "$(call win_path,$(DEBUG_KEY))" --ks-pass pass:android \
	    --key-pass pass:android --out $@ $(APK_O)/aligned.apk
	@echo "apk: $@ ($$(du -h $@ | cut -f1), ABIs: $$(ls $(B)/lib | tr '\n' ' '))"

install: $(APK)
	"$(ADB)" install -r $(APK)

.PHONY: apk install
