//
// Copyright (c) 2013 Mikko Mononen memon@inside.org
//
// This software is provided 'as-is', without any express or implied
// warranty.  In no event will the authors be held liable for any damages
// arising from the use of this software.
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:
// 1. The origin of this software must not be misrepresented; you must not
//    claim that you wrote the original software. If you use this software
//    in a product, an acknowledgment in the product documentation would be
//    appreciated but is not required.
// 2. Altered source versions must be plainly marked as such, and must not be
//    misrepresented as being the original software.
// 3. This notice may not be removed or altered from any source distribution.
//

#include <stdio.h>
#ifdef NANOVG_GLEW
#	include <GL/glew.h>
#endif
#ifdef __APPLE__
#	define GLFW_INCLUDE_GLCOREARB
#endif
#define GLFW_INCLUDE_GLEXT
#include <GLFW/glfw3.h>
#include "nanovg.h"
#define NANOVG_GL3_IMPLEMENTATION
#include "nanovg_gl.h"
#include "demo.h"
#include "perf.h"


void errorcb(int error, const char* desc)
{
	printf("GLFW error %d: %s\n", error, desc);
}

int blowup = 0;
int screenshot = 0;
int premult = 0;
// Display-list mode is on by default. Press D to compare vs pure immediate mode.
int useDisplayList = 1;
int forceRebuildDisplayList = 1;

static void key(GLFWwindow* window, int key, int scancode, int action, int mods)
{
	NVG_NOTUSED(scancode);
	NVG_NOTUSED(mods);
	if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS)
		glfwSetWindowShouldClose(window, GL_TRUE);
	if (key == GLFW_KEY_SPACE && action == GLFW_PRESS)
		blowup = !blowup;
	if (key == GLFW_KEY_S && action == GLFW_PRESS)
		screenshot = 1;
	if (key == GLFW_KEY_P && action == GLFW_PRESS)
		premult = !premult;
	if (key == GLFW_KEY_D && action == GLFW_PRESS) {
		useDisplayList = !useDisplayList;
		forceRebuildDisplayList = 1;
		printf("Display list: %s\n", useDisplayList ? "ON" : "OFF");
	}
	if (key == GLFW_KEY_R && action == GLFW_PRESS) {
		forceRebuildDisplayList = 1;
		printf("Display list rebuild requested\n");
	}
}

static void rebuildStaticDisplayList(NVGcontext* vg, NVGdrawList* dl,
	DemoData* data, float winWidth, float winHeight, float pxRatio)
{
	// Establish device pixel ratio / default state for tessellation, but do not
	// keep anything in the per-frame list while recording a retained draw list.
	nvgBeginFrame(vg, winWidth, winHeight, pxRatio);
	nvgBeginDisplayList(vg, dl); // Path 1 record → Path 2 IR
	renderDemoStatic(vg, winWidth, winHeight, data);
	nvgEndDisplayList(vg);
	nvgCancelFrame(vg);

	printf("Recorded static draw list (Path 2 IR): %d packets (pxRatio=%.2f, size=%.0fx%.0f)\n",
		nvgDrawListSize(dl), pxRatio, winWidth, winHeight);
}

int main()
{
	GLFWwindow* window;
	DemoData data;
	NVGcontext* vg = NULL;
	NVGdrawList* staticDemoList = NULL;
	GPUtimer gpuTimer;
	PerfGraph fps, cpuGraph, gpuGraph;
	double prevt = 0, cpuTime = 0;
	float lastPxRatio = 0.0f;
	int lastWinWidth = 0, lastWinHeight = 0;

	if (!glfwInit()) {
		printf("Failed to init GLFW.\n");
		return -1;
	}

	initGraph(&fps, GRAPH_RENDER_FPS, "Frame Time");
	initGraph(&cpuGraph, GRAPH_RENDER_MS, "CPU Time");
	initGraph(&gpuGraph, GRAPH_RENDER_MS, "GPU Time");

	glfwSetErrorCallback(errorcb);
#ifndef _WIN32 // don't require this on win32, and works with more cards
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
	glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
	glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#endif
	glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, 1);

#ifdef DEMO_MSAA
	glfwWindowHint(GLFW_SAMPLES, 4);
#endif
	window = glfwCreateWindow(1000, 600, "NanoVG DisplayList", NULL, NULL);
//	window = glfwCreateWindow(1000, 600, "NanoVG", glfwGetPrimaryMonitor(), NULL);
	if (!window) {
		glfwTerminate();
		return -1;
	}

	glfwSetKeyCallback(window, key);

	glfwMakeContextCurrent(window);
#ifdef NANOVG_GLEW
	glewExperimental = GL_TRUE;
	if(glewInit() != GLEW_OK) {
		printf("Could not init glew.\n");
		return -1;
	}
	// GLEW generates GL error because it calls glGetString(GL_EXTENSIONS), we'll consume it here.
	glGetError();
#endif

#ifdef DEMO_MSAA
	vg = nvgCreateGL3(NVG_STENCIL_STROKES | NVG_DEBUG);
#else
	vg = nvgCreateGL3(NVG_ANTIALIAS | NVG_STENCIL_STROKES | NVG_DEBUG);
