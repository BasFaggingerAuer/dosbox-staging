/*
 *  Copyright (C) 2002-2014  The DOSBox Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

/* $Id: sdlmain.cpp,v 1.154 2009-06-01 10:25:51 qbix79 Exp $ */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <stdarg.h>
#include <sys/types.h>
#ifdef WIN32
#include <signal.h>
#include <process.h>
#endif

#include "cross.h"
#include "SDL.h"

#include "dosbox.h"
#include "video.h"
#include "mouse.h"
#include "pic.h"
#include "timer.h"
#include "setup.h"
#include "support.h"
#include "debug.h"
#include "mapper.h"
#include "vga.h"
#include "keyboard.h"
#include "cpu.h"
#include "cross.h"
#include "control.h"

#define MAPPERFILE "mapper-" VERSION ".map"

//C_OPENGL will be the ONLY option.
#ifndef C_OPENGL
#error OpenGL support is required!
#endif

#include <GL/glew.h>
#include "SDL_opengl.h"

#if !(ENVIRON_INCLUDED)
extern char** environ;
#endif

#ifdef WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#define STDOUT_FILE    TEXT("stdout.txt")
#define STDERR_FILE    TEXT("stderr.txt")
#define DEFAULT_CONFIG_FILE "/dosbox.conf"
#elif defined(MACOSX)
#define DEFAULT_CONFIG_FILE "/Library/Preferences/DOSBox Preferences"
#else /*linux freebsd*/
#define DEFAULT_CONFIG_FILE "/.dosboxrc"
#endif

#if C_SET_PRIORITY
#include <sys/resource.h>
#define PRIO_TOTAL (PRIO_MAX-PRIO_MIN)
#endif

#ifdef OS2
#define INCL_DOS
#define INCL_WIN
#include <os2.h>
#endif

enum SCREEN_TYPES    {
    SCREEN_SURFACE,
    SCREEN_SURFACE_DDRAW,
    SCREEN_OVERLAY,
    SCREEN_OPENGL
};

enum PRIORITY_LEVELS {
    PRIORITY_LEVEL_PAUSE,
    PRIORITY_LEVEL_LOWEST,
    PRIORITY_LEVEL_LOWER,
    PRIORITY_LEVEL_NORMAL,
    PRIORITY_LEVEL_HIGHER,
    PRIORITY_LEVEL_HIGHEST
};


struct SDL_Block {
    bool inited;
    bool active;                            //If this isn't set don't draw
    bool updating;
    struct {
        Bit32u width;
        Bit32u height;
        Bit32u bpp;
        Bitu flags;
        double scalex,scaley;
        GFX_CallBack_t callback;
    } draw;
    bool wait_on_error;
    struct {
        struct {
            Bit16u width, height;
            bool fixed;
        } full;
        struct {
            Bit16u width, height;
        } window;
        Bit8u bpp;
        bool fullscreen;
        bool doublebuf;
        SCREEN_TYPES type;
        SCREEN_TYPES want_type;
    } desktop;
    struct {
        SDL_GLContext context;
        GLuint program;
        GLuint vertex_shader;
        GLuint fragment_shader;
        GLuint vertex_array;
        GLuint vertex_buffer;
        GLuint texture;
        GLint max_texsize;
        Bitu pitch;
        Bit8u *framebuf;
        bool bilinear;
    } opengl;
    struct {
        PRIORITY_LEVELS focus;
        PRIORITY_LEVELS nofocus;
    } priority;
    SDL_Rect clip;
    SDL_Window *window;
    SDL_cond *cond;
    struct {
        bool autolock;
        bool autoenable;
        bool requestlock;
        bool locked;
        Bitu sensitivity;
    } mouse;
    SDL_Rect updateRects[1024];
    Bitu num_joysticks;
#if defined (WIN32)
    // Time when sdl regains focus (alt-tab) in windowed mode
    Bit32u focus_ticks;
#endif
    // state of alt-keys for certain special handlings
    Bit32u laltstate;
    Bit32u raltstate;
};

static SDL_Block sdl;

extern const char* RunningProgram;
extern bool CPU_CycleAutoAdjust;
//Globals for keyboard initialisation
bool startup_state_numlock=false;
bool startup_state_capslock=false;

void GFX_SetTitle(Bit32s cycles,Bits frameskip,bool paused){
    char title[200]={0};
    static Bit32s internal_cycles=0;
    static Bits internal_frameskip=0;
    if(cycles != -1) internal_cycles = cycles;
    if(frameskip != -1) internal_frameskip = frameskip;
    if(CPU_CycleAutoAdjust) {
        sprintf(title,"GLDOSBox %s, Cpu speed: max %3d%% cycles, Frameskip %2d, Program: %8s",VERSION,internal_cycles,internal_frameskip,RunningProgram);
    } else {
        sprintf(title,"GLDOSBox %s, Cpu speed: %8d cycles, Frameskip %2d, Program: %8s",VERSION,internal_cycles,internal_frameskip,RunningProgram);
    }

    if(paused) strcat(title," PAUSED");
    SDL_SetWindowTitle(sdl.window, title);
}

static unsigned char logo[32*32*4]= {
#include "dosbox_logo.h"
};
static void GFX_SetIcon() {
#if !defined(MACOSX)
    /* Set Icon (must be done before any sdl_setvideomode call) */
    /* But don't set it on OS X, as we use a nicer external icon there. */
    /* Made into a separate call, so it can be called again when we restart the graphics output on win32 */
#ifdef WORDS_BIGENDIAN
    SDL_Surface* logos = SDL_CreateRGBSurfaceFrom((void*)logo,32,32,32,128,0xff000000,0x00ff0000,0x0000ff00,0);
#else
    SDL_Surface* logos = SDL_CreateRGBSurfaceFrom((void*)logo,32,32,32,128,0x000000ff,0x0000ff00,0x00ff0000,0);
#endif
    SDL_SetWindowIcon(sdl.window, logos);
#endif
}


static void KillSwitch(bool pressed) {
    if (!pressed)
        return;
    throw 1;
}

static void PauseDOSBox(bool pressed) {
    if (!pressed)
        return;
    GFX_SetTitle(-1,-1,true);
    bool paused = true;
    KEYBOARD_ClrBuffer();
    SDL_Delay(500);
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        // flush event queue.
    }

    while (paused) {
        SDL_WaitEvent(&event);    // since we're not polling, cpu usage drops to 0.
        switch (event.type) {

            case SDL_QUIT: KillSwitch(true); break;
            case SDL_KEYDOWN:   // Must use Pause/Break Key to resume.
            case SDL_KEYUP:
            if(event.key.keysym.sym == SDLK_PAUSE) {

                paused = false;
                GFX_SetTitle(-1,-1,false);
                break;
            }
#if defined (MACOSX)
            if (event.key.keysym.sym == SDLK_q && (event.key.keysym.mod == KMOD_RMETA || event.key.keysym.mod == KMOD_LMETA) ) {
                /* On macs, all aps exit when pressing cmd-q */
                KillSwitch(true);
                break;
            } 
#endif
        }
    }
}

