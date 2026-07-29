/*
 * jni_shim.h -- fake JNI environment for Syberia
 *
 * Provides a fake JavaVM and JNIEnv with stub vtables so that
 * JNI calls from libsyberia1.so don't crash.
 */

#ifndef __JNI_SHIM_H__
#define __JNI_SHIM_H__

void jni_shim_init(void **out_vm, void **out_env);

// recon: acha o ponteiro de um metodo nativo registrado via RegisterNatives
void *jni_find_native(const char *name);

// Unity soft keyboard bridge (Terraria name/world text fields)
int jni_softinput_active(void);
const char *jni_softinput_text(void);
int jni_softinput_limit(void);
void jni_softinput_open(const char *text, int limit);
void jni_softinput_set_text(const char *text);
void jni_softinput_commit(const char *text);
void jni_softinput_cancel(void);

// Processa callbacks Java assíncronos no thread da Unity, fora de nativeRender.
void jni_poll_java_callbacks(void *env);

// FMOD AudioTrack: ByteBuffer sentinel + PCM buffer p/ a thread de audio
void *jni_fmod_bytebuffer(void);
void *jni_fmod_pcm(void);
int jni_fmod_pcm_size(void);
void jni_fmod_set_pcm_size(int bytes);
void *jni_fmod_device(void);
int jni_fmod_should_run(void);

// Flush the Android SharedPreferences replacement before process teardown.
void jni_prefs_flush(void);

// Set package name and OBB version (call before jni_shim_init)
void jni_shim_set_package(const char *package_name, int obb_version);

#endif
