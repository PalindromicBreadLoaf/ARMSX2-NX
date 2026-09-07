// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glcorearb.h>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

GLuint compile(GLenum type, const std::string& code)
{
	GLuint shader = glCreateShader(type);
	const char* text = code.c_str();
	glShaderSource(shader, 1, &text, nullptr);
	glCompileShader(shader);
	GLint ok;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (!ok)
	{
		char log[8192];
		glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
		std::cerr << log;
		std::exit(2);
	}
	return shader;
}

GLuint program(const std::string& filename)
{
	std::ifstream file(filename);
	assert(file);
	const std::string fragment((std::istreambuf_iterator<char>(file)), {});
	const GLuint vs = compile(GL_VERTEX_SHADER, R"(#version 460
layout(location=0) out vec2 vTexCoord;
void main() {
    vec2 pos = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(pos * 2.0 - 1.0, 0.5, 1.0);
    vTexCoord = vec2(0.5);
})");
	const GLuint fs = compile(GL_FRAGMENT_SHADER, fragment);
	GLuint p = glCreateProgram();
	glAttachShader(p, vs);
	glAttachShader(p, fs);
	glLinkProgram(p);
	GLint ok;
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	assert(ok);
	glDeleteShader(vs);
	glDeleteShader(fs);
	return p;
}

