#define _GNU_SOURCE
#include <stdio.h>
#include <GLES3/gl3.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <sys/param.h>
#include <gbm.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <math.h>

#include <stdlib.h>

#include "rpihub75.h"
#include "util.h"

// Test Shader source code
const char *test_shader_source =
    "#version 310 es\n"
    "precision mediump float;\n"
    "out vec4 color;\n"
    "void main() {\n"
    "    color = vec4(1.0, 0.0, 1.0, 1.0);\n"  // Red color
    "}\n";

/**
 * @brief add inputs for shaderToy glsl shaders
 * usage: fragment_shader = sprintf(shadertoy_header, shader_source);
 */
const char *shadertoy_header =
    "#version 310 es\n"
    "precision mediump float;\n"
    "uniform vec3 iResolution;\n"
    "uniform float iGlobalTime;\n"
    "uniform vec4 iMouse;\n"
    "uniform vec4 iDate;\n"
    "uniform float iSampleRate;\n"
    "uniform vec3 iChannelResolution[4];\n"
    "uniform float iChannelTime[4];\n"
    "uniform float iTime;\n"

    "out vec4 fragColor;\n"
    "%s\n"

    "void main() {\n"
    "    mainImage(fragColor, gl_FragCoord.xy);\n"
    "}\n";


/**
 * @brief trivial vertex shader. pass vertex directly to the GPU
 * 
 */
const char *vertex_shader_source =
    "#version 310 es\n"
    "in vec4 position;\n"
    "void main() {\n"
    "    gl_Position = position;\n"
    "}\n";

/**
 * @brief helper method for compiling GLSL shaders
 * 
 * @param source the source code for the shader
 * @param shader_type one of GL_VERTEX_SHADER or GL_FRAGMENT_SHADER
 * @return GLuint reference to the created shader id
 */
static GLuint compile_shader(const char *source, const GLenum shader_type) {
    GLuint shader = glCreateShader(shader_type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);

    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char info_log[512];
        glGetShaderInfoLog(shader, 512, NULL, info_log);
        die("Shader compilation error: %s\n", info_log);
    }

    return shader;
}

/**
 * @brief Create a complete OpenGL program for a shadertoy shader
 * 
 * @param file name of the shadertoy file to load
 * @return GLuint OpenGL id of the new program
 */
static GLuint create_shadertoy_program(char *file) {
    long filesize;
    char *src = file_get_contents(file, &filesize);
    if (filesize == 0) {
        die( "Failed to read shader source\n");
    }

    char *src_with_header = (char *)malloc(filesize + 8192);
    if (src_with_header == NULL) {
        die("unable to allocate %d bytes memory for shader program\n", filesize + 8192);
    }
    snprintf(src_with_header, filesize + 8192, shadertoy_header, src);

    GLuint vertex_shader = compile_shader(vertex_shader_source, GL_VERTEX_SHADER);
    GLuint fragment_shader = compile_shader(src_with_header, GL_FRAGMENT_SHADER);

    GLuint program = glCreateProgram();
    glAttachShader(program, vertex_shader);
    glAttachShader(program, fragment_shader);
    glLinkProgram(program);

    GLint success;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        char info_log[512];
        glGetProgramInfoLog(program, 512, NULL, info_log);
        die("Program linking error: %s\n", info_log);
    }

    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);

    free(src_with_header);
    return program;
}


/**
 * @brief render the shadertoy compatible shader source code in the 
 * file pointed to at scene->shader_file
 * 
 * exits if shader is unable to be rendered
 * 
 * loop exits and memory is freed if/when scene->do_render becomes false
 * 
 * frame delay is adaptive and updates to current scene->fps on each frame update
 * 
 * 
 * @param arg pointer to the current scene_info object
 */
