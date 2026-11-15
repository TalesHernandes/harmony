"""Compile and verify Java capture/playout patches; never rebuild the native library."""
import difflib
import copy
import hashlib
import io
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import zipfile

ANDROID = Path(__file__).resolve().parents[1]
PATCH = ANDROID / "webrtc-patch"
VERSION = "155.8059.1.0"
SHA256 = "65ebb4faf70d32b606d2132ca2f327aa92c365ac8928278cfc7b51b8a9407fc4"
PREFIX = "org/webrtc/audio/WebRtcAudioRecord"


def run(args):
    result = subprocess.run([str(a) for a in args], capture_output=True,
                            text=True, encoding="utf-8")
    if result.returncode:
        raise RuntimeError(f"Tool failed ({result.returncode}): {args[0]}\n{result.stdout}\n{result.stderr}")
    return result.stdout


def java_tool(name):
    candidates = []
    if os.environ.get("JAVA_HOME"):
        candidates.append(Path(os.environ["JAVA_HOME"]) / "bin" / name)
    if os.name == "nt":
        candidates.append(Path("C:/Program Files/Android/Android Studio/jbr/bin") / (name + ".exe"))
    found = shutil.which(name)
    if found:
        candidates.append(Path(found))
    for p in candidates:
        if p.is_file():
            return p
        if p.with_suffix(".exe").is_file():
            return p.with_suffix(".exe")
    raise RuntimeError(f"Missing {name}; set JAVA_HOME to the Android Studio JBR (JDK 25)")


def android_jar():
    properties = ANDROID / "local.properties"
    if not properties.is_file():
        raise RuntimeError("Missing android/local.properties (sdk.dir is required)")
    m = re.search(r"(?m)^sdk\.dir\s*=\s*(.+)$", properties.read_text(encoding="utf-8"))
    if not m:
        raise RuntimeError("Missing sdk.dir in android/local.properties")
    sdk = Path(m[1].strip().replace(r"\:", ":").replace(r"\\", "\\"))
    build = (ANDROID / "app/build.gradle.kts").read_text(encoding="utf-8")
    api = re.search(r"compileSdk\s*=\s*(\d+)", build)[1]
    for folder in (f"android-{api}", f"android-{api}.0"):
        jar = sdk / "platforms" / folder / "android.jar"
        if jar.is_file():
            return jar
    raise RuntimeError(f"Android SDK platform {api} missing under {sdk}")


def is_record_class(name):
    return name == PREFIX + ".class" or name.startswith(PREFIX + "$") and name.endswith(".class")


def is_playback_class(name):
    return any(name == p + ".class" or name.startswith(p + "$") and name.endswith(".class")
               for p in ("org/webrtc/audio/WebRtcAudioTrack", "org/webrtc/audio/PlaybackRoute"))


def canonical(text):
    # Constant pool indices depend on javac/debug info, not behavior. Keep all
    # actual operands, branch destinations, descriptors, exception tables and strings.
    return re.sub(r"#\d+", "#", text.replace("\r\n", "\n"))


def check_baseline(javap, original, compiled, names):
    for name in names:
        cls = name[:-6].replace("/", ".")
        a = run([javap, "-p", "-s", "-c", "-constants", original / name])
        b = run([javap, "-p", "-s", "-c", "-constants", compiled / name])
        if canonical(a) != canonical(b):
            diff = "".join(difflib.unified_diff(canonical(a).splitlines(True), canonical(b).splitlines(True)))
            raise RuntimeError(f"Unpatched Java differs from pinned AAR: {cls}\n{diff}")
        # invokedynamic uses a recipe outside the instruction stream. Check it too.
        a = run([javap, "-v", original / name])
        b = run([javap, "-v", compiled / name])
        def bootstraps(s):
            m = re.search(r"(?ms)^BootstrapMethods:\n(.*?)(?=^\S|\Z)", s)
            return canonical(m[0]) if m else ""
        if bootstraps(a) != bootstraps(b):
            raise RuntimeError(f"Bootstrap recipes differ in {cls}")