/* Reset the screen with current values in the sdl structure */
Bitu GFX_GetBestMode(Bitu flags) {
    //We only accept 32bit output from the scalers here
    flags |= GFX_SCALING|GFX_CAN_32;
    flags &= ~(GFX_CAN_8|GFX_CAN_15|GFX_CAN_16);
    
    return flags;
}


void GFX_ResetScreen(void) {
    GFX_Stop();
    if (sdl.draw.callback)
        (sdl.draw.callback)( GFX_CallBackReset );
    GFX_Start();
    CPU_Reset_AutoAdjust();
}

static int int_log2 (int val) {
    int log = 0;
    
    while ((val >>= 1) != 0) log++;
    
    return log;
}

void GFX_Destroy() {
    //Destroy window and associated OpenGL data.
    if (sdl.window == NULL) {
        return;
    }
    
    if (sdl.opengl.framebuf != NULL) {
        delete [] sdl.opengl.framebuf;
        sdl.opengl.framebuf = NULL;
    }
    
    glDeleteProgram(sdl.opengl.program);
    glDeleteShader(sdl.opengl.vertex_shader);
    glDeleteShader(sdl.opengl.fragment_shader);
    glDeleteVertexArrays(1, &sdl.opengl.vertex_array);
    glDeleteBuffers(1, &sdl.opengl.vertex_buffer);
    glDeleteTextures(1, &sdl.opengl.texture);
    SDL_GL_DeleteContext(sdl.opengl.context);
    SDL_DestroyWindow(sdl.window);
    
    sdl.window = NULL;
}

GLuint GFX_CompileShader(const GLchar *code, const GLuint shader_type) {
    //Compile an OpenGL GLSL shader.
    GLuint shader = glCreateShader(shader_type);
    
    glShaderSource(shader, 1, &code, NULL);
    glCompileShader(shader);
    
    GLint is_compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &is_compiled);
    
    if (is_compiled != GL_TRUE) {
        //Shader compilation failed.
        GLint log_length = 0;
        
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_length);
        
        if (log_length > 0) {
            GLchar *log_text = new GLchar [log_length + 1];
        
            glGetShaderInfoLog(shader, log_length, 0, log_text);
            log_text[log_length] = 0;
            LOG_MSG("SDL:OPENGL: Unable to compile shader: %s\n", log_text);
            
            delete [] log_text;
        }
        
        E_Exit("Unable to compile shader.");
    }
    
    return shader;
}

