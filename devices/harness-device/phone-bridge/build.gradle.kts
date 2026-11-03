import org.jetbrains.kotlin.gradle.dsl.JvmTarget

// Phone bridge relay app: BLE L2CAP CoC (dial) <-> WebSocket (daemon tunnel). A dumb pipe.
// Wire spec: docs/specs/cable-ble-link.md. Versions match the bench app (dial-ble-bench-android).
plugins {
    id("com.android.application") version "8.13.0"
    id("org.jetbrains.kotlin.android") version "2.2.20"
}

android {
    namespace = "dev.openharness.phonebridge"
    compileSdk = 36

    defaultConfig {
        applicationId = "dev.openharness.phonebridge"
        minSdk = 31
        targetSdk = 36
        versionCode = 1
        versionName = "0.1"
    }

    buildFeatures {
        buildConfig = true
    }

    buildTypes {
        debug {
            // M1 (walking skeleton): the desktop daemon is reached through `adb reverse tcp:18481 tcp:18481`.
            buildConfigField("String", "TUNNEL_URL", "\"ws://127.0.0.1:18481/relay\"")
            // Unbonded CoC, because TLS-PSK already authenticates both ends. Dev only; never true in release.
            buildConfigField("boolean", "DEV_ALLOW_UNBONDED", "true")
        }
        release {
            buildConfigField("String", "TUNNEL_URL", "\"wss://dial.mayankchetan.com/relay\"")
            buildConfigField("boolean", "DEV_ALLOW_UNBONDED", "false")
            isMinifyEnabled = false
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

kotlin {
    compilerOptions {
        jvmTarget.set(JvmTarget.JVM_17)
    }
}

dependencies {
    implementation("com.squareup.okhttp3:okhttp:4.12.0")
    testImplementation("junit:junit:4.13.2")
}
