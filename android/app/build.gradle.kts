plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.plugin.compose")
}

// Shiguredo's libwebrtc, fetched by scripts/fetch-webrtc.sh (pinned by checksum).
val webrtcAar = "libs/libwebrtc-155.8059.1.0-capture.aar"

// The Firebase client config, read from the google-services.json downloaded
// from the console (kept out of git: it carries an API key). Done by hand
// instead of with Google's gradle plugin: it is five strings, and the plugin
// would be one more dependency to do what these lines do. Without the file the
// app builds and runs the same, only without notifications (Push.kt).
val firebase: Map<String, String> = run {
    val f = file("google-services.json")
    if (!f.exists()) return@run emptyMap()
    @Suppress("UNCHECKED_CAST")
    val g = groovy.json.JsonSlurper().parse(f) as Map<String, Any?>
    val project = g["project_info"] as Map<String, Any?>
    val client = (g["client"] as List<Map<String, Any?>>).first {
        ((it["client_info"] as Map<String, Any?>)["android_client_info"] as Map<String, Any?>)["package_name"] ==
            "com.taleshernandes.harmony"
    }
    mapOf(
        "google_app_id" to (client["client_info"] as Map<String, Any?>)["mobilesdk_app_id"] as String,
        "gcm_defaultSenderId" to project["project_number"] as String,
        "project_id" to project["project_id"] as String,
        "google_storage_bucket" to project["storage_bucket"] as String,
        "google_api_key" to ((client["api_key"] as List<Map<String, Any?>>).first()["current_key"] as String),
    )
}

// The release key (A8, 26/09/2026). It lives outside the repository, and
// both its path and its password come from the environment
// (HARMONY_KEYSTORE, HARMONY_KEYSTORE_PASSWORD). Without them the release
// build refuses to finish rather than quietly signing with the debug key: a
// debug-signed APK can never update the installed app, and a phone that got
// one would have to uninstall to come back.
val releaseKey: Pair<File, String>? = run {
    val path = System.getenv("HARMONY_KEYSTORE") ?: return@run null
    val pass = System.getenv("HARMONY_KEYSTORE_PASSWORD") ?: return@run null
    file(path).takeIf { it.exists() }?.let { it to pass }
}

android {
    // Native libraries compressed in the APK: libdf (DeepFilterNet, 13.5 MB)
    // and libwebrtc (13.1 MB) shrink to 4.5 and 6.2. Stored, the APK went
    // from 16 to 37.6 MB the day the noise filter came in (01/10/2026); a
    // friend downloads it as a file, and the price is a one-time unpack of
    // about 26 MB at install.
    packaging { jniLibs { useLegacyPackaging = true } }
    namespace = "com.taleshernandes.harmony"
    compileSdk = 37

    defaultConfig {
        applicationId = "com.taleshernandes.harmony"
        minSdk = 26
        targetSdk = 36
        versionCode = 53
        versionName = "0.46"
        // The WebRTC library is arm64 only (decided with the owner, 18/09/2026).
        ndk { abiFilters += "arm64-v8a" }
        // See VoiceCall.loopbackAlias: empty in release, where it can never apply.
        buildConfigField("String", "LOOPBACK_ALIAS", "\"\"")
        for ((name, value) in firebase) resValue("string", name, value)
    }

    signingConfigs {
        if (releaseKey != null) create("release") {
            storeFile = releaseKey.first
            storePassword = releaseKey.second
            keyAlias = "harmony"
            keyPassword = releaseKey.second
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            signingConfig = signingConfigs.findByName("release")
        }
        getByName("debug") {
            buildConfigField("String", "LOOPBACK_ALIAS", "\"10.0.2.2\"")
        }
        // The release build, shrunk the same way, that may also reach the local
        // test server over plain http. The emulator harness runs this one: the
        // first release shipped a crash that only R8 produced, because only the
        // debug build had been run (24/09/2026).
        create("qa") {
            initWith(getByName("release"))
            matchingFallbacks += "release"
            // The emulator's build: signed with the debug key, so it never
            // needs the release key on the machine that runs the harness.
            signingConfig = signingConfigs.getByName("debug")
            buildConfigField("String", "LOOPBACK_ALIAS", "\"10.0.2.2\"")
        }
    }
    sourceSets["qa"].manifest.srcFile("src/debug/AndroidManifest.xml")
    sourceSets["qa"].res.directories += "src/debug/res"
    buildFeatures {
        compose = true
        buildConfig = true
        resValues = true
    }
    testOptions { unitTests.isReturnDefaultValues = true }
}

// Fail the release build loudly when the key is missing (see releaseKey).
tasks.matching { it.name == "packageRelease" }.configureEach {
    doFirst {
        if (releaseKey == null) throw GradleException(
            "HARMONY_KEYSTORE / HARMONY_KEYSTORE_PASSWORD not set: refusing to build an APK that cannot update the installed app"
        )
    }
}

dependencies {
    implementation(platform("androidx.compose:compose-bom:2026.09.00"))
    implementation("androidx.compose.material3:material3")
    implementation("androidx.activity:activity-compose:1.13.0")
    implementation("com.squareup.okhttp3:okhttp:5.5.0")
    implementation(files(webrtcAar))
    implementation("io.github.kaleyravideo:android-deepfilternet:0.0.8")
    // Notifications with the app closed (A6, approved by the owner 25/09/2026:
    // 46 modules, +384 KB). Only ids travel through it (Push.kt).
    implementation("com.google.firebase:firebase-messaging:25.1.3")
    // Firebase drags in fragment 1.1, whose permission results are broken
    // under the ActivityResult API the app uses (lint refuses the release).
    // This only raises the version of something already in the build.
    implementation("androidx.fragment:fragment:1.8.9")

    testImplementation("junit:junit:4.13.2")
    // Android's own org.json is a stub off the device; this is the same library.
    testImplementation("org.json:json:20260814")
}