void GFX_Create(Bitu width, Bitu height) {
    sdl.draw.width = width;
    sdl.draw.height = height;
    sdl.desktop.type = SCREEN_OPENGL;
    
    //Destroy window if it exists already.
    GFX_Destroy();

    //Use OpenGL 3.1 core profile.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    
    //Create SDL window.
    Uint32 sdl_flags = SDL_WINDOW_OPENGL;
    Bitu window_width = sdl.desktop.window.width;
    Bitu window_height = sdl.desktop.window.height;
    
    if (sdl.desktop.fullscreen) {
        sdl_flags |= SDL_WINDOW_FULLSCREEN;
        window_width = sdl.desktop.full.width;
        window_height = sdl.desktop.full.height;
    }
    
    LOG_MSG("SDL:OPENGL: Creating a %dx%d window...\n", window_width, window_height);
    
    sdl.window = SDL_CreateWindow("", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, window_width, window_height, sdl_flags);
    
    if (sdl.window == NULL) {
        LOG_MSG("SDL:OPENGL: Unable to create window: %s\n", SDL_GetError());
        E_Exit("Unable to create window!");
    }
    
    sdl.opengl.context = SDL_GL_CreateContext(sdl.window);
    
    if (sdl.opengl.context == NULL) {
        LOG_MSG("SDL:OPENGL: Unable to create OpenGL context: %s\n", SDL_GetError());
        E_Exit("Unable to create OpenGL context!");
    }
    
    //Initialize GLEW.
    glewExperimental = GL_TRUE; 
    GLenum glewError = glewInit();
    
    if (glewError != GLEW_OK) {
        LOG_MSG("SDL:OPENGL: Error initializing GLEW: %s\n", glewGetErrorString(glewError));
        E_Exit("Unable to initialize GLEW!");
    }
    
    //Enable adaptive vertical sync.
    if (SDL_GL_SetSwapInterval(-1) == -1) {
        LOG_MSG("SDL:OPENGL: Unable to enable adaptive vsync: %s\n", SDL_GetError());
    }
    
    //Initialize OpenGL.
    glClearColor(0.0, 0.0, 0.0, 1.0);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    
    //Allocate texture.
    glGenTextures(1, &sdl.opengl.texture);
    
    sdl.opengl.framebuf = NULL;
    
    LOG_MSG("SDL:OPENGL: Creating a %dx%d texture.\n", width, height);
    
    sdl.opengl.pitch = width*4;
    sdl.opengl.framebuf = new Bit8u [width*height*4];
    
    if (sdl.opengl.framebuf == NULL) {
        LOG_MSG("SDL:OPENGL: Unable to allocate framebuffer!");
        E_Exit("Unable to allocate framebuffer!");
    }

    glBindTexture(GL_TEXTURE_2D, sdl.opengl.texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, sdl.draw.width, sdl.draw.height, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    
    if (sdl.opengl.bilinear) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    } else {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    }
    
    glBindTexture(GL_TEXTURE_2D, 0);
    
    //Compile shaders.
    //TODO: Add configurable shaders.
    LOG_MSG("SDL:OPENGL: Compiling vertex shader...\n");
    sdl.opengl.vertex_shader = GFX_CompileShader("#version 140\n"
                                                 "\n"
                                                 "in vec2 vs_tex;\n"
                                                 "in vec2 vs_vertex;\n"
                                                 "\n"
                                                 "out vec2 fs_tex;\n"
                                                 "\n"
                                                 "void main() {\n"
                                                 "    fs_tex = vs_tex;\n"
                                                 "    gl_Position = vec4(vs_vertex.xy, 0.0f, 1.0f);\n"
                                                 "}\n", GL_VERTEX_SHADER);
    LOG_MSG("SDL:OPENGL: Compiling fragment shader...\n");
    sdl.opengl.fragment_shader = GFX_CompileShader("#version 140\n"
                                                   "\n"
                                                   "uniform sampler2D framebuffer;\n"
                                                   "uniform vec2 window_size;\n"
                                                   "uniform vec2 framebuffer_size;\n"
                                                   "\n"
                                                   "in vec2 fs_tex;\n"
                                                   "out vec4 fragment;\n"
                                                   "\n"
                                                   "void main() {\n"
                                                   "    fragment = vec4(texture(framebuffer, fs_tex).xyz, 1.0f);\n"
                                                   "}\n", GL_FRAGMENT_SHADER);
    
    //Link shaders in program.
    LOG_MSG("SDL:OPENGL: Linking OpenGL program...\n");
    sdl.opengl.program = glCreateProgram();
    glAttachShader(sdl.opengl.program, sdl.opengl.vertex_shader);
    glAttachShader(sdl.opengl.program, sdl.opengl.fragment_shader);
    glLinkProgram(sdl.opengl.program);
    
    GLint is_linked = GL_FALSE;
    
	glGetProgramiv(sdl.opengl.program, GL_LINK_STATUS, &is_linked);
    
    if (is_linked != GL_TRUE) {
        E_Exit("Unable to link OpenGL shader program!");
    }
    
    //Get shader parameter indices.
    const GLint glsl_tex_vertex_index = glGetAttribLocation(sdl.opengl.program, "vs_tex");
    const GLint glsl_vertex_index = glGetAttribLocation(sdl.opengl.program, "vs_vertex");
    const GLint glsl_texture_index = glGetUniformLocation(sdl.opengl.program, "framebuffer");
    const GLint glsl_ws_index = glGetUniformLocation(sdl.opengl.program, "window_size");
    const GLint glsl_fb_index = glGetUniformLocation(sdl.opengl.program, "framebuffer_size");
    
    if (glsl_vertex_index < 0 || glsl_tex_vertex_index < 0 || glsl_texture_index < 0) {
        E_Exit("Unable to find required variables in the OpenGL shader program!");
    }
    
    //Set texture index.
    glUseProgram(sdl.opengl.program);
    glUniform1i(glsl_texture_index, 0);
    glUniform2f(glsl_ws_index, window_width, window_height);
    glUniform2f(glsl_fb_index, sdl.draw.width, sdl.draw.height);
    glUseProgram(0);
    
    //Create vertex buffer object for the screen quad.
    const GLfloat vertex_data[] = {0.0f, 0.0f, -1.0f,  1.0f,
                                   0.0f, 1.0f, -1.0f, -1.0f,
                                   1.0f, 0.0f,  1.0f,  1.0f,
                                   1.0f, 1.0f,  1.0f, -1.0f};
    
    glGenBuffers(1, &sdl.opengl.vertex_buffer);
    glBindBuffer(GL_ARRAY_BUFFER, sdl.opengl.vertex_buffer);
    glBufferData(GL_ARRAY_BUFFER, 2*2*4*sizeof(GLfloat), vertex_data, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    
    glGenVertexArrays(1, &sdl.opengl.vertex_array);
    glBindVertexArray(sdl.opengl.vertex_array);
    glBindBuffer(GL_ARRAY_BUFFER, sdl.opengl.vertex_buffer);
    glEnableVertexAttribArray(glsl_tex_vertex_index);
    glVertexAttribPointer(glsl_tex_vertex_index, 2, GL_FLOAT, GL_FALSE, 4*sizeof(GLfloat), reinterpret_cast<void *>(0*sizeof(GLfloat)));
    glEnableVertexAttribArray(glsl_vertex_index);
    glVertexAttribPointer(glsl_vertex_index, 2, GL_FLOAT, GL_FALSE, 4*sizeof(GLfloat), reinterpret_cast<void *>(2*sizeof(GLfloat)));
    glBindVertexArray(0);
}

Bitu GFX_SetSize(Bitu width, Bitu height, Bitu flags, double scalex, double scaley, GFX_CallBack_t callback) {
    if (sdl.updating) {
        GFX_EndUpdate(0);
    }

    Bitu retFlags = GFX_CAN_32 | GFX_SCALING | GFX_HARDWARE;
    
    sdl.draw.width = width;
    sdl.draw.height = height;
    sdl.draw.scalex = scalex;
    sdl.draw.scaley = scaley;
    sdl.draw.callback = callback;
    
    //Update texture.
    LOG_MSG("SDL:OPENGL: Creating a %dx%d texture.\n", width, height);
    
    if (sdl.opengl.framebuf != NULL) {
        delete [] sdl.opengl.framebuf;
    }
    
    sdl.opengl.pitch = width*4;
    sdl.opengl.framebuf = new Bit8u [width*height*4];
    
    if (sdl.opengl.framebuf == NULL) {
        LOG_MSG("SDL:OPENGL: Unable to allocate framebuffer!");
        E_Exit("Unable to allocate framebuffer!");
    }

    glBindTexture(GL_TEXTURE_2D, sdl.opengl.texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, sdl.draw.width, sdl.draw.height, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
    glBindTexture(GL_TEXTURE_2D, 0);
    
    //Update framebuffer size in fragment shader.
    glUseProgram(sdl.opengl.program);
    glUniform2f(glGetUniformLocation(sdl.opengl.program, "framebuffer_size"), sdl.draw.width, sdl.draw.height);
    glUseProgram(0);
    
    //Start graphics back up.
    GFX_Start();
    
    if (!sdl.mouse.autoenable) {
        SDL_ShowCursor(sdl.mouse.autolock ? SDL_DISABLE : SDL_ENABLE);
    }
    
    return retFlags;
}

void GFX_CaptureMouse(void) {
    sdl.mouse.locked = !sdl.mouse.locked;
    
    if (sdl.mouse.locked) {
        SDL_SetWindowGrab(sdl.window, SDL_TRUE);
        SDL_ShowCursor(SDL_DISABLE);
    } else {
        SDL_SetWindowGrab(sdl.window, SDL_FALSE);
        
        if (sdl.mouse.autoenable || !sdl.mouse.autolock) {
            SDL_ShowCursor(SDL_ENABLE);
        }
    }
    
    mouselocked=sdl.mouse.locked;
}

bool mouselocked; //Global variable for mapper

static void CaptureMouse(bool pressed) {
    if (!pressed) {
        return;
    }
    
    GFX_CaptureMouse();
}

bool GFX_StartUpdate(Bit8u * & pixels,Bitu & pitch) {
    if (!sdl.active || sdl.updating) {
        return false;
    }
    
    pixels = sdl.opengl.framebuf;
    pitch = sdl.opengl.pitch;
    sdl.updating = true;
    
    return true;
}


void GFX_EndUpdate(const Bit16u *) {
    if (!sdl.updating) {
        return;
    }
    
    sdl.updating=false;
    
    //Update texture.
    glBindTexture(GL_TEXTURE_2D, sdl.opengl.texture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                    sdl.draw.width, sdl.draw.height, GL_BGRA,
                    GL_UNSIGNED_INT_8_8_8_8_REV, sdl.opengl.framebuf);
    glBindTexture(GL_TEXTURE_2D, 0);
    
    //Update screen.
    glClear(GL_COLOR_BUFFER_BIT);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sdl.opengl.texture);
    glUseProgram(sdl.opengl.program);
    glBindVertexArray(sdl.opengl.vertex_array);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    glUseProgram(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    
    SDL_GL_SwapWindow(sdl.window);
}


void GFX_SetPalette(Bitu start,Bitu count,GFX_PalEntry * entries) {
    E_Exit("SDL:Can't set palette");
}

Bitu GFX_GetRGB(Bit8u red,Bit8u green,Bit8u blue) {
    //Use BGRA.
    return ((blue << 0) | (green << 8) | (red << 16)) | (255 << 24);
}

void GFX_Stop() {
    if (sdl.updating) {
        GFX_EndUpdate(0);
    }
    
    sdl.active = false;
}

void GFX_Start() {
    sdl.active = true;
}

static void GUI_ShutDown(Section * /*sec*/) {
    GFX_Stop();
    if (sdl.draw.callback) (sdl.draw.callback)( GFX_CallBackStop );
    if (sdl.mouse.locked) GFX_CaptureMouse();
    GFX_Destroy();
}


static void SetPriority(PRIORITY_LEVELS level) {

#if C_SET_PRIORITY
// Do nothing if priorties are not the same and not root, else the highest
// priority can not be set as users can only lower priority (not restore it)

    if((sdl.priority.focus != sdl.priority.nofocus ) &&
        (getuid()!=0) ) return;

#endif
    switch (level) {
#ifdef WIN32
    case PRIORITY_LEVEL_PAUSE:    // if DOSBox is paused, assume idle priority
    case PRIORITY_LEVEL_LOWEST:
        SetPriorityClass(GetCurrentProcess(),IDLE_PRIORITY_CLASS);
        break;
    case PRIORITY_LEVEL_LOWER:
        SetPriorityClass(GetCurrentProcess(),BELOW_NORMAL_PRIORITY_CLASS);
        break;
    case PRIORITY_LEVEL_NORMAL:
        SetPriorityClass(GetCurrentProcess(),NORMAL_PRIORITY_CLASS);
        break;
    case PRIORITY_LEVEL_HIGHER:
        SetPriorityClass(GetCurrentProcess(),ABOVE_NORMAL_PRIORITY_CLASS);
        break;
    case PRIORITY_LEVEL_HIGHEST:
        SetPriorityClass(GetCurrentProcess(),HIGH_PRIORITY_CLASS);
        break;
#elif C_SET_PRIORITY
/* Linux use group as dosbox has mulitple threads under linux */
    case PRIORITY_LEVEL_PAUSE:    // if DOSBox is paused, assume idle priority
    case PRIORITY_LEVEL_LOWEST:
        setpriority (PRIO_PGRP, 0,PRIO_MAX);
        break;
    case PRIORITY_LEVEL_LOWER:
        setpriority (PRIO_PGRP, 0,PRIO_MAX-(PRIO_TOTAL/3));
        break;
    case PRIORITY_LEVEL_NORMAL:
        setpriority (PRIO_PGRP, 0,PRIO_MAX-(PRIO_TOTAL/2));
        break;
    case PRIORITY_LEVEL_HIGHER:
        setpriority (PRIO_PGRP, 0,PRIO_MAX-((3*PRIO_TOTAL)/5) );
        break;
    case PRIORITY_LEVEL_HIGHEST:
        setpriority (PRIO_PGRP, 0,PRIO_MAX-((3*PRIO_TOTAL)/4) );
        break;
#endif
    default:
        break;
    }
}

extern Bit8u int10_font_14[256 * 14];
static void OutputString(Bitu x,Bitu y,const char * text,Bit32u color,Bit32u color2,SDL_Surface * output_surface) {
    Bit32u * draw=(Bit32u*)(((Bit8u *)output_surface->pixels)+((y)*output_surface->pitch))+x;
    while (*text) {
        Bit8u * font=&int10_font_14[(*text)*14];
        Bitu i,j;
        Bit32u * draw_line=draw;
        for (i=0;i<14;i++) {
            Bit8u map=*font++;
            for (j=0;j<8;j++) {
                if (map & 0x80) *((Bit32u*)(draw_line+j))=color; else *((Bit32u*)(draw_line+j))=color2;
                map<<=1;
            }
            draw_line+=output_surface->pitch/4;
        }
        text++;
        draw+=8;
    }
}

#include "dosbox_splash.h"

//extern void UI_Run(bool);
static void GUI_StartUp(Section * sec) {
    sec->AddDestroyFunction(&GUI_ShutDown);
    Section_prop * section=static_cast<Section_prop *>(sec);
    sdl.active=false;
    sdl.updating=false;

    GFX_SetIcon();

    sdl.desktop.fullscreen=section->Get_bool("fullscreen");
    sdl.wait_on_error=section->Get_bool("waitonerror");

    Prop_multival* p=section->Get_multival("priority");
    std::string focus = p->GetSection()->Get_string("active");
    std::string notfocus = p->GetSection()->Get_string("inactive");

    if      (focus == "lowest")  { sdl.priority.focus = PRIORITY_LEVEL_LOWEST;  }
    else if (focus == "lower")   { sdl.priority.focus = PRIORITY_LEVEL_LOWER;   }
    else if (focus == "normal")  { sdl.priority.focus = PRIORITY_LEVEL_NORMAL;  }
    else if (focus == "higher")  { sdl.priority.focus = PRIORITY_LEVEL_HIGHER;  }
    else if (focus == "highest") { sdl.priority.focus = PRIORITY_LEVEL_HIGHEST; }

    if      (notfocus == "lowest")  { sdl.priority.nofocus=PRIORITY_LEVEL_LOWEST;  }
    else if (notfocus == "lower")   { sdl.priority.nofocus=PRIORITY_LEVEL_LOWER;   }
    else if (notfocus == "normal")  { sdl.priority.nofocus=PRIORITY_LEVEL_NORMAL;  }
    else if (notfocus == "higher")  { sdl.priority.nofocus=PRIORITY_LEVEL_HIGHER;  }
    else if (notfocus == "highest") { sdl.priority.nofocus=PRIORITY_LEVEL_HIGHEST; }
    else if (notfocus == "pause")   {
        /* we only check for pause here, because it makes no sense
         * for DOSBox to be paused while it has focus
         */
        sdl.priority.nofocus=PRIORITY_LEVEL_PAUSE;
    }

    SetPriority(sdl.priority.focus); //Assume focus on startup
    sdl.mouse.locked=false;
    mouselocked=false; //Global for mapper
    sdl.mouse.requestlock=false;
    sdl.desktop.full.fixed=false;
    const char* fullresolution=section->Get_string("fullresolution");
    sdl.desktop.full.width  = 0;
    sdl.desktop.full.height = 0;
    if(fullresolution && *fullresolution) {
        char res[100];
        safe_strncpy( res, fullresolution, sizeof( res ));
        fullresolution = lowcase (res);//so x and X are allowed
        if (strcmp(fullresolution,"original")) {
            sdl.desktop.full.fixed = true;
            if (strcmp(fullresolution,"desktop")) { //desktop = 0x0
                char* height = const_cast<char*>(strchr(fullresolution,'x'));
                if (height && * height) {
                    *height = 0;
                    sdl.desktop.full.height = (Bit16u)atoi(height+1);
                    sdl.desktop.full.width  = (Bit16u)atoi(res);
                }
            }
        }
    }

    sdl.desktop.window.width  = 640;
    sdl.desktop.window.height = 480;
    const char* windowresolution=section->Get_string("windowresolution");
    if(windowresolution && *windowresolution) {
        char res[100];
        safe_strncpy( res,windowresolution, sizeof( res ));
        windowresolution = lowcase (res);//so x and X are allowed
        if(strcmp(windowresolution,"original")) {
            char* height = const_cast<char*>(strchr(windowresolution,'x'));
            if(height && *height) {
                *height = 0;
                sdl.desktop.window.height = (Bit16u)atoi(height+1);
                sdl.desktop.window.width  = (Bit16u)atoi(res);
            }
        }
    }
    sdl.desktop.doublebuf=section->Get_bool("fulldouble");
    
    //Detect desktop size fully via SDL to limit code clutter.
    if (!sdl.desktop.full.width || !sdl.desktop.full.height) {
        SDL_DisplayMode dm;
        
        SDL_GetCurrentDisplayMode(0, &dm);
        
        sdl.desktop.full.width = dm.w;
        sdl.desktop.full.height = dm.h;
    }
    
    if (!sdl.desktop.full.width || !sdl.desktop.full.height) {
        LOG_MSG("Your fullscreen resolution can NOT be determined, it's assumed to be 1024x768.\nPlease edit the configuration file if this value is wrong.");
        sdl.desktop.full.width=1024;
        sdl.desktop.full.height=768;
    }
    
    sdl.mouse.autoenable=section->Get_bool("autolock");
    if (!sdl.mouse.autoenable) SDL_ShowCursor(SDL_DISABLE);
    sdl.mouse.autolock=false;
    sdl.mouse.sensitivity=section->Get_int("sensitivity");
    std::string output=section->Get_string("output");

    /* Setup Mouse correctly if fullscreen */
    if(sdl.desktop.fullscreen) GFX_CaptureMouse();
    
    if (output == "opengl") {
        sdl.desktop.want_type=SCREEN_OPENGL;
        sdl.opengl.bilinear=true;
    } else if (output == "openglnb") {
        sdl.desktop.want_type=SCREEN_OPENGL;
        sdl.opengl.bilinear=false;
    } else {
        LOG_MSG("SDL:Unsupported output device %s, switching back to opengl",output.c_str());
        sdl.desktop.want_type=SCREEN_OPENGL;
        sdl.opengl.bilinear=true;
    }

    /* Initialize screen for first time */
    GFX_Create(640, 400);
    GFX_Stop();
    GFX_SetTitle(0, 0, true);

/* The endian part is intentionally disabled as somehow it produces correct results without according to rhoenie*/
//#if SDL_BYTEORDER == SDL_BIG_ENDIAN
//    Bit32u rmask = 0xff000000;
//    Bit32u gmask = 0x00ff0000;
//    Bit32u bmask = 0x0000ff00;
//#else
    Bit32u rmask = 0x000000ff;
    Bit32u gmask = 0x0000ff00;
    Bit32u bmask = 0x00ff0000;
//#endif
    
    //TODO: Add back splash screen.

    /* Get some Event handlers */
    MAPPER_AddHandler(KillSwitch,MK_f9,MMOD1,"shutdown","ShutDown");
    MAPPER_AddHandler(CaptureMouse,MK_f10,MMOD1,"capmouse","Cap Mouse");
#if C_DEBUG
    /* Pause binds with activate-debugger */
#else
    MAPPER_AddHandler(&PauseDOSBox, MK_pause, MMOD2, "pause", "Pause");
#endif
    /* Get Keyboard state of numlock and capslock */
    SDL_Keymod keystate = SDL_GetModState();
    if(keystate&KMOD_NUM) startup_state_numlock = true;
    if(keystate&KMOD_CAPS) startup_state_capslock = true;
}

void Mouse_AutoLock(bool enable) {
    sdl.mouse.autolock=enable;
    if (sdl.mouse.autoenable) sdl.mouse.requestlock=enable;
    else {
        SDL_ShowCursor(enable?SDL_DISABLE:SDL_ENABLE);
        sdl.mouse.requestlock=false;
    }
}

static void HandleMouseMotion(SDL_MouseMotionEvent * motion) {
    if (sdl.mouse.locked || !sdl.mouse.autoenable)
        Mouse_CursorMoved((float)motion->xrel*sdl.mouse.sensitivity/100.0f,
                          (float)motion->yrel*sdl.mouse.sensitivity/100.0f,
                          (float)(motion->x-sdl.clip.x)/(sdl.clip.w-1)*sdl.mouse.sensitivity/100.0f,
                          (float)(motion->y-sdl.clip.y)/(sdl.clip.h-1)*sdl.mouse.sensitivity/100.0f,
                          sdl.mouse.locked);
}

static void HandleMouseButton(SDL_MouseButtonEvent * button) {
    switch (button->state) {
    case SDL_PRESSED:
        if (sdl.mouse.requestlock && !sdl.mouse.locked) {
            GFX_CaptureMouse();
            // Dont pass klick to mouse handler
            break;
        }
        if (!sdl.mouse.autoenable && sdl.mouse.autolock && button->button == SDL_BUTTON_MIDDLE) {
            GFX_CaptureMouse();
            break;
        }
        switch (button->button) {
        case SDL_BUTTON_LEFT:
            Mouse_ButtonPressed(0);
            break;
        case SDL_BUTTON_RIGHT:
            Mouse_ButtonPressed(1);
            break;
        case SDL_BUTTON_MIDDLE:
            Mouse_ButtonPressed(2);
            break;
        }
        break;
    case SDL_RELEASED:
        switch (button->button) {
        case SDL_BUTTON_LEFT:
            Mouse_ButtonReleased(0);
            break;
        case SDL_BUTTON_RIGHT:
            Mouse_ButtonReleased(1);
            break;
        case SDL_BUTTON_MIDDLE:
            Mouse_ButtonReleased(2);
            break;
        }
        break;
    }
}

void GFX_LosingFocus(void) {
    sdl.laltstate=SDL_KEYUP;
    sdl.raltstate=SDL_KEYUP;
    MAPPER_LosingFocus();
}

#if defined(MACOSX)
#define DB_POLLSKIP 3
#else
//Not used yet, see comment below
#define DB_POLLSKIP 1
#endif

#if defined(LINUX)
#define SDL_XORG_FIX 1
#else
#define SDL_XORG_FIX 0
#endif

void GFX_Events() {
    //Don't poll too often. This can be heavy on the OS, especially Macs.
    //In idle mode 3000-4000 polls are done per second without this check.
    //Macs, with this code,  max 250 polls per second. (non-macs unused default max 500)
    //Currently not implemented for all platforms, given the ALT-TAB stuff for WIN32.
#if defined (MACOSX)
    static int last_check = 0;
    int current_check = GetTicks();
    if (current_check - last_check <=  DB_POLLSKIP) return;
    last_check = current_check;
#endif

    SDL_Event event;
#if defined (REDUCE_JOYSTICK_POLLING)
    static int poll_delay = 0;
    int time = GetTicks();
    if (time - poll_delay > 20) {
        poll_delay = time;
        if (sdl.num_joysticks > 0) SDL_JoystickUpdate();
        MAPPER_UpdateJoysticks();
    }
#endif
    while (SDL_PollEvent(&event)) {
        //TODO: Check for Xorg 1.20.1 mouse grabbing issues.
        switch (event.type) {
        case SDL_WINDOWEVENT:
            if (event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
#ifdef WIN32
                if (!sdl.desktop.fullscreen) sdl.focus_ticks = GetTicks();
#endif
                if (sdl.desktop.fullscreen && !sdl.mouse.locked)
                    GFX_CaptureMouse();
                SetPriority(sdl.priority.focus);
                CPU_Disable_SkipAutoAdjust();
            }
            if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                if (sdl.mouse.locked) {
#ifdef WIN32
                    if (sdl.desktop.fullscreen) {
                        VGA_KillDrawing();
                        sdl.desktop.fullscreen=false;
                        GFX_ResetScreen();
                    }
#endif
                    GFX_CaptureMouse();
                }
                SetPriority(sdl.priority.nofocus);
                GFX_LosingFocus();
                CPU_Enable_SkipAutoAdjust();
            }

            /* Non-focus priority is set to pause; check to see if we've lost window or input focus
             * i.e. has the window been minimised or made inactive?
             */
            if (sdl.priority.nofocus == PRIORITY_LEVEL_PAUSE) {
                if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                    /* Window has lost focus, pause the emulator.
                     * This is similar to what PauseDOSBox() does, but the exit criteria is different.
                     * Instead of waiting for the user to hit Alt-Break, we wait for the window to
                     * regain window or input focus.
                     */
                    bool paused = true;
                    SDL_Event ev;

                    GFX_SetTitle(-1,-1,true);
                    KEYBOARD_ClrBuffer();
//                    SDL_Delay(500);
//                    while (SDL_PollEvent(&ev)) {
                        // flush event queue.
//                    }

                    while (paused) {
                        // WaitEvent waits for an event rather than polling, so CPU usage drops to zero
                        SDL_WaitEvent(&ev);

                        switch (ev.type) {
                        case SDL_QUIT: throw(0); break; // a bit redundant at linux at least as the active events gets before the quit event.
                        case SDL_WINDOWEVENT:     // wait until we get window focus back
                            if (event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                                // We've got focus back, so unpause and break out of the loop
                                paused = false;
                                GFX_SetTitle(-1,-1,false);

                                /* Now poke a "release ALT" command into the keyboard buffer
                                 * we have to do this, otherwise ALT will 'stick' and cause
                                 * problems with the app running in the DOSBox.
                                 */
                                KEYBOARD_AddKey(KBD_leftalt, false);
                                KEYBOARD_AddKey(KBD_rightalt, false);
                            }
                            break;
                        }
                    }
                }
            }
            break;
        case SDL_MOUSEMOTION:
            HandleMouseMotion(&event.motion);
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            HandleMouseButton(&event.button);
            break;
        case SDL_QUIT:
            throw(0);
            break;
#ifdef WIN32
        case SDL_KEYDOWN:
        case SDL_KEYUP:
            // ignore event alt+tab
            if (event.key.keysym.sym==SDLK_LALT) sdl.laltstate = event.key.type;
            if (event.key.keysym.sym==SDLK_RALT) sdl.raltstate = event.key.type;
            if (((event.key.keysym.sym==SDLK_TAB)) &&
                ((sdl.laltstate==SDL_KEYDOWN) || (sdl.raltstate==SDL_KEYDOWN))) break;
            // This can happen as well.
            if (((event.key.keysym.sym == SDLK_TAB )) && (event.key.keysym.mod & KMOD_ALT)) break;
            // ignore tab events that arrive just after regaining focus. (likely the result of alt-tab)
            if ((event.key.keysym.sym == SDLK_TAB) && (GetTicks() - sdl.focus_ticks < 2)) break;
#endif
#if defined (MACOSX)            
        case SDL_KEYDOWN:
        case SDL_KEYUP:
            /* On macs CMD-Q is the default key to close an application */
            if (event.key.keysym.sym == SDLK_q && (event.key.keysym.mod == KMOD_RMETA || event.key.keysym.mod == KMOD_LMETA) ) {
                KillSwitch(true);
                break;
            } 
#endif
        default:
            void MAPPER_CheckEvent(SDL_Event * event);
            MAPPER_CheckEvent(&event);
        }
    }
}

