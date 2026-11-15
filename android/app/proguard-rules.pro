# libwebrtc calls back into Java from native code by name, and Shiguredo's AAR
# ships no consumer rules; without these R8 strips or renames what JNI needs.
# The AAR carries three packages, and all three are looked up from C++:
# org.webrtc, org.jni_zero (the JNI glue, found by FindClass at load time —
# stripping it aborted the first release build on the owner's phone,
# 24/09/2026) and J (jni_zero's generated natives).
-keep class org.webrtc.** { *; }
-keep class org.jni_zero.** { *; }
-keep class J.** { *; }
-dontwarn org.webrtc.**
-dontwarn org.jni_zero.**

# Exception names go into the diagnostic log ("entrar falhou (SocketTimeoutException)");
# renamed to "z9" they say nothing. Names only: the code is still shrunk.
-keepnames class * extends java.lang.Throwable