__attribute__((noreturn))
void *render_shader(void *arg) {
    const scene_info *scene = (scene_info*)arg;
    debug("render shader %s\n", scene->shader_file);

    // Open a file descriptor to the DRM device
    int fd = open("/dev/dri/card0", O_RDWR);
    if (fd < 0) {
        die("Failed to open DRM device /dev/dri/card0\n");
    }


	static const EGLint context_attribs[] = {
		EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE
	};

    // Create GBM device and surface
    struct gbm_device *gbm = gbm_create_device(fd);
    struct gbm_surface *surface = gbm_surface_create(gbm, scene->width, scene->height, GBM_FORMAT_XRGB8888, GBM_BO_USE_RENDERING);

    // Initialize EGL
    EGLDisplay display = eglGetDisplay(gbm);
    eglInitialize(display, NULL, NULL);
    eglBindAPI(EGL_OPENGL_ES_API);

    // Create EGL context and surface.
    // TODO experiment with 565 color
    EGLConfig config;
    EGLint num_configs;
    EGLint attribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    eglChooseConfig(display, attribs, &config, 1, &num_configs);

    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attribs);
    EGLSurface egl_surface = eglCreateWindowSurface(display, config, (EGLNativeWindowType)surface, NULL);
    eglMakeCurrent(display, egl_surface, egl_surface, context);

    // Set up OpenGL ES
    GLuint program = create_shadertoy_program(scene->shader_file);
    glUseProgram(program);

    // Define a square with two triangles. This is a rendering surface for our fragment shader
    GLfloat vertices[] = {
        -1.0f,  1.0f, 0.0f,
        -1.0f, -1.0f, 0.0f,
         1.0f,  1.0f, 0.0f,
         1.0f, -1.0f, 0.0f
    };

    GLuint vbo;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    GLint posAttrib = glGetAttribLocation(program, "position");
    glEnableVertexAttribArray(posAttrib);
    glVertexAttribPointer(posAttrib, 3, GL_FLOAT, GL_FALSE, 0, 0);


    // setup the timers for frame delays
    struct timespec start_time, end_time;
    uint32_t frame_time_us = 1000000 / scene->fps;
    size_t image_buf_sz = scene->width * (scene->height) * sizeof(uint32_t);

    // RGBA format (4 bytes per pixel)
    GLubyte *restrict pixelsA __attribute__((aligned(16))) = (GLubyte*)malloc(image_buf_sz*(MAX(scene->motion_blur_frames+1,10)));
    if (pixelsA == NULL) {
        die("unable to allocate %d bytes memory for shader frames...\n", image_buf_sz * (MAX(scene->motion_blur_frames+1,10)));
    }
    GLubyte *pixels = pixelsA;

    // pointer to the current motion blur buffer
    GLubyte *pixelsO = pixelsA+(image_buf_sz * scene->motion_blur_frames+1);

    // uniforms point to information we will pass to the GLSL shader
    GLint timeLocation = glGetUniformLocation(program, "iTime");
    GLint resLocation = glGetUniformLocation(program, "iResolution");


    // some variables for each frame iteration
    float motion_blur[scene->motion_blur_frames+1];
    float time = 0.0f;
    unsigned long frame= 0;
    int frame_num = 0;


    // calculate motion blur frame weights  (decreasing frame weights)
    float sum = 0;
    for (int i = 0; i < scene->motion_blur_frames; i++) {
        motion_blur[i] = powf(0.5f, i);  // Exponents symmetric around zero
        sum += motion_blur[i];
    }

    // now average each frame so that the sum of all frames adds to 1.0
    for (int i = 0; i < scene->motion_blur_frames; i++) {
        motion_blur[i] /= sum;
    }


    // loop until do_render is false. most likely never exit...
    while(scene->do_render) {
        frame++;
        time += 1.0f / scene->fps;
        clock_gettime(CLOCK_MONOTONIC, &start_time);
        glUseProgram(program);
        glUniform1f(timeLocation, time);
        glUniform3f(resLocation, scene->width, (scene->width), 0);

        // Render
        glViewport(0, 0, scene->width, scene->height);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        eglSwapBuffers(display, egl_surface);

        // switch between pixels buffers A-F based on frame number
        pixels = pixelsA + (frame_num * image_buf_sz);
        glReadPixels(0, 0, scene->width, scene->height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

        // apply motion blur in the CPU
        if (scene->motion_blur_frames > 0) {
            for (int i = 0; i < scene->width * scene->height * 4; i++) {
                float accum = 0;
                for (int f = 0; f < scene->motion_blur_frames; f++) {
                    GLubyte *frame_idx = pixelsA + (((f + frame_num) % scene->motion_blur_frames)* image_buf_sz);
                    accum += ((float)(frame_idx[i])) * motion_blur[(f + frame_num) % scene->motion_blur_frames];
                }

                pixelsO[i] = (uint8_t)(accum);
            }
            scene->pwm_mapper(pixelsO, scene, FALSE);
            frame_num = frame % scene->motion_blur_frames;
        }
        // skip motion blur ....
        else {
            scene->pwm_mapper(pixels, scene, FALSE);
        }


        // Get the end time for this frame
        clock_gettime(CLOCK_MONOTONIC, &end_time);

        // Calculate the time taken for the frame
        long frame_time = (end_time.tv_sec - start_time.tv_sec) * 1000000L +
                          (end_time.tv_nsec - start_time.tv_nsec) / 1000L;

        // Sleep for the remainder of the frame time to achieve 120 FPS
        if (frame_time < frame_time_us) {
            usleep(frame_time_us - frame_time);
        }

        calculate_fps(frame_time_us - frame_time);
    }


    // Cleanup
    glDeleteBuffers(1, &vbo);
    eglDestroySurface(display, egl_surface);
    eglDestroyContext(display, context);
    eglTerminate(display);
    gbm_surface_destroy(surface);
    gbm_device_destroy(gbm);

    free(pixelsA);
    close(fd);
}