#if defined (WIN32)
static BOOL WINAPI ConsoleEventHandler(DWORD event) {
    switch (event) {
    case CTRL_SHUTDOWN_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_BREAK_EVENT:
        raise(SIGTERM);
        return TRUE;
    case CTRL_C_EVENT:
    default: //pass to the next handler
        return FALSE;
    }
}
#endif


/* static variable to show wether there is not a valid stdout.
 * Fixes some bugs when -noconsole is used in a read only directory */
static bool no_stdout = false;
void GFX_ShowMsg(char const* format,...) {
    char buf[512];
    va_list msg;
    va_start(msg,format);
    vsprintf(buf,format,msg);
        strcat(buf,"\n");
    va_end(msg);
    if(!no_stdout) printf("%s",buf); //Else buf is parsed again.
}


void Config_Add_SDL() {
    Section_prop * sdl_sec=control->AddSection_prop("sdl",&GUI_StartUp);
    sdl_sec->AddInitFunction(&MAPPER_StartUp);
    Prop_bool* Pbool;
    Prop_string* Pstring;
    Prop_int* Pint;
    Prop_multival* Pmulti;

    Pbool = sdl_sec->Add_bool("fullscreen",Property::Changeable::Always,false);
    Pbool->Set_help("Start dosbox directly in fullscreen. (Press ALT-Enter to go back)");
     
    Pbool = sdl_sec->Add_bool("fulldouble",Property::Changeable::Always,false);
    Pbool->Set_help("Use double buffering in fullscreen. It can reduce screen flickering, but it can also result in a slow DOSBox.");

    Pstring = sdl_sec->Add_string("fullresolution",Property::Changeable::Always,"original");
    Pstring->Set_help("What resolution to use for fullscreen: original, desktop or fixed size (e.g. 1024x768).\n"
                      "  Using your monitor's native resolution (desktop) with aspect=true might give the best results.\n"
              "  If you end up with small window on a large screen, try an output different from surface.\n"
                      "  On Windows 10 with display scaling (Scale and layout) set to a value above 100%, it is recommended\n"
                      "  to use a lower full/windowresolution, in order to avoid window size problems.");
    Pstring = sdl_sec->Add_string("windowresolution",Property::Changeable::Always,"original");
    Pstring->Set_help("Scale the window to this size IF the output device supports hardware scaling.\n"
                      "  (output=surface does not!)");

    const char* outputs[] = {
#if C_OPENGL
        "opengl", "openglnb",
#endif
        0 };
    Pstring = sdl_sec->Add_string("output",Property::Changeable::Always,"opengl");
    Pstring->Set_help("What video system to use for output.");
    Pstring->Set_values(outputs);
    
    Pstring = sdl_sec->Add_string("vertexshader",Property::Changeable::Always,"");
    Pstring->Set_help("Full filename of OpenGL vertex shader to use. Leave empty for default shader.");

    Pstring = sdl_sec->Add_string("fragmentshader",Property::Changeable::Always,"");
    Pstring->Set_help("Full filename of OpenGL fragment shader to use. Leave empty for default shader.");

    Pbool = sdl_sec->Add_bool("autolock",Property::Changeable::Always,true);
    Pbool->Set_help("Mouse will automatically lock, if you click on the screen. (Press CTRL-F10 to unlock)");

    Pint = sdl_sec->Add_int("sensitivity",Property::Changeable::Always,100);
    Pint->SetMinMax(1,1000);
    Pint->Set_help("Mouse sensitivity.");

    Pbool = sdl_sec->Add_bool("waitonerror",Property::Changeable::Always, true);
    Pbool->Set_help("Wait before closing the console if dosbox has an error.");

    Pmulti = sdl_sec->Add_multi("priority", Property::Changeable::Always, ",");
    Pmulti->SetValue("higher,normal");
    Pmulti->Set_help("Priority levels for dosbox. Second entry behind the comma is for when dosbox is not focused/minimized.\n"
                     "  pause is only valid for the second entry.");

    const char* actt[] = { "lowest", "lower", "normal", "higher", "highest", "pause", 0};
    Pstring = Pmulti->GetSection()->Add_string("active",Property::Changeable::Always,"higher");
    Pstring->Set_values(actt);

    const char* inactt[] = { "lowest", "lower", "normal", "higher", "highest", "pause", 0};
    Pstring = Pmulti->GetSection()->Add_string("inactive",Property::Changeable::Always,"normal");
    Pstring->Set_values(inactt);

    Pstring = sdl_sec->Add_path("mapperfile",Property::Changeable::Always,MAPPERFILE);
    Pstring->Set_help("File used to load/save the key/event mappings from. Resetmapper only works with the defaul value.");
}

