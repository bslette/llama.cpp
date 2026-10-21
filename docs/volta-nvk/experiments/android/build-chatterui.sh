#!/bin/bash
# Build the bslette ChatterUI fork as a release APK (arm64 only) in the React Native
# community Android image. APP_VARIANT=rey -> package com.Vali98.ChatterUI.rey.
# Output: ~/apps/ChatterUI/android/app/build/outputs/apk/release/app-release.apk
set -euo pipefail
podman run --rm -v $HOME/apps:/work -v $HOME/apps/cache/gradle:/root/.gradle \
  -v $HOME/apps/cache/npm:/root/.npm -w /work/ChatterUI \
  -e APP_VARIANT=rey -e NODE_OPTIONS=--max-old-space-size=8192 \
  docker.io/reactnativecommunity/react-native-android:latest bash -c "
    set -euo pipefail
    npm ci --no-audit --no-fund
    npx expo prebuild --platform android --clean --no-install
    cd android
    sed -i 's|^org.gradle.jvmargs=.*|org.gradle.jvmargs=-Xmx8g -XX:MaxMetaspaceSize=2g|' gradle.properties
    ./gradlew --no-daemon assembleRelease -PreactNativeArchitectures=arm64-v8a
    ls -la app/build/outputs/apk/release/"
