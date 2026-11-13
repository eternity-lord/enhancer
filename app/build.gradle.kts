plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

val generatedCodeformerAssetsDir = layout.buildDirectory.dir("generated/codeformer_assets")
val generatedNcnnDir = layout.buildDirectory.dir("generated/ncnn-android-vulkan")

val prepareEnhancerDeps by tasks.registering(Exec::class) {
    workingDir = rootProject.projectDir
    commandLine("bash", "${rootProject.projectDir}/scripts/prepare_build_deps.sh")
}

android {
    namespace = "com.enhancer.app"
    compileSdk = 34
    ndkVersion = "30.0.16248370"

    defaultConfig {
        applicationId = "com.enhancer.app"
        minSdk = 24
        targetSdk = 34
        versionCode = 3
        versionName = "3.0"

        val configuredAbis = (project.findProperty("enhancerAbis") as? String)
            ?.split(",")
            ?.map { it.trim() }
            ?.filter { it.isNotEmpty() }
            ?: listOf("arm64-v8a", "armeabi-v7a", "x86_64")

        ndk {
            abiFilters += configuredAbis
        }

        externalNativeBuild {
            cmake {
                cppFlags += listOf("-std=c++17", "-fexceptions")
                arguments += listOf(
                    "-DANDROID_STL=c++_shared",
                    "-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON",
                    "-DENHANCER_GENERATED_NCNN_DIR=${generatedNcnnDir.get().asFile.absolutePath}"
                )
            }
        }

        vectorDrawables {
            useSupportLibrary = true
        }
    }

    sourceSets {
        getByName("main") {
            assets.srcDir(generatedCodeformerAssetsDir)
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
        debug {
            isMinifyEnabled = false
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    androidResources {
        noCompress += listOf("param", "bin")
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    buildFeatures {
        compose = true
        buildConfig = true
    }

    composeOptions {
        kotlinCompilerExtensionVersion = "1.5.14"
    }

    packaging {
        resources {
            excludes += "/META-INF/{AL2.0,LGPL2.1}"
        }
    }
}

tasks.matching {
    it.name == "preBuild" ||
        it.name.startsWith("merge") && it.name.endsWith("Assets") ||
        it.name.startsWith("configureCMake") ||
        it.name.startsWith("buildCMake")
}.configureEach {
    dependsOn(prepareEnhancerDeps)
}

dependencies {
    val composeBom = platform("androidx.compose:compose-bom:2024.06.00")
    implementation(composeBom)

    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.8.4")
    implementation("androidx.lifecycle:lifecycle-runtime-compose:2.8.4")
    implementation("androidx.lifecycle:lifecycle-viewmodel-compose:2.8.4")
    implementation("androidx.activity:activity-compose:1.9.1")

    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.ui:ui-graphics")
    implementation("androidx.compose.ui:ui-tooling-preview")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.material:material-icons-extended")

    implementation("androidx.exifinterface:exifinterface:1.3.7")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.8.1")

    debugImplementation("androidx.compose.ui:ui-tooling")
}

tasks.matching { it.name == "assembleDebug" }.configureEach {
    doLast {
        val debugApkDir = layout.buildDirectory.dir("outputs/apk/debug").get().asFile
        val sourceApk = debugApkDir.resolve("app-debug.apk")
        val renamedApk = debugApkDir.resolve("Enhancer-v3.0-debug.apk")
        if (sourceApk.exists()) {
            sourceApk.copyTo(renamedApk, overwrite = true)
        }
    }
}