static void launcheditor() {
    std::string path,file;
    Cross::CreatePlatformConfigDir(path);
    Cross::GetPlatformConfigName(file);
    path += file;
    FILE* f = fopen(path.c_str(),"r");
    if(!f && !control->PrintConfig(path.c_str())) {
        printf("tried creating %s. but failed.\n",path.c_str());
        exit(1);
    }
    if(f) fclose(f);
/*    if(edit.empty()) {
        printf("no editor specified.\n");
        exit(1);
    }*/
    std::string edit;
    while(control->cmdline->FindString("-editconf",edit,true)) //Loop until one succeeds
        execlp(edit.c_str(),edit.c_str(),path.c_str(),(char*) 0);
    //if you get here the launching failed!
    printf("can't find editor(s) specified at the command line.\n");
    exit(1);
}

static void launchcaptures(std::string const& edit) {
    std::string path,file;
    Section* t = control->GetSection("dosbox");
    if(t) file = t->GetPropValue("captures");
    if(!t || file == NO_SUCH_PROPERTY) {
        printf("Config system messed up.\n");
        exit(1);
    }
    Cross::CreatePlatformConfigDir(path);
    path += file;
    Cross::CreateDir(path);
    struct stat cstat;
    if(stat(path.c_str(),&cstat) || (cstat.st_mode & S_IFDIR) == 0) {
        printf("%s doesn't exists or isn't a directory.\n",path.c_str());
        exit(1);
    }
/*    if(edit.empty()) {
        printf("no editor specified.\n");
        exit(1);
    }*/

    execlp(edit.c_str(),edit.c_str(),path.c_str(),(char*) 0);
    //if you get here the launching failed!
    printf("can't find filemanager %s\n",edit.c_str());
    exit(1);
}

