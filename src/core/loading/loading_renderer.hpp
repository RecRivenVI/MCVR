#pragma once

#include <jni.h>

// Bind the GLFW instance already loaded by LWJGL. Never load a second GLFW.
bool bindLoadedGlfw(JNIEnv *env, jobjectArray candidates);
void releaseLoadingRenderer();