#endif
	if (vg == NULL) {
		printf("Could not init nanovg.\n");
		return -1;
	}

	if (loadDemoData(vg, &data) == -1)
		return -1;

	staticDemoList = nvgCreateDrawList(vg);
	if (staticDemoList == NULL) {
		printf("Could not create draw list.\n");
		return -1;
	}

	printf("Controls: D=toggle retained draw list, R=rebuild list, SPACE=blowup, P=premult, S=screenshot, ESC=quit\n");
	printf("Path 2: all drawing goes through a tessellated draw-list IR.\n");
	printf("Retained static chrome starts ON; animated parts use the per-frame list.\n");

	glfwSwapInterval(0);

	initGPUTimer(&gpuTimer);

	glfwSetTime(0);
	prevt = glfwGetTime();

	while (!glfwWindowShouldClose(window))
	{
		double mx, my, t, dt;
		int winWidth, winHeight;
		int fbWidth, fbHeight;
		float pxRatio;
		float gpuTimes[3];
		int i, n;
		int needRebuild;

		t = glfwGetTime();
		dt = t - prevt;
		prevt = t;

		startGPUTimer(&gpuTimer);

		glfwGetCursorPos(window, &mx, &my);
		glfwGetWindowSize(window, &winWidth, &winHeight);
		glfwGetFramebufferSize(window, &fbWidth, &fbHeight);
		// Calculate pixel ration for hi-dpi devices.
		pxRatio = (float)fbWidth / (float)winWidth;

		// Update and render
		glViewport(0, 0, fbWidth, fbHeight);
		if (premult)
			glClearColor(0,0,0,0);
		else
			glClearColor(0.3f, 0.3f, 0.32f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);

		// Blowup applies a live transform over the widgets; fall back to immediate mode.
		needRebuild = forceRebuildDisplayList
			|| nvgDrawListNeedsRebuild(vg, staticDemoList)
			|| winWidth != lastWinWidth
			|| winHeight != lastWinHeight
			|| pxRatio != lastPxRatio;

		if (useDisplayList && !blowup && needRebuild) {
			rebuildStaticDisplayList(vg, staticDemoList, &data, (float)winWidth, (float)winHeight, pxRatio);
			forceRebuildDisplayList = 0;
			lastWinWidth = winWidth;
			lastWinHeight = winHeight;
			lastPxRatio = pxRatio;
		}

		nvgBeginFrame(vg, (float)winWidth, (float)winHeight, pxRatio);

		if (useDisplayList && !blowup) {
			// Path 2 retained submit (no retessellation, no geometry copy).
			nvgSubmitDrawList(vg, staticDemoList);
			// Dynamic content tessellates into the per-frame draw list.
			renderDemoDynamic(vg, (float)mx, (float)my, (float)winWidth, (float)winHeight, (float)t, &data);
		} else {
			// Everything immediate → still Path 2 per-frame IR, just no retained list.
			renderDemo(vg, (float)mx, (float)my, (float)winWidth, (float)winHeight, (float)t, blowup, &data);
		}

		renderGraph(vg, 5,5, &fps);
		renderGraph(vg, 5+200+5,5, &cpuGraph);
		if (gpuTimer.supported)
			renderGraph(vg, 5+200+5+200+5,5, &gpuGraph);

		// Small on-screen mode indicator (always immediate)
		{
			char status[128];
			snprintf(status, sizeof(status), "Retained:%s packets:%d  (D toggle, R rebuild)",
				(useDisplayList && !blowup) ? "ON" : "OFF",
				nvgDrawListSize(staticDemoList));
			nvgFontSize(vg, 16.0f);
			nvgFontFace(vg, "sans");
			nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
			nvgFillColor(vg, nvgRGBA(0, 0, 0, 160));
			nvgText(vg, 6, (float)winHeight - 22, status, NULL);
			nvgFillColor(vg, nvgRGBA(240, 240, 240, 220));
			nvgText(vg, 5, (float)winHeight - 23, status, NULL);
		}

		nvgEndFrame(vg);

		// Measure the CPU time taken excluding swap buffers (as the swap may wait for GPU)
		cpuTime = glfwGetTime() - t;

		updateGraph(&fps, (float)dt);
		updateGraph(&cpuGraph, (float)cpuTime);

		// We may get multiple results.
		n = stopGPUTimer(&gpuTimer, gpuTimes, 3);
		for (i = 0; i < n; i++)
			updateGraph(&gpuGraph, gpuTimes[i]);

		if (screenshot) {
			screenshot = 0;
			saveScreenShot(fbWidth, fbHeight, premult, "dump.png");
		}

		glfwSwapBuffers(window);
		glfwPollEvents();
	}

	nvgDeleteDrawList(vg, staticDemoList);
	freeDemoData(vg, &data);

	nvgDeleteGL3(vg);

	printf("Average Frame Time: %.2f ms\n", getGraphAverage(&fps) * 1000.0f);
	printf("          CPU Time: %.2f ms\n", getGraphAverage(&cpuGraph) * 1000.0f);
	printf("          GPU Time: %.2f ms\n", getGraphAverage(&gpuGraph) * 1000.0f);

	glfwTerminate();
	return 0;
}