static void printconfiglocation() {
    std::string path,file;
    Cross::CreatePlatformConfigDir(path);
    Cross::GetPlatformConfigName(file);
    path += file;
     
    FILE* f = fopen(path.c_str(),"r");
    if(!f && !control->PrintConfig(path.c_str())) {
        printf("tried creating %s. but failed",path.c_str());
        exit(1);
    }
    if(f) fclose(f);
    printf("%s\n",path.c_str());
    exit(0);
}

static void eraseconfigfile() {
    FILE* f = fopen("dosbox.conf","r");
    if(f) {
        fclose(f);
        LOG_MSG("Warning: dosbox.conf exists in current working directory.\nThis will override the configuration file at runtime.\n");
    }
    std::string path,file;
    Cross::GetPlatformConfigDir(path);
    Cross::GetPlatformConfigName(file);
    path += file;
    f = fopen(path.c_str(),"r");
    if(!f) exit(0);
    fclose(f);
    unlink(path.c_str());
    exit(0);
}

static void erasemapperfile() {
    FILE* g = fopen("dosbox.conf","r");
    if(g) {
        fclose(g);
        LOG_MSG("Warning: dosbox.conf exists in current working directory.\nKeymapping might not be properly reset.\n"
                "Please reset configuration as well and delete the dosbox.conf.\n");
    }

    std::string path,file=MAPPERFILE;
    Cross::GetPlatformConfigDir(path);
    path += file;
    FILE* f = fopen(path.c_str(),"r");
    if(!f) exit(0);
    fclose(f);
    unlink(path.c_str());
    exit(0);
}