int main(int argc, char** argv)
{
	assert(argc == 2);
	EGLDisplay display = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
	assert(eglInitialize(display, nullptr, nullptr));
	assert(eglBindAPI(EGL_OPENGL_API));
	const EGLint attr[] = {EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 6, EGL_NONE};
	EGLContext ctx = eglCreateContext(display, nullptr, EGL_NO_CONTEXT, attr);
	assert(ctx != EGL_NO_CONTEXT);
	assert(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx));
	std::cout << "Renderer: " << glGetString(GL_RENDERER) << '\n';
	const GLuint normal = program(std::string(argv[1]) + "/convert_fsh.glsl");
	const GLuint integer = program(std::string(argv[1]) + "/convert_int_fsh.glsl");
	GLuint vao, fbo, source, color, depth, ubo;
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glGenTextures(1, &source);
	glGenTextures(1, &color);
	glGenTextures(1, &depth);
	glBindTexture(GL_TEXTURE_2D, depth);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, 1, 1, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth, 0);
	glGenBuffers(1, &ubo);
	glBindBuffer(GL_UNIFORM_BUFFER, ubo);
	glBufferData(GL_UNIFORM_BUFFER, 256, nullptr, GL_DYNAMIC_DRAW);
	glBindBufferBase(GL_UNIFORM_BUFFER, 0, ubo);
	glViewport(0, 0, 1, 1);
	int failures = 0, cases = 0;
	using Pixel = std::array<float, 4>;
	auto run = [&](const char* name, uint32_t variant, const Pixel& input, const Pixel& expected,
				   bool is_integer = false, bool depth_output = false, bool bilinear = false,
				   const Pixel* four_pixels = nullptr) {
		++cases;
		glBindTexture(GL_TEXTURE_2D, color);
		glTexImage2D(GL_TEXTURE_2D, 0, is_integer ? GL_R32UI : GL_RGBA32F, 1, 1, 0,
			is_integer ? GL_RED_INTEGER : GL_RGBA, is_integer ? GL_UNSIGNED_INT : GL_FLOAT, nullptr);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
		assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, source);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, four_pixels ? 2 : 1, four_pixels ? 2 : 1,
			0, GL_RGBA, GL_FLOAT, four_pixels ? four_pixels : &input);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		uint32_t constants[4] = {variant, bilinear, 0, 0};
		glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(constants), constants);
		glUseProgram(is_integer ? integer : normal);
		if (depth_output)
		{
			glEnable(GL_DEPTH_TEST);
			glDepthFunc(GL_ALWAYS);
		}
		else
			glDisable(GL_DEPTH_TEST);
		glDrawArrays(GL_TRIANGLES, 0, 3);
		Pixel actual{};
		if (is_integer)
		{
			uint32_t v;
			glReadPixels(0, 0, 1, 1, GL_RED_INTEGER, GL_UNSIGNED_INT, &v);
			actual[0] = v;
		}
		else if (depth_output)
			glReadPixels(0, 0, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, actual.data());
		else
			glReadPixels(0, 0, 1, 1, GL_RGBA, GL_FLOAT, actual.data());
		assert(glGetError() == GL_NO_ERROR);
		const int channels = (is_integer || depth_output) ? 1 : 4;
		bool passed = true;
		for (int i = 0; i < channels; ++i)
		{
			const float tolerance = is_integer ? 0.0f : std::max(1e-11f, std::abs(expected[i]) * 2e-6f);
			passed &= std::abs(actual[i] - expected[i]) <= tolerance;
		}
		if (!passed)
		{
			++failures;
			std::cerr << "FAIL " << name << ": " << actual[0] << " expected " << expected[0] << '\n';
		}
	};
	const float scale = std::ldexp(1.0f, -32);
	const Pixel rgba{0x56 / 255.0f, 0x34 / 255.0f, 0x12 / 255.0f, 0};
	run("depth-copy", 1, {.375f, 0, 0, 0}, {.375f}, false, true);
	run("depth-copy to float color", 1, {.375f, 0, 0, 0}, {.375f, .375f, .375f, .375f});
	run("colclip-init", 7, rgba, {0x56 / 65535.0f, 0x34 / 65535.0f, 0x12 / 65535.0f, 0});
	run("colclip-resolve", 8, {342 / 65535.0f, 308 / 65535.0f, 274 / 65535.0f, 0}, rgba);
	run("rta-correction", 9, {.2f, .3f, .4f, .5f}, {.2f, .3f, .4f, .5f / (128.25f / 255)});
	run("rta-decorrection", 10, {.2f, .3f, .4f, .5f}, {.2f, .3f, .4f, .5f * (128.25f / 255)});
	run("transparency", 11, {.2f, .3f, .4f, .5f}, {.2f, .3f, .4f, 1});
	run("depth32-to-rgba8", 14, {0x123456 * scale, 0, 0, 0}, rgba);
	run("depth32-to-rgb8", 15, {0x123456 * scale, 0, 0, 0}, rgba);
	run("depth16-to-rgb5a1", 16, {0xffff * scale, 0, 0, 0}, {248 / 255.0f, 248 / 255.0f, 248 / 255.0f, 128 / 255.0f});
	run("rgba-to-depth32", 17, rgba, {0x123456 * scale}, false, true);
	run("rgba-to-depth24", 18, rgba, {0x123456 * scale}, false, true);
	run("rgba-to-depth16", 19, rgba, {0x3456 * scale}, false, true);
	run("rgb5a1-to-depth16", 20, {1, 1, 1, 1}, {0xffff * scale}, false, true);
	run("depth32-to-depth24", 21, {0xab123400u * scale, 0, 0, 0}, {0x123400 * scale}, false, true);
	run("rgba8-to-16-bits", 2, {1, 1, 1, 1}, {65535}, true);
	run("depth32-to-16-bits", 12, {0x123456 * scale, 0, 0, 0}, {0x3456}, true);
	run("depth32-to-32-bits", 13, {0x123456 * scale, 0, 0, 0}, {0x123456}, true);
	Pixel four[] = {{1, 0, 0, 0}, {0, 1 / 255.0f, 0, 0}, {1, 1 / 255.0f, 0, 0}, {0, 2 / 255.0f, 0, 0}};
	run("bilinear after depth conversion", 17, {}, {383.5f * scale}, false, true, true, four);
	std::cout << cases << " shader cases, " << failures << " failures\n";
	eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroyContext(display, ctx);
	eglTerminate(display);
	return failures ? 1 : 0;
}
