import org.jetbrains.kotlin.gradle.dsl.JvmTarget
import java.util.Properties

// Phone bridge relay app: BLE L2CAP CoC (dial) <-> WebSocket (daemon tunnel). A dumb pipe.
// Wire spec: docs/specs/cable-ble-link.md. Versions match the bench app (dial-ble-bench-android).
plugins {
    id("com.android.application") version "8.13.0"
    id("org.jetbrains.kotlin.android") version "2.2.20"
}

// Release signing. The keystore and its passwords stay outside the repository. Point PHONEBRIDGE_SIGNING (an environment
// variable) or -PPHONEBRIDGE_SIGNING (a Gradle property) at a properties file with storeFile, storePassword, keyAlias and
// keyPassword. Without it (or if the file is missing) assembleRelease still works and gives an unsigned APK.
val signingProps: Properties? =
    (System.getenv("PHONEBRIDGE_SIGNING") ?: (findProperty("PHONEBRIDGE_SIGNING") as String?))
        ?.let { file(it) }
        ?.takeIf { it.isFile }
        ?.let { f -> Properties().also { p -> f.inputStream().use(p::load) } }

android {
    namespace = "dev.openharness.phonebridge"
    compileSdk = 36

    defaultConfig {
        applicationId = "dev.openharness.phonebridge"
        minSdk = 31
        targetSdk = 36
        versionCode = 2
        versionName = "1.0"
    }

    buildFeatures {
        buildConfig = true
    }

    signingConfigs {
        if (signingProps != null) {
            create("release") {
                storeFile = file(signingProps.getProperty("storeFile"))
                storePassword = signingProps.getProperty("storePassword")
                keyAlias = signingProps.getProperty("keyAlias")
                keyPassword = signingProps.getProperty("keyPassword")
            }
        }
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
            if (signingProps != null) signingConfig = signingConfigs.getByName("release")
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