//extern void UI_Init(void);
int main(int argc, char* argv[]) {
    try {
        CommandLine com_line(argc,argv);
        Config myconf(&com_line);
        control=&myconf;
        /* Init the configuration system and add default values */
        Config_Add_SDL();
        DOSBOX_Init();

        std::string editor;
        if(control->cmdline->FindString("-editconf",editor,false)) launcheditor();
        if(control->cmdline->FindString("-opencaptures",editor,true)) launchcaptures(editor);
        if(control->cmdline->FindExist("-eraseconf")) eraseconfigfile();
        if(control->cmdline->FindExist("-resetconf")) eraseconfigfile();
        if(control->cmdline->FindExist("-erasemapper")) erasemapperfile();
        if(control->cmdline->FindExist("-resetmapper")) erasemapperfile();

        /* Can't disable the console with debugger enabled */
#if defined(WIN32) && !(C_DEBUG)
        if (control->cmdline->FindExist("-noconsole")) {
            FreeConsole();
            /* Redirect standard input and standard output */
            if(freopen(STDOUT_FILE, "w", stdout) == NULL)
                no_stdout = true; // No stdout so don't write messages
            freopen(STDERR_FILE, "w", stderr);
            setvbuf(stdout, NULL, _IOLBF, BUFSIZ);    /* Line buffered */
            setbuf(stderr, NULL);                    /* No buffering */
        } else {
            if (AllocConsole()) {
                fclose(stdin);
                fclose(stdout);
                fclose(stderr);
                freopen("CONIN$","r",stdin);
                freopen("CONOUT$","w",stdout);
                freopen("CONOUT$","w",stderr);
            }
            SetConsoleTitle("DOSBox Status Window");
        }
#endif  //defined(WIN32) && !(C_DEBUG)
        if (control->cmdline->FindExist("-version") ||
            control->cmdline->FindExist("--version") ) {
            printf("\nDOSBox version %s, copyright 2002-2019 DOSBox Team.\n\n",VERSION);
            printf("DOSBox is written by the DOSBox Team (See AUTHORS file))\n");
            printf("DOSBox comes with ABSOLUTELY NO WARRANTY.  This is free software,\n");
            printf("and you are welcome to redistribute it under certain conditions;\n");
            printf("please read the COPYING file thoroughly before doing so.\n\n");
            return 0;
        }
        if(control->cmdline->FindExist("-printconf")) printconfiglocation();

#if C_DEBUG
        DEBUG_SetupConsole();
#endif

#if defined(WIN32)
    SetConsoleCtrlHandler((PHANDLER_ROUTINE) ConsoleEventHandler,TRUE);
#endif

#ifdef OS2
        PPIB pib;
        PTIB tib;
        DosGetInfoBlocks(&tib, &pib);
        if (pib->pib_ultype == 2) pib->pib_ultype = 3;
        setbuf(stdout, NULL);
        setbuf(stderr, NULL);
#endif

    /* Display Welcometext in the console */
    LOG_MSG("GLDOSBox version %s",VERSION);
    LOG_MSG("Copyright 2002-2019 DOSBox Team, published under GNU GPL.");
    LOG_MSG("---");

    /* Init SDL */
#if SDL_VERSION_ATLEAST(1, 2, 14)
    /* Or debian/ubuntu with older libsdl version as they have done this themselves, but then differently.
     * with this variable they will work correctly. I've only tested the 1.2.14 behaviour against the windows version
     * of libsdl
     */
    putenv(const_cast<char*>("SDL_DISABLE_LOCK_KEYS=1"));
#endif
    // Don't init timers, GetTicks seems to work fine and they can use a fair amount of power (Macs again) 
    // Please report problems with audio and other things.
    if ( SDL_Init( SDL_INIT_AUDIO|SDL_INIT_VIDEO /*| SDL_INIT_TIMER*/
        |SDL_INIT_NOPARACHUTE
        ) < 0 ) E_Exit("Can't init SDL %s",SDL_GetError());
    sdl.inited = true;

#ifndef DISABLE_JOYSTICK
    //Initialise Joystick separately. This way we can warn when it fails instead
    //of exiting the application
    if( SDL_InitSubSystem(SDL_INIT_JOYSTICK) < 0 ) LOG_MSG("Failed to init joystick support");
#endif

    sdl.laltstate = SDL_KEYUP;
    sdl.raltstate = SDL_KEYUP;

    sdl.num_joysticks=SDL_NumJoysticks();

    /* Parse configuration files */
    std::string config_file,config_path;
    bool parsed_anyconfigfile = false;
    //First Parse -userconf
    if(control->cmdline->FindExist("-userconf",true)){
        config_file.clear();
        Cross::GetPlatformConfigDir(config_path);
        Cross::GetPlatformConfigName(config_file);
        config_path += config_file;
        if(control->ParseConfigFile(config_path.c_str())) parsed_anyconfigfile = true;
        if(!parsed_anyconfigfile) {
            //Try to create the userlevel configfile.
            config_file.clear();
            Cross::CreatePlatformConfigDir(config_path);
            Cross::GetPlatformConfigName(config_file);
            config_path += config_file;
            if(control->PrintConfig(config_path.c_str())) {
                LOG_MSG("CONFIG: Generating default configuration.\nWriting it to %s",config_path.c_str());
                //Load them as well. Makes relative paths much easier
                if(control->ParseConfigFile(config_path.c_str())) parsed_anyconfigfile = true;
            }
        }
    }

    //Second parse -conf entries
    while(control->cmdline->FindString("-conf",config_file,true))
        if (control->ParseConfigFile(config_file.c_str())) parsed_anyconfigfile = true;

    //if none found => parse localdir conf
    config_file = "dosbox.conf";
    if (!parsed_anyconfigfile && control->ParseConfigFile(config_file.c_str())) parsed_anyconfigfile = true;

    //if none found => parse userlevel conf
    if(!parsed_anyconfigfile) {
        config_file.clear();
        Cross::GetPlatformConfigDir(config_path);
        Cross::GetPlatformConfigName(config_file);
        config_path += config_file;
        if(control->ParseConfigFile(config_path.c_str())) parsed_anyconfigfile = true;
    }

    if(!parsed_anyconfigfile) {
        //Try to create the userlevel configfile.
        config_file.clear();
        Cross::CreatePlatformConfigDir(config_path);
        Cross::GetPlatformConfigName(config_file);
        config_path += config_file;
        if(control->PrintConfig(config_path.c_str())) {
            LOG_MSG("CONFIG: Generating default configuration.\nWriting it to %s",config_path.c_str());
            //Load them as well. Makes relative paths much easier
            control->ParseConfigFile(config_path.c_str());
        } else {
            LOG_MSG("CONFIG: Using default settings. Create a configfile to change them");
        }
    }


#if (ENVIRON_LINKED)
        control->ParseEnv(environ);
#endif
//        UI_Init();
//        if (control->cmdline->FindExist("-startui")) UI_Run(false);
        /* Init all the sections */
        control->Init();
        /* Some extra SDL Functions */
        Section_prop * sdl_sec=static_cast<Section_prop *>(control->GetSection("sdl"));

        /* Init the keyMapper */
        MAPPER_Init();
        /* Start up main machine */
        control->StartUp();
        /* Shutdown everything */
    } catch (char * error) {
        GFX_ShowMsg("Exit to error: %s",error);
        fflush(NULL);
        if(sdl.wait_on_error) {
            //TODO Maybe look for some way to show message in linux?
#if (C_DEBUG)
            GFX_ShowMsg("Press enter to continue");
            fflush(NULL);
            fgetc(stdin);
#elif defined(WIN32)
            Sleep(5000);
#endif
        }

    }
    catch (int){
        ;//nothing pressed killswitch
    }
    catch(...){
        //Force visible mouse to end user. Somehow this sometimes doesn't happen
        SDL_SetWindowGrab(sdl.window, SDL_FALSE);
        SDL_ShowCursor(SDL_ENABLE);
        throw;//dunno what happened. rethrow for sdl to catch
    }
    //Force visible mouse to end user. Somehow this sometimes doesn't happen
    SDL_SetWindowGrab(sdl.window, SDL_FALSE);
    SDL_ShowCursor(SDL_ENABLE);

    GFX_Destroy();
    
    SDL_Quit();//Let's hope sdl will quit as well when it catches an exception
    return 0;
}

void GFX_GetSize(int &width, int &height, bool &fullscreen) {
    width = sdl.draw.width;
    height = sdl.draw.height;
    fullscreen = sdl.desktop.fullscreen;
}