def main():
    original = ANDROID / f"app/libs/libwebrtc-{VERSION}.aar"
    output = ANDROID / f"app/libs/libwebrtc-{VERSION}-capture.aar"
    if hashlib.sha256(original.read_bytes()).hexdigest() != SHA256:
        raise RuntimeError("Original AAR checksum mismatch; refusing to patch")
    javac, javap = java_tool("javac"), java_tool("javap")
    sdk_jar = android_jar()
    (ANDROID / "build").mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="webrtc-capture-", dir=ANDROID / "build") as temporary:
        work = Path(temporary)
        archive = zipfile.ZipFile(original)
        jar_data = archive.read("classes.jar")
        classes_jar = work / "classes.jar"
        classes_jar.write_bytes(jar_data)
        jar = zipfile.ZipFile(io.BytesIO(jar_data))
        original_classes = work / "original"
        # Extract for direct-file javap inspection (no classpath lookup required).
        jar.extractall(original_classes)
        names = sorted(n for n in jar.namelist() if is_record_class(n))
        major = struct.unpack(">H", jar.read(PREFIX + ".class")[6:8])[0]
        if major != 69:
            raise RuntimeError(f"Pinned AAR expected Java 25 (major 69), got {major}")
        # Compile-only annotation declarations: same binary names and retention;
        # none of these stubs goes into the AAR. No extra Maven dependency.
        annotations = work / "annotations/androidx/annotation"
        annotations.mkdir(parents=True)
        (annotations / "Nullable.java").write_text(
            "package androidx.annotation; import java.lang.annotation.*; "
            "@Retention(RetentionPolicy.CLASS) @Target({ElementType.METHOD, ElementType.PARAMETER, "
            "ElementType.FIELD, ElementType.LOCAL_VARIABLE, ElementType.TYPE_USE}) public @interface Nullable {}")
        (annotations / "RequiresApi.java").write_text(
            "package androidx.annotation; import java.lang.annotation.*; "
            "@Retention(RetentionPolicy.CLASS) public @interface RequiresApi { int value(); }")
        sources = list(annotations.glob("*.java"))
        def compile_source(source, destination):
            run([javac, "-source", "25", "-target", "25", "-proc:none", "-encoding", "UTF-8",
                 "-classpath", os.pathsep.join(map(str, [classes_jar, sdk_jar])),
                 "-d", destination, *sources, source])
        baseline = work / "baseline"
        compile_source(PATCH / "baseline/WebRtcAudioRecord.java", baseline)
        generated = sorted(p.relative_to(baseline).as_posix() for p in baseline.rglob("*.class")
                           if is_record_class(p.relative_to(baseline).as_posix()))
        if generated != names:
            raise RuntimeError(f"Unpatched nested class inventory differs: {generated} != {names}")
        check_baseline(javap, original_classes, baseline, names)
        patched = work / "patched"
        baseline_source = (PATCH / "baseline/WebRtcAudioRecord.java").read_text(encoding="utf-8")
        patched_source = (PATCH / "src/org/webrtc/audio/WebRtcAudioRecord.java").read_text(encoding="utf-8")
        documented_diff = "".join(difflib.unified_diff(
            baseline_source.splitlines(True), patched_source.splitlines(True),
            fromfile="baseline/WebRtcAudioRecord.java", tofile="src/org/webrtc/audio/WebRtcAudioRecord.java"))
        if documented_diff != (PATCH / "capture.patch").read_text(encoding="utf-8"):
            raise RuntimeError("capture.patch is stale; review and update the documented diff")
        compile_source(PATCH / "src/org/webrtc/audio/WebRtcAudioRecord.java", patched)
        replacements = {p.relative_to(patched).as_posix(): p.read_bytes()
                        for p in patched.rglob("*.class") if is_record_class(p.relative_to(patched).as_posix())}
        if not set(names).issubset(replacements) or len(replacements) != len(names) + 1:
            raise RuntimeError("Patched nested class inventory changed unexpectedly")
        playback_names = sorted(n for n in jar.namelist() if is_playback_class(n))
        playback_baseline = work / "playback-baseline"
        compile_source(PATCH / "baseline/WebRtcAudioTrack.java", playback_baseline)
        generated = sorted(p.relative_to(playback_baseline).as_posix()
                           for p in playback_baseline.rglob("*.class")
                           if is_playback_class(p.relative_to(playback_baseline).as_posix()))
        if generated != playback_names:
            raise RuntimeError("Unpatched playback class inventory differs")
        check_baseline(javap, original_classes, playback_baseline, playback_names)
        a = (PATCH / "baseline/WebRtcAudioTrack.java").read_text(encoding="utf-8")
        b = (PATCH / "src/org/webrtc/audio/WebRtcAudioTrack.java").read_text(encoding="utf-8")
        diff = "".join(difflib.unified_diff(a.splitlines(True), b.splitlines(True), n=0,
                       fromfile="baseline/WebRtcAudioTrack.java", tofile="src/org/webrtc/audio/WebRtcAudioTrack.java"))
        if diff != (PATCH / "playback.patch").read_text(encoding="utf-8"):
            raise RuntimeError("playback.patch is stale")
        # Compile both files together; no additional library or native changes.
        run([javac, "-source", "25", "-target", "25", "-proc:none", "-encoding", "UTF-8",
             "-classpath", os.pathsep.join(map(str, [classes_jar, sdk_jar])), "-d", patched,
             *sources, PATCH / "src/org/webrtc/audio/PlaybackRoute.java",
             PATCH / "src/org/webrtc/audio/WebRtcAudioTrack.java"])
        replacements.update({p.relative_to(patched).as_posix(): p.read_bytes()
                             for p in patched.rglob("*.class")
                             if is_playback_class(p.relative_to(patched).as_posix())})
        names += playback_names
        for name in names:
            a = run([javap, "-p", "-s", original_classes / name])
            b = run([javap, "-p", "-s", patched / name])
            def members(s):
                return set(re.findall(r"(?m)^  [^\n]+;\n    descriptor: [^\n]+", s))
            if not members(a).issubset(members(b)):
                raise RuntimeError(f"Original JNI fields/method descriptors changed in {name}")
        thread = run([javap, "-p", "-c", patched / (PREFIX + "$AudioRecordThread.class")])
        playback_thread = run([javap, "-p", "-c", patched / "org/webrtc/audio/WebRtcAudioTrack$AudioTrackThread.class"])
        markers = ["android/media/AudioTrack.write:", "PlaybackRoute$Meter.written:", "java/nio/ByteBuffer.rewind:"]
        positions = [playback_thread.index(marker) for marker in markers]
        if positions != sorted(positions) or any(playback_thread.count(marker) != 1 for marker in markers):
            raise RuntimeError("Expected AudioTrack.write -> latency meter -> rewind order")
        if "PlaybackRoute$Meter.clear:" not in playback_thread:
            raise RuntimeError("Playback exit must invalidate latency")
        meter = run([javap, "-p", "-c", patched / "org/webrtc/audio/PlaybackRoute$Meter.class"])
        for marker in ("android/media/AudioTrack.getTimestamp:", "android/media/AudioTrack.getRoutedDevice:"):
            if marker not in meter:
                raise RuntimeError(f"Missing public playback measurement API: {marker}")
        route_api = run([javap, "-p", "-s", patched / "org/webrtc/audio/PlaybackRoute.class"])
        for signature in ("public static int outputLatencyMs();", "public static int routedDeviceType();",
                          "public static org.webrtc.audio.PlaybackRoute$Output output();"):
            if signature not in route_api:
                raise RuntimeError(f"Missing playback telemetry getter: {signature}")
        order = ["CaptureProcessor.process:", "WebRtcAudioRecord.microphoneMute:",
                 "java/nio/ByteBuffer.put:([B)", "CaptureProcessor.processAfterMute:",
                 "Natives.dataIsRecorded:", "onWebRtcAudioRecordSamplesReady:"]
        # The mute field and its zeroing put must sit between the filter and
        # playback hooks in compiled code.
        positions = [thread.index(marker) for marker in order]
        if positions != sorted(positions) or any(thread.count(marker) != 1 for marker in order):
            raise RuntimeError("Expected filter -> microphone mute/zero -> playback -> JNI -> callback order")
        buffer = io.BytesIO()
        with zipfile.ZipFile(buffer, "w") as modified:
            for info in jar.infolist():
                modified.writestr(copy.copy(info), replacements.pop(info.filename, jar.read(info.filename)))
            for name, data in replacements.items():
                info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
                modified.writestr(info, data, compress_type=zipfile.ZIP_DEFLATED)
        new_jar = buffer.getvalue()
        staged = work / "patched.aar"
        with zipfile.ZipFile(staged, "w") as modified:
            for info in archive.infolist():
                modified.writestr(copy.copy(info), new_jar if info.filename == "classes.jar" else archive.read(info.filename))
        # Every other Java class and every native/manifest entry must be byte-for-byte identical.
        with zipfile.ZipFile(io.BytesIO(new_jar)) as checked:
            for name in jar.namelist():
                if not is_record_class(name) and not is_playback_class(name) and checked.read(name) != jar.read(name):
                    raise RuntimeError(f"Unexpected Java mutation: {name}")
        with zipfile.ZipFile(staged) as checked:
            for name in archive.namelist():
                if name != "classes.jar" and checked.read(name) != archive.read(name):
                    raise RuntimeError(f"Unexpected AAR mutation: {name}")
        archive.close()
        data = staged.read_bytes()
        if not output.is_file() or output.read_bytes() != data:
            # Publish only after all checks; never leave a partially valid AAR.
            part = output.with_suffix(".aar.part")
            part.write_bytes(data)
            os.replace(part, output)
        print(f"Verified baseline bytecode + JNI signatures; generated {output.name} ({len(data)} bytes)")


if __name__ == "__main__":
    main()
